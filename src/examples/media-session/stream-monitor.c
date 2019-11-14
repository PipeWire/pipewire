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
#include "media-session.h"

#define NAME "stream-monitor"

#define DEFAULT_CHANNELS	2
#define DEFAULT_SAMPLERATE	48000

struct client_endpoint;

struct impl {
	struct sm_media_session *session;
	struct spa_hook listener;

	int seq;
};

struct node {
	struct sm_node *obj;

	struct impl *impl;

	struct spa_hook proxy_listener;
	struct spa_hook listener;

	uint32_t id;
	enum pw_direction direction;
	char *media;

	struct client_endpoint *endpoint;

	uint32_t media_type;
	uint32_t media_subtype;
	struct spa_audio_info_raw format;
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
	uint32_t pending_config;
};

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
	struct sm_object *obj;
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
		pw_properties_setf(p, PW_KEY_LINK_OUTPUT_NODE, "%d", endpoint->node->id);
		pw_properties_setf(p, PW_KEY_LINK_OUTPUT_PORT, "-1");
		str = spa_dict_lookup(props, PW_KEY_LINK_INPUT_NODE);
	} else {
		pw_properties_setf(p, PW_KEY_LINK_INPUT_NODE, "%d", endpoint->node->id);
		pw_properties_setf(p, PW_KEY_LINK_INPUT_PORT, "-1");
		str = spa_dict_lookup(props, PW_KEY_LINK_OUTPUT_NODE);
	}
	if (str == NULL) {
		pw_log_warn(NAME" %p: no target endpoint given", impl);
		res = -EINVAL;
		goto exit;
	}
	obj = sm_media_session_find_object(impl->session, atoi(str));
	if (obj == NULL || obj->type != PW_TYPE_INTERFACE_Endpoint) {
		pw_log_warn(NAME" %p: could not find object %s (%p)", impl, str, obj);
		res = -EINVAL;
		goto exit;
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

		pw_node_proxy_set_param((struct pw_node_proxy*)node->obj->obj.proxy,
				SPA_PARAM_PortConfig, 0, param);

		endpoint->pending_config = pw_proxy_sync(node->obj->obj.proxy, 0);

		endpoint->stream.active = true;
	}

	pw_endpoint_proxy_create_link((struct pw_endpoint_proxy*)obj->proxy, &p->dict);

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
	struct impl *impl = node->impl;
	struct pw_properties *props;
	struct client_endpoint *endpoint;
	struct stream *s;
	struct pw_proxy *proxy;
	const char *str, *media_class = NULL, *name = NULL;

	props = pw_properties_new(NULL, NULL);
	if (props == NULL)
		return NULL;

	if (node->obj->info && node->obj->info->props) {
		struct spa_dict *dict = node->obj->info->props;
		if ((media_class = spa_dict_lookup(dict, PW_KEY_MEDIA_CLASS)) != NULL)
			pw_properties_set(props, PW_KEY_MEDIA_CLASS, media_class);
		if ((name = spa_dict_lookup(dict, PW_KEY_MEDIA_NAME)) != NULL)
			pw_properties_set(props, PW_KEY_ENDPOINT_NAME, name);
		if ((str = spa_dict_lookup(dict, PW_KEY_NODE_AUTOCONNECT)) != NULL)
			pw_properties_set(props, PW_KEY_ENDPOINT_AUTOCONNECT, str);
	}

	proxy = sm_media_session_create_object(impl->session,
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
	endpoint->info.name = (char*)pw_properties_get(props, PW_KEY_ENDPOINT_NAME);
	endpoint->info.media_class = (char*)pw_properties_get(props, PW_KEY_MEDIA_CLASS);
	endpoint->info.session_id = impl->session->info.id;
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
	if ((str = pw_properties_get(props, PW_KEY_MEDIA_CLASS)) != NULL)
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

	pw_log_debug("stream %d", node->id);
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

static void node_event_param(void *object, int seq,
                       uint32_t id, uint32_t index, uint32_t next,
                       const struct spa_pod *param)
{
	struct node *n = object;
	struct impl *impl = n->impl;
	struct spa_audio_info_raw info = { 0, };

	pw_log_debug(NAME" %p: param for node %d, %d", impl, n->id, id);

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
	.param = node_event_param,
};

static void node_proxy_destroy(void *data)
{
	struct node *n = data;
	struct impl *impl = n->impl;

	pw_log_debug(NAME " %p: proxy destroy node %d", impl, n->id);

	if (n->endpoint)
		destroy_endpoint(n->endpoint);
	free(n->media);
}

static void node_proxy_done(void *data, int seq)
{
	struct node *n = data;
	struct impl *impl = n->impl;
	struct client_endpoint *endpoint = n->endpoint;

	if (endpoint == NULL)
		return;
	if (endpoint->pending_config != 0) {
		pw_log_debug(NAME" %p: config complete", impl);
		endpoint->pending_config = 0;
	}
}

static void node_proxy_error(void *data, int seq, int res, const char *message)
{
	struct node *n = data;
	struct impl *impl = n->impl;
	pw_log_error(NAME " %p: proxy seq:%d got error %d: %s", impl, seq, res, message);
}

static const struct pw_proxy_events node_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = node_proxy_destroy,
	.done = node_proxy_done,
	.error = node_proxy_error,
};

static int
handle_node(struct impl *impl, struct sm_object *obj)
{
	const char *media_class;
	enum pw_direction direction;
	struct node *node;

	if (sm_object_get_data(obj, "stream-monitor") != NULL)
		return 0;

	media_class = obj->props ? pw_properties_get(obj->props, PW_KEY_MEDIA_CLASS) : NULL;

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

	node = sm_object_add_data(obj, "stream-monitor", sizeof(struct node));
	node->obj = (struct sm_node*)obj;
	node->impl = impl;
	node->id = obj->id;
	node->direction = direction;
	node->media = strdup(media_class);
	pw_log_debug(NAME "%p: node %d is stream %s", impl, node->id, node->media);

	pw_proxy_add_listener(obj->proxy, &node->proxy_listener, &node_proxy_events, node);
	pw_proxy_add_object_listener(obj->proxy, &node->listener, &node_events, node);

	pw_node_proxy_enum_params((struct pw_node_proxy*)obj->proxy,
				0, SPA_PARAM_EnumFormat,
				0, -1, NULL);
	return 1;
}

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

static void session_update(void *data, struct sm_object *object)
{
	struct impl *impl = data;
	int res;

	pw_log_debug(NAME " %p: update object '%d' %d", impl, object->id, object->type);

	switch (object->type) {
	case PW_TYPE_INTERFACE_Node:
		res = handle_node(impl, object);
		break;

	default:
		res = 0;
		break;
	}
	if (res < 0) {
		pw_log_warn(NAME" %p: can't handle global %d: %s", impl,
				object->id, spa_strerror(res));
	}
}

static void session_remove(void *data, struct sm_object *object)
{
}

static const struct sm_media_session_events session_events = {
	SM_VERSION_MEDIA_SESSION_EVENTS,
	.update = session_update,
	.remove = session_remove,
};

void * sm_stream_monitor_start(struct sm_media_session *session)
{
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	impl->session = session;
	sm_media_session_add_listener(session, &impl->listener, &session_events, impl);

	return impl;
}

int sm_stream_monitor_stop(struct impl *impl)
{
	spa_hook_remove(&impl->listener);
	return 0;
}
