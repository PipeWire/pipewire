/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
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

#include "pipewire/core.h"
#include "pipewire/control.h"
#include "pipewire/link.h"
#include "pipewire/log.h"
#include "pipewire/module.h"
#include "pipewire/type.h"
#include "pipewire/private.h"

#define NAME "media-session"

#define DEFAULT_CHANNELS	2
#define DEFAULT_SAMPLERATE	48000

#define DEFAULT_IDLE_SECONDS	3

#define MIN_QUANTUM_SIZE	64
#define MAX_QUANTUM_SIZE	1024

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

	struct spa_list node_list;
	struct spa_list session_list;
	uint32_t seq;
};

struct object {
	struct impl *impl;
	uint32_t id;
	uint32_t parent_id;
	uint32_t type;
	struct pw_proxy *proxy;
	struct spa_hook listener;
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
#define NODE_TYPE_DSP		2
#define NODE_TYPE_DEVICE	3
	uint32_t type;

	struct spa_audio_info_raw format;
};

struct port {
	struct object obj;

	struct spa_list l;
	enum pw_direction direction;
	struct pw_port_info *info;
	struct node *node;

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
	struct node *dsp;
	struct pw_proxy *dsp_proxy;
	struct pw_proxy *link_proxy;

	struct spa_list node_list;

	struct spa_hook listener;

	struct spa_source *idle_timeout;

