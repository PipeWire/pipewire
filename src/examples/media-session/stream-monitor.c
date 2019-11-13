/* PipeWire
 *
 * Copyright © 2019 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include "config.h"

#include <spa/node/node.h>
#include <spa/utils/hook.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/pod.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"
#include "extensions/session-manager.h"

#define NAME "stream-monitor"

#define DEFAULT_CHANNELS	2
#define DEFAULT_SAMPLERATE	48000

struct client_endpoint;

struct impl {
	struct timespec now;

	struct pw_core *core;
	struct pw_remote *remote;
	struct spa_hook remote_listener;

	uint32_t session_id;

	struct pw_core_proxy *core_proxy;
	struct spa_hook core_listener;

	struct pw_registry_proxy *registry_proxy;
	struct spa_hook registry_listener;

	struct pw_map globals;

	struct spa_list client_list;
	struct spa_list node_list;
	int seq;
};

struct object {
	struct impl *impl;
	uint32_t id;
	uint32_t type;
	struct pw_proxy *proxy;
	struct spa_hook listener;
};

struct node {
	struct object obj;

	struct spa_list l;
	struct client_endpoint *endpoint;

	struct spa_hook listener;
	struct pw_node_info *info;

	struct spa_list port_list;

	enum pw_direction direction;
#define NODE_TYPE_UNKNOWN	0
#define NODE_TYPE_STREAM	1
	uint32_t type;
	char *media;

	uint32_t media_type;
	uint32_t media_subtype;
	struct spa_audio_info_raw format;
};

struct endpoint {
	struct object obj;
};

struct port {
	struct object obj;

	struct spa_list l;
	enum pw_direction direction;
	struct pw_port_info *info;
	struct node *node;
#define PORT_FLAG_NONE		0
#define PORT_FLAG_DSP		(1<<0)
#define PORT_FLAG_SKIP		(1<<1)
	uint32_t flags;

	struct spa_hook listener;
};

struct stream {
	struct pw_properties *props;
	struct pw_endpoint_stream_info info;

	unsigned int active:1;
};

struct client_endpoint {
	struct spa_list link;

	struct impl *impl;

	struct pw_properties *props;
	struct node *node;

	struct pw_client_endpoint_proxy *client_endpoint;
	struct spa_hook client_endpoint_listener;
	struct pw_endpoint_info info;

	struct stream stream;
};

static void add_object(struct impl *impl, struct object *obj)
{
	size_t size = pw_map_get_size(&impl->globals);
        while (obj->id > size)
                pw_map_insert_at(&impl->globals, size++, NULL);
        pw_map_insert_at(&impl->globals, obj->id, obj);
}

static void remove_object(struct impl *impl, struct object *obj)
{
        pw_map_insert_at(&impl->globals, obj->id, NULL);
}

static void *find_object(struct impl *impl, uint32_t id)
{
	void *obj;
	if ((obj = pw_map_lookup(&impl->globals, id)) != NULL)
		return obj;
	return NULL;
}

static int client_endpoint_set_id(void *object, uint32_t id)
{
	struct client_endpoint *endpoint = object;

	endpoint->info.id = id;

	pw_client_endpoint_proxy_update(endpoint->client_endpoint,
			PW_CLIENT_ENDPOINT_UPDATE_INFO,
			0, NULL,
			&endpoint->info);
	return 0;
}

static int client_endpoint_set_session_id(void *object, uint32_t id)
{
	struct client_endpoint *endpoint = object;
	endpoint->info.session_id = id;
	return 0;
}

static int client_endpoint_set_param(void *object,
		uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	return -ENOTSUP;
}


static int client_endpoint_stream_set_param(void *object, uint32_t stream_id,
		uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	return -ENOTSUP;
}

static int client_endpoint_create_link(void *object, const struct spa_dict *props)
{
	struct client_endpoint *endpoint = object;
	struct impl *impl = endpoint->impl;
	const char *str;
	struct endpoint *ep;
	struct node *node = endpoint->node;
	struct pw_properties *p;
	int res;

	pw_log_debug("create link");

	if (props == NULL)
		return -EINVAL;

	p = pw_properties_new(NULL, NULL);
	if (p == NULL)
		return -errno;

	if (endpoint->info.direction == PW_DIRECTION_OUTPUT) {
		pw_properties_setf(p, PW_KEY_LINK_OUTPUT_NODE, "%d", endpoint->node->info->id);
		pw_properties_setf(p, PW_KEY_LINK_OUTPUT_PORT, "-1");
		str = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
	} else {
		pw_properties_setf(p, PW_KEY_LINK_INPUT_NODE, "%d", endpoint->node->info->id);
		pw_properties_setf(p, PW_KEY_LINK_INPUT_PORT, "-1");
		str = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
	}

	if (!endpoint->stream.active) {
		char buf[1024];
		struct spa_pod_builder b = { 0, };
		struct spa_pod *param;

		node->format.rate = 48000;

		spa_pod_builder_init(&b, buf, sizeof(buf));
		param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &node->format);
		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
			SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(endpoint->info.direction),
			SPA_PARAM_PORT_CONFIG_mode,	 SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp),
			SPA_PARAM_PORT_CONFIG_monitor,   SPA_POD_Bool(true),
			SPA_PARAM_PORT_CONFIG_format,    SPA_POD_Pod(param));

		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_pod(2, NULL, param);

		pw_node_proxy_set_param((struct pw_node_proxy*)node->obj.proxy,
				SPA_PARAM_PortConfig, 0, param);

		endpoint->stream.active = true;
	}

	str = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
	if (str == NULL) {
		res = -EINVAL;
		goto exit;
	}

	ep = find_object(impl, atoi(str));
	if (ep == NULL) {
		res = -EINVAL;
		goto exit;
	}

	pw_endpoint_proxy_create_link((struct pw_endpoint_proxy*)ep->obj.proxy, &p->dict);

	res = 0;

exit:
	pw_properties_free(p);

	return res;
}

static const struct pw_client_endpoint_proxy_events client_endpoint_events = {
	PW_VERSION_CLIENT_ENDPOINT_PROXY_EVENTS,
	.set_id = client_endpoint_set_id,
	.set_session_id = client_endpoint_set_session_id,
	.set_param = client_endpoint_set_param,
	.stream_set_param = client_endpoint_stream_set_param,
	.create_link = client_endpoint_create_link,
};

static struct client_endpoint *make_endpoint(struct node *node)
{
	struct impl *impl = node->obj.impl;
	struct pw_properties *props;
	struct client_endpoint *endpoint;
	struct stream *s;
	struct pw_proxy *proxy;
	const char *str, *media_class = NULL, *name = NULL;
	struct spa_dict *dict = node->info->props;

	props = pw_properties_new(NULL, NULL);
	if (props == NULL)
		return NULL;

	if (node->info && node->info->props) {
		if ((media_class = spa_dict_lookup(dict, PW_KEY_MEDIA_CLASS)) != NULL)
			pw_properties_set(props, PW_KEY_MEDIA_CLASS, media_class);
		if ((name = spa_dict_lookup(dict, PW_KEY_MEDIA_NAME)) != NULL)
			pw_properties_set(props, PW_KEY_ENDPOINT_NAME, name);
		if ((str = spa_dict_lookup(dict, PW_KEY_NODE_AUTOCONNECT)) != NULL)
			pw_properties_set(props, PW_KEY_ENDPOINT_AUTOCONNECT, str);
	}

	proxy = pw_core_proxy_create_object(impl->core_proxy,
						"client-endpoint",
						PW_TYPE_INTERFACE_ClientEndpoint,
						PW_VERSION_CLIENT_ENDPOINT_PROXY,
						&props->dict, sizeof(*endpoint));
	if (proxy == NULL) {
		pw_properties_free(props);
		return NULL;
	}

	endpoint = pw_proxy_get_user_data(proxy);
	endpoint->impl = impl;
	endpoint->node = node;
	endpoint->props = props;
	endpoint->client_endpoint = (struct pw_client_endpoint_proxy *) proxy;
	endpoint->info.version = PW_VERSION_ENDPOINT_INFO;
	endpoint->info.name = (char*)pw_properties_get(endpoint->props, PW_KEY_ENDPOINT_NAME);
	endpoint->info.media_class = (char*)spa_dict_lookup(node->info->props, PW_KEY_MEDIA_CLASS);
	endpoint->info.session_id = impl->session_id;
	endpoint->info.direction = node->direction;
	endpoint->info.flags = 0;
	endpoint->info.change_mask =
		PW_ENDPOINT_CHANGE_MASK_STREAMS |
		PW_ENDPOINT_CHANGE_MASK_SESSION |
		PW_ENDPOINT_CHANGE_MASK_PROPS;
	endpoint->info.n_streams = 1;
	endpoint->info.props = &endpoint->props->dict;

	pw_client_endpoint_proxy_add_listener(endpoint->client_endpoint,
			&endpoint->client_endpoint_listener,
			&client_endpoint_events,
			endpoint);

	s = &endpoint->stream;
	s->props = pw_properties_new(NULL, NULL);
	if ((str = spa_dict_lookup(dict, PW_KEY_MEDIA_CLASS)) != NULL)
		pw_properties_set(s->props, PW_KEY_MEDIA_CLASS, str);
	if (node->direction == PW_DIRECTION_OUTPUT)
		pw_properties_set(s->props, PW_KEY_STREAM_NAME, "Playback");
	else
		pw_properties_set(s->props, PW_KEY_STREAM_NAME, "Capture");

	s->info.version = PW_VERSION_ENDPOINT_STREAM_INFO;
	s->info.id = 0;
	s->info.endpoint_id = endpoint->info.id;
	s->info.name = (char*)pw_properties_get(s->props, PW_KEY_STREAM_NAME);
	s->info.change_mask = PW_ENDPOINT_STREAM_CHANGE_MASK_PROPS;
	s->info.props = &s->props->dict;

	pw_log_debug("stream %d", node->obj.id);
	pw_client_endpoint_proxy_stream_update(endpoint->client_endpoint,
			s->info.id,
			PW_CLIENT_ENDPOINT_STREAM_UPDATE_INFO,
			0, NULL,
			&s->info);

	return endpoint;
}
static void destroy_endpoint(struct client_endpoint *endpoint)
{
	pw_proxy_destroy((struct pw_proxy*)endpoint->client_endpoint);
}

static void node_event_info(void *object, const struct pw_node_info *info)
{
	struct node *n = object;
	struct impl *impl = n->obj.impl;

	pw_log_debug(NAME" %p: info for node %d type %d", impl, n->obj.id, n->type);
	n->info = pw_node_info_update(n->info, info);
}

static void node_event_param(void *object, int seq,
                       uint32_t id, uint32_t index, uint32_t next,
                       const struct spa_pod *param)
{
	struct node *n = object;
	struct impl *impl = n->obj.impl;
	struct spa_audio_info_raw info = { 0, };

	pw_log_debug(NAME" %p: param for node %d, %d", impl, n->obj.id, id);

	if (id != SPA_PARAM_EnumFormat)
		goto error;

	if (spa_format_parse(param, &n->media_type, &n->media_subtype) < 0)
		goto error;

	if (n->media_type != SPA_MEDIA_TYPE_audio ||
	    n->media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;

	spa_pod_object_fixate((struct spa_pod_object*)param);
	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_pod(2, NULL, param);

	if (spa_format_audio_raw_parse(param, &info) < 0)
		goto error;

	if (n->format.channels < info.channels)
		n->format = info;

	if (n->endpoint == NULL) {
		n->endpoint = make_endpoint(n);
	}
	return;

      error:
	pw_log_warn("unhandled param:");
	if (pw_log_level_enabled(SPA_LOG_LEVEL_WARN))
		spa_debug_pod(2, NULL, param);
	return;
}

static const struct pw_node_proxy_events node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
	.info = node_event_info,
	.param = node_event_param,
};

static void node_proxy_destroy(void *data)
{
	struct node *n = data;
	struct impl *impl = n->obj.impl;
	struct port *p, *t;

	pw_log_debug(NAME " %p: proxy destroy node %d", impl, n->obj.id);

	spa_list_remove(&n->l);

	spa_list_for_each_safe(p, t, &n->port_list, l) {
		spa_list_remove(&p->l);
		p->node = NULL;
	}
	if (n->info)
		pw_node_info_free(n->info);
	if (n->endpoint)
		destroy_endpoint(n->endpoint);

	free(n->media);
}

static const struct pw_proxy_events node_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = node_proxy_destroy,
};

static int
handle_node(struct impl *impl, uint32_t id,
		uint32_t type, const struct spa_dict *props)
{
	const char *media_class;
	enum pw_direction direction;
	struct pw_proxy *p;
	struct node *node;

	media_class = props ? spa_dict_lookup(props, PW_KEY_MEDIA_CLASS) : NULL;

	pw_log_debug(NAME" %p: node "PW_KEY_MEDIA_CLASS" %s", impl, media_class);

	if (media_class == NULL)
		return 0;

	if (strstr(media_class, "Stream/") != media_class)
		return 0;

	media_class += strlen("Stream/");

	if (strstr(media_class, "Output/") == media_class) {
		direction = PW_DIRECTION_OUTPUT;
		media_class += strlen("Output/");
	}
	else if (strstr(media_class, "Input/") == media_class) {
		direction = PW_DIRECTION_INPUT;
		media_class += strlen("Input/");
	}
	else
		return 0;

	p = pw_registry_proxy_bind(impl->registry_proxy,
			id, type, PW_VERSION_NODE_PROXY,
			sizeof(struct node));

	node = pw_proxy_get_user_data(p);
	node->obj.impl = impl;
	node->obj.id = id;
	node->obj.type = type;
	node->obj.proxy = p;
	spa_list_init(&node->port_list);
	pw_proxy_add_listener(p, &node->obj.listener, &node_proxy_events, node);
	pw_proxy_add_object_listener(p, &node->listener, &node_events, node);
	add_object(impl, &node->obj);
	spa_list_append(&impl->node_list, &node->l);
	node->type = NODE_TYPE_UNKNOWN;

	node->direction = direction;
	node->type = NODE_TYPE_STREAM;
	node->media = strdup(media_class);
	pw_log_debug(NAME "%p: node %d is stream %s", impl, id, node->media);

	pw_node_proxy_enum_params((struct pw_node_proxy*)p,
				0, SPA_PARAM_EnumFormat,
				0, -1, NULL);
	return 1;
}

static void port_event_info(void *object, const struct pw_port_info *info)
{
	struct port *p = object;
	pw_log_debug(NAME" %p: info for port %d", p->obj.impl, p->obj.id);
	p->info = pw_port_info_update(p->info, info);
}

static const struct pw_port_proxy_events port_events = {
	PW_VERSION_PORT_PROXY_EVENTS,
	.info = port_event_info,
};

static void port_proxy_destroy(void *data)
{
	struct port *p = data;

	pw_log_debug(NAME " %p: proxy destroy port %d", p->obj.impl, p->obj.id);

	if (p->node) {
		spa_list_remove(&p->l);
		p->node = NULL;
	}
	if (p->info)
		pw_port_info_free(p->info);
}

static const struct pw_proxy_events port_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = port_proxy_destroy,
};

static int
handle_port(struct impl *impl, uint32_t id, uint32_t type,
		const struct spa_dict *props)
{
	struct port *port;
	struct pw_proxy *p;
	struct node *node;
	const char *str;
	uint32_t node_id;

	if (props == NULL || (str = spa_dict_lookup(props, PW_KEY_NODE_ID)) == NULL)
		return -EINVAL;

	node_id = atoi(str);

	if ((node = find_object(impl, node_id)) == NULL)
		return 0;

	if (props == NULL || (str = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)) == NULL)
		return -EINVAL;

	p = pw_registry_proxy_bind(impl->registry_proxy,
			id, type, PW_VERSION_PORT_PROXY,
			sizeof(struct port));

	port = pw_proxy_get_user_data(p);
	port->obj.impl = impl;
	port->obj.id = id;
	port->obj.type = type;
	port->obj.proxy = p;
	port->node = node;
	port->direction = strcmp(str, "out") ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT;

	if (props != NULL && (str = spa_dict_lookup(props, PW_KEY_FORMAT_DSP)) != NULL)
		port->flags |= PORT_FLAG_DSP;

	pw_proxy_add_listener(p, &port->obj.listener, &port_proxy_events, port);
	pw_proxy_add_object_listener(p, &port->listener, &port_events, port);
	add_object(impl, &port->obj);

	spa_list_append(&node->port_list, &port->l);

	pw_log_debug(NAME" %p: new port %d for node %d type %d %08x", impl, id, node_id,
			node->type, port->flags);

	return 0;
}

static int
handle_endpoint(struct impl *impl, uint32_t id, uint32_t type,
		const struct spa_dict *props)
{
	struct endpoint *ep;
	struct pw_proxy *p;

	p = pw_registry_proxy_bind(impl->registry_proxy,
			id, type, PW_VERSION_ENDPOINT_PROXY,
			sizeof(struct endpoint));

	ep = pw_proxy_get_user_data(p);
	ep->obj.impl = impl;
	ep->obj.id = id;
	ep->obj.type = type;
	ep->obj.proxy = p;
	add_object(impl, &ep->obj);

	pw_log_debug(NAME" %p: new endpoint %d", impl, id);

	return 0;
}

static void
registry_global(void *data,uint32_t id,
		uint32_t permissions, uint32_t type, uint32_t version,
		const struct spa_dict *props)
{
	struct impl *impl = data;
	int res;

	pw_log_debug(NAME " %p: new global '%d' %d", impl, id, type);

	switch (type) {
	case PW_TYPE_INTERFACE_Node:
		res = handle_node(impl, id, type, props);
		break;

	case PW_TYPE_INTERFACE_Port:
		res = handle_port(impl, id, type, props);
		break;

	case PW_TYPE_INTERFACE_Endpoint:
		res = handle_endpoint(impl, id, type, props);
		break;

	default:
		res = 0;
		break;
	}
	if (res < 0) {
		pw_log_warn(NAME" %p: can't handle global %d: %s", impl, id, spa_strerror(res));
	}
}

static void
registry_global_remove(void *data, uint32_t id)
{
	struct impl *impl = data;
	struct object *obj;

	pw_log_debug(NAME " %p: remove global '%d'", impl, id);

	if ((obj = find_object(impl, id)) == NULL)
		return;

	remove_object(impl, obj);
}

static const struct pw_registry_proxy_events registry_events = {
	PW_VERSION_REGISTRY_PROXY_EVENTS,
        .global = registry_global,
        .global_remove = registry_global_remove,
};


#if 0
static void stream_set_volume(struct impl *impl, struct node *node, float volume, bool mute)
{
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

	pw_log_debug(NAME " %p: node %d set volume:%f mute:%d", impl, node->obj.id, volume, mute);

	pw_node_proxy_set_param((struct pw_node_proxy*)node->obj.proxy,
			SPA_PARAM_Props, 0,
			spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
				SPA_PROP_volume,	SPA_POD_Float(volume),
				SPA_PROP_mute,		SPA_POD_Bool(mute)));
}

static void rescan_session(struct impl *impl, struct session *sess)
{
	struct node *node = sess->node;
	struct spa_audio_info_raw info = { 0, };
	uint8_t buf[1024];
	struct spa_pod_builder b = { 0, };
	struct spa_pod *param;

	if (!sess->starting)
		return;

	if (node->info->props == NULL) {
		pw_log_debug(NAME " %p: node %p has no properties", impl, node);
		return;
	}

	if (node->media_type != SPA_MEDIA_TYPE_audio ||
	    node->media_subtype != SPA_MEDIA_SUBTYPE_raw) {
		pw_log_debug(NAME " %p: node %p has no media type", impl, node);
		return;
	}

	info = node->format;
	info.rate = DEFAULT_SAMPLERATE;

	pw_log_debug(NAME" %p: setting profile for session %d %d", impl, sess->id, sess->direction);

	spa_pod_builder_init(&b, buf, sizeof(buf));
	param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info);
	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction,	SPA_POD_Id(pw_direction_reverse(sess->direction)),
		SPA_PARAM_PORT_CONFIG_mode,		SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp),
		SPA_PARAM_PORT_CONFIG_monitor,		SPA_POD_Bool(true),
		SPA_PARAM_PORT_CONFIG_format,		SPA_POD_Pod(param));

	pw_node_proxy_set_param((struct pw_node_proxy*)sess->node->obj.proxy,
			SPA_PARAM_PortConfig, 0, param);
	schedule_rescan(impl);

	sess->starting = false;
}
#endif

void * sm_stream_monitor_start(struct pw_remote *remote, int session_id)
{
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	impl->core = pw_remote_get_core(remote);
	impl->remote = remote;
	impl->session_id = session_id;

	pw_map_init(&impl->globals, 64, 64);

	spa_list_init(&impl->client_list);
	spa_list_init(&impl->node_list);

	impl->core_proxy = pw_remote_get_core_proxy(impl->remote);
	impl->registry_proxy = pw_core_proxy_get_registry(impl->core_proxy,
					PW_VERSION_REGISTRY_PROXY, 0);
	pw_registry_proxy_add_listener(impl->registry_proxy,
					&impl->registry_listener,
					&registry_events, impl);

	return impl;
}

int sm_stream_monitor_stop(struct impl *impl)
{
	return 0;
}
