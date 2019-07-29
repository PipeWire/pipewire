/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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

#define NAME "media-session"

#define DEFAULT_CHANNELS	2
#define DEFAULT_SAMPLERATE	48000

#define DEFAULT_IDLE_SECONDS	3

#define MIN_QUANTUM_SIZE	64
#define MAX_QUANTUM_SIZE	1024

struct impl;

struct monitor {
	struct impl *impl;

	struct spa_handle *handle;
	struct spa_monitor *monitor;

	struct spa_list object_list;
};

struct impl {
	struct timespec now;

	struct pw_main_loop *loop;
	struct pw_core *core;
	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_core_proxy *core_proxy;
	struct spa_hook core_listener;

	struct pw_registry_proxy *registry_proxy;
	struct spa_hook registry_listener;

	struct pw_map globals;

	struct spa_list client_list;
	struct spa_list node_list;
	struct spa_list session_list;
	int seq;

	struct monitor bluez5_monitor;
	struct monitor alsa_monitor;
	struct monitor v4l2_monitor;
};

struct object {
	struct impl *impl;
	uint32_t id;
	uint32_t parent_id;
	uint32_t type;
	struct pw_proxy *proxy;
	struct spa_hook listener;
};

struct client {
	struct object obj;

	struct spa_list l;

	struct spa_hook listener;
	struct pw_client_info *info;
};

struct node {
	struct object obj;

	struct spa_list l;

	struct spa_hook listener;
	struct pw_node_info *info;

	struct spa_list session_link;
	struct session *session;

	struct session *manager;
	struct spa_list port_list;

	enum pw_direction direction;
#define NODE_TYPE_UNKNOWN	0
#define NODE_TYPE_STREAM	1
#define NODE_TYPE_DEVICE	2
	uint32_t type;
	char *media;

	uint32_t media_type;
	uint32_t media_subtype;
	struct spa_audio_info_raw format;
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

struct link {
	struct object obj;
	struct port *out;
	struct port *in;
};

struct session {
	struct spa_list l;

	uint32_t id;

	struct impl *impl;
	enum pw_direction direction;
	uint64_t plugged;

	struct node *node;

	struct spa_list node_list;

	struct spa_hook listener;

	struct spa_source *idle_timeout;

	bool starting;
	bool enabled;
	bool busy;
	bool exclusive;
	bool need_dsp;
};

#include "alsa-monitor.c"
#include "v4l2-monitor.c"
#include "bluez-monitor.c"

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

static void schedule_rescan(struct impl *impl)
{
	if (impl->core_proxy)
		impl->seq = pw_core_proxy_sync(impl->core_proxy, 0, impl->seq);
}

static void remove_idle_timeout(struct session *sess)
{
	struct impl *impl = sess->impl;
	struct pw_loop *main_loop = pw_core_get_main_loop(impl->core);

	if (sess->idle_timeout) {
		pw_loop_destroy_source(main_loop, sess->idle_timeout);
		sess->idle_timeout = NULL;
	}
}

static void idle_timeout(void *data, uint64_t expirations)
{
	struct session *sess = data;
	struct impl *impl = sess->impl;
	struct spa_command *cmd = &SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Suspend);

	pw_log_debug(NAME " %p: session %d idle timeout", impl, sess->id);

	remove_idle_timeout(sess);

	pw_node_proxy_send_command((struct pw_node_proxy*)sess->node->obj.proxy, cmd);
}

static void add_idle_timeout(struct session *sess)
{
	struct timespec value;
	struct impl *impl = sess->impl;
	struct pw_loop *main_loop = pw_core_get_main_loop(impl->core);

	if (sess->idle_timeout == NULL)
		sess->idle_timeout = pw_loop_add_timer(main_loop, idle_timeout, sess);

	value.tv_sec = DEFAULT_IDLE_SECONDS;
	value.tv_nsec = 0;
	pw_loop_update_timer(main_loop, sess->idle_timeout, &value, NULL, false);
}

static int on_node_idle(struct impl *impl, struct node *node)
{
	struct session *sess = node->manager;

	if (sess == NULL)
		return 0;

	switch (node->type) {
	case NODE_TYPE_DEVICE:
		pw_log_debug(NAME" %p: device idle for session %d", impl, sess->id);
		sess->busy = false;
		sess->exclusive = false;
		add_idle_timeout(sess);
		break;
	default:
		break;
	}
	return 0;
}