	bool starting;
	bool enabled;
	bool busy;
	bool exclusive;
	bool need_dsp;
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
	if (obj->proxy)
		pw_proxy_destroy(obj->proxy);
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
		pw_core_proxy_sync(impl->core_proxy, ++impl->seq);
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
	if (sess->dsp)
		pw_node_proxy_send_command((struct pw_node_proxy*)sess->dsp->obj.proxy, cmd);
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

static int unlink_session_dsp(struct impl *impl, struct session *session)
{
	if (session->link_proxy == NULL)
		return 0;

	if (impl->core_proxy)
		pw_core_proxy_destroy(impl->core_proxy, session->link_proxy);

	session->link_proxy = NULL;
	return 0;
}

static int on_node_idle(struct impl *impl, struct node *node)
{
	struct session *sess = node->manager;

	if (sess == NULL)
		return 0;

	switch (node->type) {
	case NODE_TYPE_DSP:
		pw_log_debug(NAME" %p: dsp idle for session %d", impl, sess->id);
		unlink_session_dsp(impl, sess);
		break;

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


static int link_session_dsp(struct impl *impl, struct session *session)
{
	struct pw_properties *props;

	if (session->link_proxy != NULL)
		return 0;

	pw_log_debug(NAME " %p: link session dsp '%d'", impl, session->id);

	props = pw_properties_new(NULL, NULL);
	pw_properties_set(props, PW_LINK_PROP_PASSIVE, "true");
	if (session->direction == PW_DIRECTION_OUTPUT) {
		pw_properties_setf(props, PW_LINK_OUTPUT_NODE_ID, "%d", session->dsp->info->id);
		pw_properties_setf(props, PW_LINK_OUTPUT_PORT_ID, "%d", -1);
		pw_properties_setf(props, PW_LINK_INPUT_NODE_ID, "%d", session->node->info->id);
		pw_properties_setf(props, PW_LINK_INPUT_PORT_ID, "%d", -1);
	}
	else {
		pw_properties_setf(props, PW_LINK_OUTPUT_NODE_ID, "%d", session->node->info->id);
		pw_properties_setf(props, PW_LINK_OUTPUT_PORT_ID, "%d", -1);
		pw_properties_setf(props, PW_LINK_INPUT_NODE_ID, "%d", session->dsp->info->id);
		pw_properties_setf(props, PW_LINK_INPUT_PORT_ID, "%d", -1);
	}

        session->link_proxy = pw_core_proxy_create_object(impl->core_proxy,
                                          "link-factory",
                                          PW_TYPE_INTERFACE_Link,
                                          PW_VERSION_LINK,
                                          &props->dict,
					  0);
	pw_properties_free(props);

	return 0;
}

static int on_node_running(struct impl *impl, struct node *node)
{
	struct session *sess = node->manager;

	if (sess == NULL)
		return 0;

	switch (node->type) {
	case NODE_TYPE_DSP:
		pw_log_debug(NAME" %p: dsp running for session %d", impl, sess->id);
		link_session_dsp(impl, sess);
		break;

	case NODE_TYPE_DEVICE:
		pw_log_debug(NAME" %p: device running or session %d", impl, sess->id);
		remove_idle_timeout(sess);
		break;
	default:
		break;
	}
	return 0;
}

static void node_event_info(void *object, struct pw_node_info *info)
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

static void node_event_param(void *object,
                       uint32_t id, uint32_t index, uint32_t next,
                       const struct spa_pod *param)
{
	struct node *n = object;
	struct impl *impl = n->obj.impl;
	uint32_t media_type, media_subtype;
	struct spa_audio_info_raw info;

	pw_log_debug(NAME" %p: param for node %d", impl, n->obj.id);

	if (id != SPA_PARAM_EnumFormat)
		return;

	if (spa_format_parse(param, &media_type, &media_subtype) < 0)
		return;

	if (media_type != SPA_MEDIA_TYPE_audio ||
	    media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;

	spa_pod_fixate((struct spa_pod*)param);

	if (spa_format_audio_raw_parse(param, &info) < 0)
		return;

	n->format = info;
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

	if (sess->dsp) {
		sess->dsp->manager = NULL;
	}
	if (sess->dsp_proxy) {
		pw_log_debug(NAME " %p: destroy dsp %p", impl, sess->dsp_proxy);
		if (impl->core_proxy)
			pw_core_proxy_destroy(impl->core_proxy, sess->dsp_proxy);
		sess->dsp_proxy = NULL;
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
	if (n->session) {
		spa_list_remove(&n->session_link);
		n->session = NULL;
	}
	if (n->manager) {
		switch (n->type) {
		case NODE_TYPE_DSP:
			n->manager->dsp = NULL;
			break;
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

	media_class = props ? spa_dict_lookup(props, "media.class") : NULL;

	p = pw_registry_proxy_bind(impl->registry_proxy,
			id, type, PW_VERSION_NODE,
			sizeof(struct node));

	node = pw_proxy_get_user_data(p);
	node->obj.impl = impl;
	node->obj.id = id;
	node->obj.parent_id = parent_id;
	node->obj.type = type;
	node->obj.proxy = p;
	spa_list_init(&node->port_list);
	pw_proxy_add_listener(p, &node->obj.listener, &node_proxy_events, node);
	pw_proxy_add_proxy_listener(p, &node->listener, &node_events, node);
	add_object(impl, &node->obj);
	spa_list_append(&impl->node_list, &node->l);
	node->type = NODE_TYPE_UNKNOWN;

	if (media_class == NULL)
		return 0;

	pw_log_debug(NAME" %p: node media.class %s", impl, media_class);

	if (strstr(media_class, "Stream/") == media_class) {
		media_class += strlen("Stream/");

		if (strstr(media_class, "Output/") == media_class)
			direction = PW_DIRECTION_OUTPUT;
		else if (strstr(media_class, "Input/") == media_class)
			direction = PW_DIRECTION_INPUT;
		else
			return 0;

		node->direction = direction;
		node->type = NODE_TYPE_STREAM;
		pw_log_debug(NAME "%p: node %d is stream", impl, id);

		pw_node_proxy_enum_params((struct pw_node_proxy*)p,
				SPA_PARAM_EnumFormat,
				0, -1, NULL);
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
		sess->starting = true;
		sess->node = node;
		if ((str = spa_dict_lookup(props, "node.plugged")) != NULL)
			sess->plugged = pw_properties_parse_uint64(str);
		else
			sess->plugged = SPA_TIMESPEC_TO_TIME(&impl->now);

		spa_list_init(&sess->node_list);
		spa_list_append(&impl->session_list, &sess->l);

		node->direction = direction;
		node->type = NODE_TYPE_DEVICE;
		node->manager = sess;

		pw_log_debug(NAME" %p: new session for device node %d", impl, id);
	}
	return 1;
}

static void port_event_info(void *object, struct pw_port_info *info)
{
	struct port *p = object;
	pw_log_debug(NAME" %p: info for port %d", p->obj.impl, p->obj.id);
	p->info = pw_port_info_update(p->info, info);
}

static void port_event_param(void *object,
                       uint32_t id, uint32_t index, uint32_t next,
                       const struct spa_pod *param)
{
	struct port *p = object;
	struct node *node = p->node;
	uint32_t media_type, media_subtype;
	struct spa_audio_info_raw info;

	pw_log_debug(NAME" %p: param for port %d", p->obj.impl, p->obj.id);

	if (node == NULL)
		return;

	if (id != SPA_PARAM_EnumFormat)
		return;

	if (spa_format_parse(param, &media_type, &media_subtype) < 0)
		return;

	if (media_type != SPA_MEDIA_TYPE_audio ||
	    media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;

	spa_pod_fixate((struct spa_pod*)param);

	if (spa_format_audio_raw_parse(param, &info) < 0)
		return;

	if (info.channels > node->format.channels) {
		node->format = info;
		if (node->manager)
			node->manager->enabled = true;
	}
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

	if (props == NULL || (str = spa_dict_lookup(props, "port.direction")) == NULL)
		return -EINVAL;

	p = pw_registry_proxy_bind(impl->registry_proxy,
			id, type, PW_VERSION_PORT,
			sizeof(struct port));

	port = pw_proxy_get_user_data(p);
	port->obj.impl = impl;
	port->obj.id = id;
	port->obj.parent_id = parent_id;
	port->obj.type = type;
	port->obj.proxy = p;
	port->node = node;
	port->direction = strcmp(str, "out") ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT;
	pw_proxy_add_listener(p, &port->obj.listener, &port_proxy_events, port);
	pw_proxy_add_proxy_listener(p, &port->listener, &port_events, port);
	add_object(impl, &port->obj);

	spa_list_append(&node->port_list, &port->l);

	if (node->type == NODE_TYPE_DEVICE) {
		pw_port_proxy_enum_params((struct pw_port_proxy*)p,
				SPA_PARAM_EnumFormat,
				0, -1, NULL);
	}

	pw_log_debug(NAME" %p: new port %d for node %d", impl, id, parent_id);

	return 0;
}

static void
registry_global(void *data,uint32_t id, uint32_t parent_id,
		uint32_t permissions, uint32_t type, uint32_t version,
		const struct spa_dict *props)
{
	struct impl *impl = data;

	pw_log_debug(NAME " %p: new global '%d'", impl, id);

	switch (type) {
	case PW_TYPE_INTERFACE_Node:
		handle_node(impl, id, parent_id, type, props);
		break;

	case PW_TYPE_INTERFACE_Port:
		handle_port(impl, id, parent_id, type, props);
		break;

	default:
		break;
	}
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

		if ((str = spa_dict_lookup(props, "media.class")) == NULL)
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

	pw_log_debug(NAME " %p: link nodes %d %d", impl, node->obj.id, peer->obj.id);

	spa_list_for_each(p, &peer->port_list, l) {
		struct pw_properties *props;

		if (p->direction == direction)
			continue;

		if (max-- == 0)
			return 0;

		props = pw_properties_new(NULL, NULL);
		if (p->direction == PW_DIRECTION_OUTPUT) {
			pw_properties_setf(props, PW_LINK_OUTPUT_NODE_ID, "%d", node->obj.id);
			pw_properties_setf(props, PW_LINK_OUTPUT_PORT_ID, "%d", -1);
			pw_properties_setf(props, PW_LINK_INPUT_NODE_ID, "%d", peer->obj.id);
			pw_properties_setf(props, PW_LINK_INPUT_PORT_ID, "%d", p->obj.id);
			pw_log_debug(NAME " %p: node %d -> port %d:%d", impl,
					node->obj.id, peer->obj.id, p->obj.id);

		}
		else {
			pw_properties_setf(props, PW_LINK_OUTPUT_NODE_ID, "%d", peer->obj.id);
			pw_properties_setf(props, PW_LINK_OUTPUT_PORT_ID, "%d", p->obj.id);
			pw_properties_setf(props, PW_LINK_INPUT_NODE_ID, "%d", node->obj.id);
			pw_properties_setf(props, PW_LINK_INPUT_PORT_ID, "%d", -1);
			pw_log_debug(NAME " %p: port %d:%d -> node %d", impl,
					peer->obj.id, p->obj.id, node->obj.id);
		}

		pw_core_proxy_create_object(impl->core_proxy,
                                          "link-factory",
                                          PW_TYPE_INTERFACE_Link,
                                          PW_VERSION_LINK,
                                          &props->dict,
					  0);

		pw_properties_free(props);
	}
	return 0;
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
	struct spa_audio_info_raw audio_info;
	struct spa_pod *param;
	char buf[1024];

	if (node->type == NODE_TYPE_DSP || node->type == NODE_TYPE_DEVICE)
		return 0;

	if (node->session != NULL)
		return 0;

	if (node->info == NULL || node->info->props == NULL) {
		pw_log_debug(NAME " %p: node %d has no properties", impl, node->obj.id);
		return 0;
	}

	info = node->info;
	props = info->props;

        str = spa_dict_lookup(props, PW_NODE_PROP_AUTOCONNECT);
        if (str == NULL || !pw_properties_parse_bool(str)) {
		pw_log_debug(NAME" %p: node %d does not need autoconnect", impl, node->obj.id);
                return 0;
	}

	if ((media = spa_dict_lookup(props, PW_NODE_PROP_MEDIA)) == NULL)
		media = "Audio";

	if ((category = spa_dict_lookup(props, PW_NODE_PROP_CATEGORY)) == NULL) {
		if (info->n_input_ports > 0 && info->n_output_ports == 0)
			category = "Capture";
		else if (info->n_output_ports > 0 && info->n_input_ports == 0)
			category = "Playback";
		else
			return -EINVAL;
	}

	if ((role = spa_dict_lookup(props, PW_NODE_PROP_ROLE)) == NULL)
		role = "Music";

	if ((str = spa_dict_lookup(props, PW_NODE_PROP_EXCLUSIVE)) != NULL)
		exclusive = pw_properties_parse_bool(str);
	else
		exclusive = false;

	if (strcmp(media, "Audio") == 0) {
		if (strcmp(category, "Playback") == 0)
			find.media_class = "Audio/Sink";
		else if (strcmp(category, "Capture") == 0)
			find.media_class = "Audio/Source";
		else
			return -EINVAL;
	}
	else if (strcmp(media, "Video") == 0) {
		if (strcmp(category, "Capture") == 0)
			find.media_class = "Video/Source";
		else
			return -EINVAL;
	}
	else
		return -EINVAL;

	str = spa_dict_lookup(props, PW_NODE_PROP_TARGET_NODE);
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
	if (find.sess == NULL)
		return -ENOENT;

	session = find.sess;

	if (session->starting) {
		pw_log_info(NAME " %p: session %d is starting", impl, session->id);
		return 0;
	}

	if (strcmp(category, "Capture") == 0)
		direction = PW_DIRECTION_OUTPUT;
	else if (strcmp(category, "Playback") == 0)
		direction = PW_DIRECTION_INPUT;
	else
		return -EINVAL;

	if (exclusive || session->dsp == NULL) {
		if (exclusive && session->busy) {
			pw_log_warn(NAME" %p: session %d busy, can't get exclusive access", impl, session->id);
			return -EBUSY;
		}
		if (session->link_proxy != NULL) {
			pw_log_warn(NAME" %p: session %d busy with DSP", impl, session->id);
			return -EBUSY;
		}
		peer = session->node;
		session->exclusive = exclusive;
	}
	else {
		peer = session->dsp;
	}

	pw_log_debug(NAME" %p: linking to session '%d'", impl, session->id);

        session->busy = true;
	node->session = session;
	spa_list_append(&session->node_list, &node->session_link);

	if (!exclusive) {
		audio_info = session->node->format;
		audio_info.format = SPA_AUDIO_FORMAT_F32P;
		audio_info.rate = session->node->format.rate;
		audio_info.channels = SPA_MIN(session->node->format.channels, node->format.channels);

		spa_pod_builder_init(&b, buf, sizeof(buf));
		param = spa_pod_builder_object(&b,
			SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
			SPA_PARAM_PROFILE_direction, &SPA_POD_Id(pw_direction_reverse(direction)),
			SPA_PARAM_PROFILE_format,    spa_format_audio_raw_build(&b,
							SPA_PARAM_Format, &audio_info),
			0);

		pw_node_proxy_set_param((struct pw_node_proxy*)node->obj.proxy,
				SPA_PARAM_Profile, 0, param);
	} else {
		audio_info.channels = 1;
	}
	link_nodes(peer, direction, node, audio_info.channels);

        return 1;
}

static void dsp_node_event_info(void *object, struct pw_node_info *info)
{
	struct session *s = object;
	struct node *dsp;

	if ((dsp = find_object(s->impl, info->id)) == NULL)
		return;

	pw_log_debug(NAME" %p: dsp node session %d id %d", dsp->obj.impl, s->id, info->id);

	s->dsp = dsp;
	spa_hook_remove(&s->listener);

	dsp->direction = s->direction;
	dsp->type = NODE_TYPE_DSP;
	dsp->manager = s;
}

static const struct pw_node_proxy_events dsp_node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
	.info = dsp_node_event_info,
};

static void rescan_session(struct impl *impl, struct session *sess)
{
	if (sess->need_dsp && sess->dsp == NULL && sess->dsp_proxy == NULL) {
		struct pw_properties *props;
		struct node *node = sess->node;
		struct spa_audio_info_raw info;
		uint8_t buf[1024];
		struct spa_pod_builder b = { 0, };
		struct spa_pod *param;

		if (node->info->props == NULL)
			return;

		info = node->format;

		props = pw_properties_new_dict(node->info->props);
		pw_properties_setf(props, "audio-dsp.direction", "%d", sess->direction);
		pw_properties_setf(props, "audio-dsp.maxbuffer", "%ld", MAX_QUANTUM_SIZE * sizeof(float));

		pw_log_debug(NAME" %p: making audio dsp for session %d", impl, sess->id);

		sess->dsp_proxy = pw_core_proxy_create_object(impl->core_proxy,
				"audio-dsp",
				PW_TYPE_INTERFACE_Node,
				PW_VERSION_NODE,
				&props->dict,
				0);
		pw_properties_free(props);

		pw_proxy_add_proxy_listener(sess->dsp_proxy, &sess->listener, &dsp_node_events, sess);

		spa_pod_builder_init(&b, buf, sizeof(buf));
		param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &info);
		param = spa_pod_builder_object(&b,
			SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
			SPA_PARAM_PROFILE_direction, &SPA_POD_Id(pw_direction_reverse(sess->direction)),
			SPA_PARAM_PROFILE_format,    param,
			0);

		pw_node_proxy_set_param((struct pw_node_proxy*)sess->dsp_proxy,
				SPA_PARAM_Profile, 0, param);
		schedule_rescan(impl);
	}
	else {
		sess->starting = false;
	}
}

static void do_rescan(struct impl *impl)
{
	struct session *sess;
	struct node *node;

	clock_gettime(CLOCK_MONOTONIC, &impl->now);

	spa_list_for_each(sess, &impl->session_list, l)
		rescan_session(impl, sess);
	spa_list_for_each(node, &impl->node_list, l)
		rescan_node(impl, node);
}

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
		impl->registry_proxy = pw_core_proxy_get_registry(impl->core_proxy,
                                                PW_TYPE_INTERFACE_Registry,
                                                PW_VERSION_REGISTRY, 0);
		pw_registry_proxy_add_listener(impl->registry_proxy,
                                               &impl->registry_listener,
                                               &registry_events, impl);
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

static void remote_sync_reply(void *data, uint32_t seq)
{
	struct impl *impl = data;
	if (impl->seq == seq)
		do_rescan(impl);
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_state_changed,
	.sync_reply = remote_sync_reply
};

int main(int argc, char *argv[])
{
	struct impl impl = { 0, };

	pw_init(&argc, &argv);

	impl.loop = pw_main_loop_new(NULL);
	impl.core = pw_core_new(pw_main_loop_get_loop(impl.loop), NULL);
        impl.remote = pw_remote_new(impl.core, NULL, 0);

	pw_map_init(&impl.globals, 64, 64);

	spa_list_init(&impl.session_list);
	spa_list_init(&impl.node_list);

	clock_gettime(CLOCK_MONOTONIC, &impl.now);

	pw_remote_add_listener(impl.remote, &impl.remote_listener, &remote_events, &impl);

        pw_remote_connect(impl.remote);

	pw_main_loop_run(impl.loop);

	pw_core_destroy(impl.core);
	pw_main_loop_destroy(impl.loop);

	return 0;
}