static int on_node_running(struct impl *impl, struct node *node)
{
	struct session *sess = node->manager;

	if (sess == NULL)
		return 0;

	switch (node->type) {
	case NODE_TYPE_DEVICE:
		pw_log_debug(NAME" %p: device running or session %d", impl, sess->id);
		remove_idle_timeout(sess);
		break;
	default:
		break;
	}
	return 0;
}

static void node_event_info(void *object, const struct pw_node_info *info)
{
	struct node *n = object;
	struct impl *impl = n->obj.impl;

	pw_log_debug(NAME" %p: info for node %d type %d", impl, n->obj.id, n->type);
	n->info = pw_node_info_update(n->info, info);

	switch (info->state) {
	case PW_NODE_STATE_IDLE:
		on_node_idle(impl, n);
		break;
	case PW_NODE_STATE_RUNNING:
		on_node_running(impl, n);
		break;
	case PW_NODE_STATE_SUSPENDED:
		break;
	default:
		break;
	}
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
	spa_debug_pod(2, NULL, param);

	if (spa_format_audio_raw_parse(param, &info) < 0)
		goto error;

	n->format = info;
	return;

      error:
	pw_log_warn("unhandled param:");
	spa_debug_pod(2, NULL, param);
	return;
}

static const struct pw_node_proxy_events node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
	.info = node_event_info,
	.param = node_event_param,
};

static void remove_session(struct impl *impl, struct session *sess)
{
	struct node *n, *t;

	pw_log_debug(NAME " %p: remove session '%d'", impl, sess->id);
	remove_idle_timeout(sess);

	spa_list_for_each_safe(n, t, &sess->node_list, session_link) {
		n->session = NULL;
		spa_list_remove(&n->session_link);
	}

	spa_list_remove(&sess->l);
	free(sess);
}

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
	free(n->media);
	if (n->session) {
		spa_list_remove(&n->session_link);
		n->session = NULL;
	}
	if (n->manager) {
		switch (n->type) {
		case NODE_TYPE_DEVICE:
			remove_session(impl, n->manager);
			n->manager = NULL;
			break;
		}
	}
}

static const struct pw_proxy_events node_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = node_proxy_destroy,
};

static int
handle_node(struct impl *impl, uint32_t id, uint32_t parent_id,
		uint32_t type, const struct spa_dict *props)
{
	const char *str, *media_class;
	bool need_dsp = false;
	enum pw_direction direction;
	struct pw_proxy *p;
	struct node *node;

	media_class = props ? spa_dict_lookup(props, PW_KEY_MEDIA_CLASS) : NULL;

	p = pw_registry_proxy_bind(impl->registry_proxy,
			id, type, PW_VERSION_NODE_PROXY,
			sizeof(struct node));

	node = pw_proxy_get_user_data(p);
	node->obj.impl = impl;
	node->obj.id = id;
	node->obj.parent_id = parent_id;
	node->obj.type = type;
	node->obj.proxy = p;
	spa_list_init(&node->port_list);
	pw_proxy_add_listener(p, &node->obj.listener, &node_proxy_events, node);
	pw_proxy_add_object_listener(p, &node->listener, &node_events, node);
	add_object(impl, &node->obj);
	spa_list_append(&impl->node_list, &node->l);
	node->type = NODE_TYPE_UNKNOWN;

	pw_log_debug(NAME" %p: node "PW_KEY_MEDIA_CLASS" %s", impl, media_class);

	if (media_class == NULL)
		return 0;

	if (strstr(media_class, "Stream/") == media_class) {
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

		node->direction = direction;
		node->type = NODE_TYPE_STREAM;
		node->media = strdup(media_class);
		pw_log_debug(NAME "%p: node %d is stream %s", impl, id, node->media);

	}
	else {
		struct session *sess;

		if (strstr(media_class, "Audio/") == media_class) {
			need_dsp = true;
			media_class += strlen("Audio/");
		}
		else if (strstr(media_class, "Video/") == media_class) {
			media_class += strlen("Video/");
		}
		else
			return 0;

		if (strcmp(media_class, "Sink") == 0)
			direction = PW_DIRECTION_OUTPUT;
		else if (strcmp(media_class, "Source") == 0)
			direction = PW_DIRECTION_INPUT;
		else
			return 0;

		sess = calloc(1, sizeof(struct session));
		sess->impl = impl;
		sess->direction = direction;
		sess->id = id;
		sess->need_dsp = need_dsp;
		sess->enabled = false;
		sess->starting = need_dsp;
		sess->node = node;
		if ((str = spa_dict_lookup(props, PW_KEY_NODE_PLUGGED)) != NULL)
			sess->plugged = pw_properties_parse_uint64(str);
		else
			sess->plugged = SPA_TIMESPEC_TO_NSEC(&impl->now);

		spa_list_init(&sess->node_list);
		spa_list_append(&impl->session_list, &sess->l);

		node->direction = direction;
		node->type = NODE_TYPE_DEVICE;
		node->manager = sess;

		pw_log_debug(NAME" %p: new session for device node %d %d", impl, id,
				need_dsp);
	}
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

static void port_event_param(void *object, int seq,
                       uint32_t id, uint32_t index, uint32_t next,
                       const struct spa_pod *param)
{
	struct port *p = object;
	struct node *node = p->node;
	struct spa_audio_info_raw info = { 0, };

	pw_log_debug(NAME" %p: param for port %d", p->obj.impl, p->obj.id);

	if (node == NULL)
		return;

	if (id != SPA_PARAM_EnumFormat)
		return;

	if (node->manager)
		node->manager->enabled = true;

	if (spa_format_parse(param, &node->media_type, &node->media_subtype) < 0)
		return;

	if (node->media_type != SPA_MEDIA_TYPE_audio ||
	    node->media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;

	spa_pod_fixate((struct spa_pod*)param);

	if (spa_format_audio_raw_parse(param, &info) < 0)
		return;

	if (info.channels > node->format.channels)
		node->format = info;
}

static const struct pw_port_proxy_events port_events = {
	PW_VERSION_PORT_PROXY_EVENTS,
	.info = port_event_info,
	.param = port_event_param,
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
handle_port(struct impl *impl, uint32_t id, uint32_t parent_id, uint32_t type,
		const struct spa_dict *props)
{
	struct port *port;
	struct pw_proxy *p;
	struct node *node;
	const char *str;

	if ((node = find_object(impl, parent_id)) == NULL)
		return -ESRCH;

	if (props == NULL || (str = spa_dict_lookup(props, PW_KEY_PORT_DIRECTION)) == NULL)
		return -EINVAL;

	p = pw_registry_proxy_bind(impl->registry_proxy,
			id, type, PW_VERSION_PORT_PROXY,
			sizeof(struct port));

	port = pw_proxy_get_user_data(p);
	port->obj.impl = impl;
	port->obj.id = id;
	port->obj.parent_id = parent_id;
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

	pw_log_debug(NAME" %p: new port %d for node %d type %d %08x", impl, id, parent_id,
			node->type, port->flags);

	if (node->type == NODE_TYPE_DEVICE) {
		pw_port_proxy_enum_params((struct pw_port_proxy*)p,
				0, SPA_PARAM_EnumFormat,
				0, -1, NULL);
	}


	return 0;
}

static void client_event_info(void *object, const struct pw_client_info *info)
{
	struct client *c = object;
	uint32_t i;

	pw_log_debug(NAME" %p: info for client %d", c->obj.impl, c->obj.id);
	c->info = pw_client_info_update(c->info, info);
	for (i = 0; i < info->props->n_items; i++)
		pw_log_debug(NAME" %p:  %s = %s", c,
				info->props->items[i].key,
				info->props->items[i].value);
}

static const struct pw_client_proxy_events client_events = {
	PW_VERSION_CLIENT_PROXY_EVENTS,
	.info = client_event_info,
};

static void client_proxy_destroy(void *data)
{
	struct client *c = data;

	pw_log_debug(NAME " %p: proxy destroy client %d", c->obj.impl, c->obj.id);

	spa_list_remove(&c->l);
	if (c->info)
		pw_client_info_free(c->info);
}

static const struct pw_proxy_events client_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = client_proxy_destroy,
};

static int
handle_client(struct impl *impl, uint32_t id, uint32_t parent_id,
		uint32_t type, const struct spa_dict *props)
{
	struct pw_proxy *p;
	struct client *client;
	struct pw_permission perms[2];
	const char *str;

	p = pw_registry_proxy_bind(impl->registry_proxy,
			id, type, PW_VERSION_CLIENT_PROXY,
			sizeof(struct client));

	client = pw_proxy_get_user_data(p);
	client->obj.impl = impl;
	client->obj.id = id;
	client->obj.parent_id = parent_id;
	client->obj.type = type;
	client->obj.proxy = p;

	pw_proxy_add_listener(p, &client->obj.listener, &client_proxy_events, client);
	pw_proxy_add_object_listener(p, &client->listener, &client_events, client);
	add_object(impl, &client->obj);
	spa_list_append(&impl->client_list, &client->l);

	if (props == NULL)
		return 0;

	str = spa_dict_lookup(props, PW_KEY_ACCESS);
	if (str == NULL)
		return 0;

	if (strcmp(str, "restricted") == 0) {
		perms[0] = PW_PERMISSION_INIT(-1, PW_PERM_RWX);
		pw_client_proxy_update_permissions((struct pw_client_proxy*)p,
				1, perms);
	}
	return 0;
}

static void
registry_global(void *data,uint32_t id, uint32_t parent_id,
		uint32_t permissions, uint32_t type, uint32_t version,
		const struct spa_dict *props)
{
	struct impl *impl = data;
	int res;

	pw_log_debug(NAME " %p: new global '%d' %d", impl, id, type);

	switch (type) {
	case PW_TYPE_INTERFACE_Client:
		res = handle_client(impl, id, parent_id, type, props);
		break;

	case PW_TYPE_INTERFACE_Node:
		res = handle_node(impl, id, parent_id, type, props);
		break;

	case PW_TYPE_INTERFACE_Port:
		res = handle_port(impl, id, parent_id, type, props);
		break;

	default:
		res = 0;
		break;
	}
	if (res < 0) {
		pw_log_warn(NAME" %p: can't handle global %d", impl, id);
	}
	else
		schedule_rescan(impl);
}

static void
registry_global_remove(void *data, uint32_t id)
{
	struct impl *impl = data;
	struct object *obj;

	pw_log_debug(NAME " %p: remove global '%d'", impl, id);

	if ((obj = find_object(impl, id)) == NULL)
		return;

	switch (obj->type) {
	case PW_TYPE_INTERFACE_Node:
	{
		struct node *node = (struct node*) obj;
		if (node->manager)
			remove_session(impl, node->manager);
		node->manager = NULL;
		break;
	}
	default:
		break;
	}
	remove_object(impl, obj);
	schedule_rescan(impl);
}

static const struct pw_registry_proxy_events registry_events = {
	PW_VERSION_REGISTRY_PROXY_EVENTS,
        .global = registry_global,
        .global_remove = registry_global_remove,
};


struct find_data {
	struct impl *impl;
	uint32_t path_id;
	const char *media_class;
	struct session *sess;
	bool exclusive;
	uint64_t plugged;
};

static int find_session(void *data, struct session *sess)
{
	struct find_data *find = data;
	struct impl *impl = find->impl;
	const struct spa_dict *props;
	const char *str;
	uint64_t plugged = 0;

	pw_log_debug(NAME " %p: looking at session '%d' enabled:%d busy:%d exclusive:%d",
			impl, sess->id, sess->enabled, sess->busy, sess->exclusive);

	if (!sess->enabled)
		return 0;

	if (find->path_id != SPA_ID_INVALID && sess->id != find->path_id)
		return 0;

	if (find->path_id == SPA_ID_INVALID) {
		if ((props = sess->node->info->props) == NULL)
			return 0;

		if ((str = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS)) == NULL)
			return 0;

		if (strcmp(str, find->media_class) != 0)
			return 0;

		plugged = sess->plugged;
	}

	if ((find->exclusive && sess->busy) || sess->exclusive) {
		pw_log_debug(NAME " %p: session '%d' in use", impl, sess->id);
		return 0;
	}

	pw_log_debug(NAME " %p: found session '%d' %" PRIu64, impl,
			sess->id, plugged);

	if (find->sess == NULL || plugged > find->plugged) {
		pw_log_debug(NAME " %p: new best %" PRIu64, impl, plugged);
		find->sess = sess;
		find->plugged = plugged;
	}
	return 0;
}

static int link_nodes(struct node *peer, enum pw_direction direction, struct node *node, int max)
{
	struct impl *impl = peer->obj.impl;
	struct port *p;

	pw_log_debug(NAME " %p: link nodes %d %d %d", impl, max, node->obj.id, peer->obj.id);

	spa_list_for_each(p, &peer->port_list, l) {
		struct pw_properties *props;

		pw_log_debug(NAME " %p: port %p: %d %d", impl, p, p->direction, p->flags);

		if (p->direction == direction)
			continue;
		if (p->flags & PORT_FLAG_SKIP)
			continue;

		if (max-- == 0)
			return 0;

		props = pw_properties_new(NULL, NULL);
		if (p->direction == PW_DIRECTION_OUTPUT) {
			pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%d", node->obj.id);
			pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%d", -1);
			pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%d", peer->obj.id);
			pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%d", p->obj.id);
			pw_log_debug(NAME " %p: node %d -> port %d:%d", impl,
					node->obj.id, peer->obj.id, p->obj.id);

		}
		else {
			pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%d", peer->obj.id);
			pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%d", p->obj.id);
			pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%d", node->obj.id);
			pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%d", -1);
			pw_log_debug(NAME " %p: port %d:%d -> node %d", impl,
					peer->obj.id, p->obj.id, node->obj.id);
		}

		pw_core_proxy_create_object(impl->core_proxy,
                                          "link-factory",
                                          PW_TYPE_INTERFACE_Link,
                                          PW_VERSION_LINK_PROXY,
                                          &props->dict,
					  0);

		pw_properties_free(props);
	}
	return 0;
}

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

static int rescan_node(struct impl *impl, struct node *node)
{
	struct spa_dict *props;
        const char *str, *media, *category, *role;
        bool exclusive;
        struct find_data find;
	struct session *session;
	struct pw_node_info *info;
	struct node *peer;
	enum pw_direction direction;
	struct spa_pod_builder b = { 0, };
	struct spa_audio_info_raw audio_info = { 0, };
	struct spa_pod *param;
	char buf[1024];
	int n_links = 0;

	if (node->type == NODE_TYPE_DEVICE)
		return 0;

	if (node->session != NULL)
		return 0;

	if (node->info == NULL || node->info->props == NULL) {
		pw_log_debug(NAME " %p: node %d has no properties", impl, node->obj.id);
		return 0;
	}

	info = node->info;
	props = info->props;

        str = spa_dict_lookup(props, PW_KEY_NODE_AUTOCONNECT);
        if (str == NULL || !pw_properties_parse_bool(str)) {
		pw_log_debug(NAME" %p: node %d does not need autoconnect", impl, node->obj.id);
                return 0;
	}

	if ((media = spa_dict_lookup(props, PW_KEY_MEDIA_TYPE)) == NULL)
		media = node->media;
	if (media == NULL) {
		pw_log_debug(NAME" %p: node %d has unknown media", impl, node->obj.id);
		return 0;
	}

	if ((category = spa_dict_lookup(props, PW_KEY_MEDIA_CATEGORY)) == NULL) {
		pw_log_debug(NAME" %p: node %d find category from ports: %d %d",
			impl, node->obj.id, info->n_input_ports, info->n_output_ports);
		if (node->direction == PW_DIRECTION_INPUT ||
		    (info->n_input_ports > 0 && info->n_output_ports == 0))
			category = "Capture";
		else if (node->direction == PW_DIRECTION_OUTPUT ||
		    (info->n_output_ports > 0 && info->n_input_ports == 0))
			category = "Playback";
		else if (info->n_output_ports > 0 && info->n_input_ports > 0)
			category = "Duplex";
		else {
			pw_log_warn(NAME" %p: node %d can't determine category",
					impl, node->obj.id);
			return -EINVAL;
		}
	}

	if ((role = spa_dict_lookup(props, PW_KEY_MEDIA_ROLE)) == NULL) {
		if (strcmp(media, "Audio") == 0) {
			if (strcmp(category, "Duplex") == 0)
				role = "Communication";
			else if (strcmp(category, "Capture") == 0)
				role = "Production";
			else
				role = "Music";
		}
		else if (strcmp(media, "Video") == 0) {
			if (strcmp(category, "Duplex") == 0)
				role = "Communication";
			else if (strcmp(category, "Capture") == 0)
				role = "Camera";
			else
				role = "Video";
		}
	}

	if ((str = spa_dict_lookup(props, PW_KEY_NODE_EXCLUSIVE)) != NULL)
		exclusive = pw_properties_parse_bool(str);
	else
		exclusive = false;

	if (strcmp(media, "Audio") == 0) {
		if (strcmp(category, "Playback") == 0)
			find.media_class = "Audio/Sink";
		else if (strcmp(category, "Capture") == 0)
			find.media_class = "Audio/Source";
		else {
			pw_log_debug(NAME" %p: node %d unhandled category %s",
					impl, node->obj.id, category);
			return -EINVAL;
		}
	}
	else if (strcmp(media, "Video") == 0) {
		if (strcmp(category, "Capture") == 0)
			find.media_class = "Video/Source";
		else {
			pw_log_debug(NAME" %p: node %d unhandled category %s",
					impl, node->obj.id, category);
			return -EINVAL;
		}
	}
	else {
		pw_log_debug(NAME" %p: node %d unhandled media %s",
				impl, node->obj.id, media);
		return -EINVAL;
	}

	if (strcmp(category, "Capture") == 0)
		direction = PW_DIRECTION_OUTPUT;
	else if (strcmp(category, "Playback") == 0)
		direction = PW_DIRECTION_INPUT;
	else {
		pw_log_debug(NAME" %p: node %d unhandled category %s",
				impl, node->obj.id, category);
		return -EINVAL;
	}

	str = spa_dict_lookup(props, PW_KEY_NODE_TARGET);
	if (str != NULL)
		find.path_id = atoi(str);
	else
		find.path_id = SPA_ID_INVALID;

	pw_log_info(NAME " %p: '%s' '%s' '%s' exclusive:%d target %d", impl,
			media, category, role, exclusive, find.path_id);

	find.impl = impl;
	find.sess = NULL;
	find.plugged = 0;
	find.exclusive = exclusive;
	spa_list_for_each(session, &impl->session_list, l)
		find_session(&find, session);

	if (find.sess == NULL && find.path_id != SPA_ID_INVALID) {
		pw_log_debug(NAME " %p: no session found for %d, try node", impl, node->obj.id);

		n_links = 1;
		if ((peer = find_object(impl, find.path_id)) != NULL) {
			if (peer->obj.type == PW_TYPE_INTERFACE_Node) {
				if (peer->media_type == SPA_MEDIA_TYPE_audio)
					goto do_link_profile;
				else
					goto do_link;
			}
		}
		else {
			str = spa_dict_lookup(props, PW_KEY_NODE_DONT_RECONNECT);
			if (str != NULL && pw_properties_parse_bool(str)) {
				pw_registry_proxy_destroy(impl->registry_proxy, node->obj.id);
				return -ENOENT;
			}
		}
	}

	if (find.sess == NULL) {
		struct client *client;

		pw_log_warn(NAME " %p: no session found for %d", impl, node->obj.id);

		client = find_object(impl, node->obj.parent_id);
		if (client && client->obj.type == PW_TYPE_INTERFACE_Client) {
			pw_client_proxy_error((struct pw_client_proxy*)client->obj.proxy,
				node->obj.id, -ENOENT, "no session available");
		}
		return -ENOENT;
	}

	session = find.sess;

	if (session->starting) {
		pw_log_info(NAME " %p: session %d is starting", impl, session->id);
		return 0;
	}

	if (exclusive && session->busy) {
		pw_log_warn(NAME" %p: session %d busy, can't get exclusive access", impl, session->id);
		return -EBUSY;
	}
	peer = session->node;
	session->exclusive = exclusive;

	pw_log_debug(NAME" %p: linking to session '%d'", impl, session->id);

        session->busy = true;
	node->session = session;
	spa_list_append(&session->node_list, &node->session_link);

	if (!exclusive && peer->media_type == SPA_MEDIA_TYPE_audio) {
do_link_profile:
		audio_info = peer->format;

		if (direction == PW_DIRECTION_INPUT)
			audio_info.channels = SPA_MIN(audio_info.channels, node->format.channels);
		else
			audio_info.channels = SPA_MAX(audio_info.channels, node->format.channels);

		pw_log_debug(NAME" %p: channels: %d -> %d", impl,
				node->format.channels, audio_info.channels);

		audio_info.rate = DEFAULT_SAMPLERATE;

		spa_pod_builder_init(&b, buf, sizeof(buf));
		param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &audio_info);
		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
			SPA_PARAM_PROFILE_direction,  SPA_POD_Id(pw_direction_reverse(direction)),
			SPA_PARAM_PROFILE_format,     SPA_POD_Pod(param));

		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_pod(2, NULL, param);

		pw_node_proxy_set_param((struct pw_node_proxy*)node->obj.proxy,
				SPA_PARAM_Profile, 0, param);

		stream_set_volume(impl, node, 1.0, false);
		n_links = audio_info.channels;
	} else {
		n_links = audio_info.channels = 1;
	}
do_link:
	link_nodes(peer, direction, node, n_links);

        return 1;
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

	pw_log_debug(NAME" %p: setting profile for session %d", impl, sess->id);

	spa_pod_builder_init(&b, buf, sizeof(buf));
	param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info);
	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
		SPA_PARAM_PROFILE_direction,  SPA_POD_Id(pw_direction_reverse(sess->direction)),
		SPA_PARAM_PROFILE_format,     SPA_POD_Pod(param));

	pw_node_proxy_set_param((struct pw_node_proxy*)sess->node->obj.proxy,
			SPA_PARAM_Profile, 0, param);
	schedule_rescan(impl);

	sess->starting = false;
}

static void do_rescan(struct impl *impl)
{
	struct session *sess;
	struct node *node;

	clock_gettime(CLOCK_MONOTONIC, &impl->now);
	pw_log_debug("media-session %p: do rescan", impl);

	spa_list_for_each(sess, &impl->session_list, l)
		rescan_session(impl, sess);
	spa_list_for_each(node, &impl->node_list, l)
		rescan_node(impl, node);
}

static void core_done(void *data, uint32_t id, int seq)
{
	struct impl *impl = data;
	pw_log_debug("media-session %p: sync %u %d/%d", impl, id, seq, impl->seq);
	if (impl->seq == seq)
		do_rescan(impl);
}

static const struct pw_core_proxy_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = core_done
};

static void on_state_changed(void *_data, enum pw_remote_state old, enum pw_remote_state state, const char *error)
{
	struct impl *impl = _data;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		pw_log_error(NAME" %p: remote error: %s", impl, error);
		pw_main_loop_quit(impl->loop);
		break;

	case PW_REMOTE_STATE_CONNECTED:
		pw_log_info(NAME" %p: connected", impl);
		impl->core_proxy = pw_remote_get_core_proxy(impl->remote);
		pw_core_proxy_add_listener(impl->core_proxy,
					   &impl->core_listener,
					   &core_events, impl);
		impl->registry_proxy = pw_core_proxy_get_registry(impl->core_proxy,
                                                PW_VERSION_REGISTRY_PROXY, 0);
		pw_registry_proxy_add_listener(impl->registry_proxy,
                                               &impl->registry_listener,
                                               &registry_events, impl);
		bluez5_start_monitor(impl, &impl->bluez5_monitor);
		alsa_start_monitor(impl, &impl->alsa_monitor);
		v4l2_start_monitor(impl, &impl->v4l2_monitor);

		schedule_rescan(impl);
		break;

	case PW_REMOTE_STATE_UNCONNECTED:
		pw_log_info(NAME" %p: disconnected", impl);
		impl->core_proxy = NULL;
		impl->registry_proxy = NULL;
		pw_main_loop_quit(impl->loop);
		break;

	default:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));
		break;
	}
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_state_changed,
};

int main(int argc, char *argv[])
{
	struct impl impl = { 0, };

	pw_init(&argc, &argv);

	impl.loop = pw_main_loop_new(NULL);
	impl.core = pw_core_new(pw_main_loop_get_loop(impl.loop), NULL, 0);
        impl.remote = pw_remote_new(impl.core, NULL, 0);

	pw_map_init(&impl.globals, 64, 64);

	spa_list_init(&impl.client_list);
	spa_list_init(&impl.node_list);
	spa_list_init(&impl.session_list);

	pw_core_add_spa_lib(impl.core, "api.bluez5.*", "bluez5/libspa-bluez5");
	pw_core_add_spa_lib(impl.core, "api.alsa.*", "alsa/libspa-alsa");
	pw_core_add_spa_lib(impl.core, "api.v4l2.*", "v4l2/libspa-v4l2");

	pw_module_load(impl.core, "libpipewire-module-client-device", NULL, NULL, NULL, NULL);
	pw_module_load(impl.core, "libpipewire-module-adapter", NULL, NULL, NULL, NULL);

	clock_gettime(CLOCK_MONOTONIC, &impl.now);

	pw_remote_add_listener(impl.remote, &impl.remote_listener, &remote_events, &impl);

	if (pw_remote_connect(impl.remote) < 0)
		return -1;

	pw_main_loop_run(impl.loop);

	pw_core_destroy(impl.core);
	pw_main_loop_destroy(impl.loop);

	return 0;
}
