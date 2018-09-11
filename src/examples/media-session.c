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

#define DEFAULT_CHANNELS	2
#define DEFAULT_SAMPLERATE	48000

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

	struct spa_list stream_list;
	struct spa_list session_list;
	uint32_t seq;
};

struct object {
	struct impl *impl;
	uint32_t id;
	uint32_t parent_id;
	uint32_t type;
};

struct node {
	struct object obj;

	struct pw_node_proxy *proxy;
	struct spa_hook listener;
	struct pw_node_info *info;

	struct spa_list port_list;
};

struct port {
	struct object obj;

	struct node *node;

	struct spa_list l;

	enum pw_direction direction;
	struct pw_port_proxy *proxy;
	struct spa_hook listener;
};

struct link {
	struct object obj;

	struct node *node;
	struct pw_link_proxy *proxy;
	struct spa_hook listener;
};

struct session {
	struct spa_list l;

	uint32_t id;

	struct impl *impl;
	enum pw_direction direction;
	uint64_t plugged;

	struct node *node;
	struct node *dsp;
	struct link *link;

	struct spa_proxy *proxy;
	struct spa_hook listener;

	bool dsp_pending;
	bool enabled;
	bool busy;
	bool exclusive;
	bool need_dsp;
};

struct stream {
	struct spa_list l;

	struct impl *impl;

	struct node *node;
	enum pw_direction direction;

	struct session *session;
};

static void add_object(struct impl *impl, struct object *obj)
{
	size_t size = pw_map_get_size(&impl->globals);
        while (obj->id > size)
                pw_map_insert_at(&impl->globals, size++, NULL);
        pw_map_insert_at(&impl->globals, obj->id, obj);
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
	pw_core_proxy_sync(impl->core_proxy, ++impl->seq);
}

static void node_event_info(void *object, struct pw_node_info *info)
{
	struct node *n = object;
	pw_log_debug("node %d id %d", n->obj.id, info->id);
	n->info = pw_node_info_update(n->info, info);
}

static const struct pw_node_proxy_events node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
	.info = node_event_info,
};

static int
handle_node(struct impl *impl, uint32_t id, uint32_t parent_id,
		uint32_t type, const struct spa_dict *props)
{
	const char *str;
	bool need_dsp = false;
	enum pw_direction direction;
	struct pw_proxy *p;
	struct node *node;

	p = pw_registry_proxy_bind(impl->registry_proxy,
			id, type, PW_VERSION_NODE,
			sizeof(struct node));

	node = pw_proxy_get_user_data(p);
	node->obj.impl = impl;
	node->obj.id = id;
	node->obj.parent_id = parent_id;
	node->obj.type = type;
	node->proxy = (struct pw_node_proxy *) p;
	spa_list_init(&node->port_list);
	pw_proxy_add_proxy_listener(p, &node->listener, &node_events, node);
	add_object(impl, &node->obj);

	if (props == NULL)
		return 0;

	if ((str = spa_dict_lookup(props, "media.class")) == NULL)
		return 0;

	pw_log_debug("node media.class %s", str);

	if (strstr(str, "Stream/") == str) {
		struct stream *stream;

		str += strlen("Stream/");

		if (strstr(str, "Output/") == str)
			direction = PW_DIRECTION_OUTPUT;
		else if (strstr(str, "Input/") == str)
			direction = PW_DIRECTION_INPUT;
		else
			return 0;

		stream = calloc(1, sizeof(struct stream));
		stream->impl = impl;
		stream->direction = direction;
		stream->node = node;

		spa_list_append(&impl->stream_list, &stream->l);
		pw_log_debug("new stream %p for node %d", stream, id);
	}
	else {
		struct session *sess;

		if (strstr(str, "Audio/") == str) {
			need_dsp = true;
			str += strlen("Audio/");
		}
		else if (strstr(str, "Video/") == str) {
			str += strlen("Video/");
		}
		else
			return 0;

		if (strcmp(str, "Sink") == 0)
			direction = PW_DIRECTION_OUTPUT;
		else if (strcmp(str, "Source") == 0)
			direction = PW_DIRECTION_INPUT;
		else
			return 0;

		sess = calloc(1, sizeof(struct session));
		sess->impl = impl;
		sess->direction = direction;
		sess->id = id;
		sess->need_dsp = need_dsp;
		sess->enabled = true;
		sess->node = node;
		spa_list_append(&impl->session_list, &sess->l);

		pw_log_debug("new session %p for node %d", sess, id);
	}
	return 1;
}

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
	port->node = node;
	port->direction = strcmp(str, "out") ? PW_DIRECTION_OUTPUT : PW_DIRECTION_INPUT;
	port->proxy = (struct pw_port_proxy *)p;
	add_object(impl, &port->obj);

	spa_list_append(&node->port_list, &port->l);

	pw_log_debug("new port %p for node %d", port, parent_id);

	return 0;
}

static void
registry_global(void *data,uint32_t id, uint32_t parent_id,
		uint32_t permissions, uint32_t type, uint32_t version,
		const struct spa_dict *props)
{
	struct impl *impl = data;

	clock_gettime(CLOCK_MONOTONIC, &impl->now);

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
registry_global_remove(void *data,uint32_t id)
{
}

static const struct pw_registry_proxy_events registry_events = {
	PW_VERSION_REGISTRY_PROXY_EVENTS,
        .global = registry_global,
        .global_remove = registry_global_remove,
};


static int link_session_dsp(struct session *session)
{
	struct impl *impl = session->impl;
	struct pw_properties *props;

	pw_log_debug("module %p: link session dsp '%d'", impl, session->id);

	props = pw_properties_new(NULL, NULL);
	pw_properties_setf(props, PW_LINK_OUTPUT_NODE_ID, "%d", session->dsp->info->id);
	pw_properties_setf(props, PW_LINK_OUTPUT_PORT_ID, "%d", -1);
	pw_properties_setf(props, PW_LINK_INPUT_NODE_ID, "%d", session->node->info->id);
	pw_properties_setf(props, PW_LINK_INPUT_PORT_ID, "%d", -1);
	pw_properties_set(props, PW_LINK_PROP_PASSIVE, "true");

        session->link = pw_core_proxy_create_object(impl->core_proxy,
                                          "link-factory",
                                          PW_TYPE_INTERFACE_Link,
                                          PW_VERSION_LINK,
                                          &props->dict,
					  0);
	return 0;
}



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

	pw_log_debug("module %p: looking at session '%d' enabled:%d busy:%d exclusive:%d",
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
		pw_log_debug("module %p: session in use", impl);
		return 0;
	}

	pw_log_debug("module %p: found session '%d' %" PRIu64, impl,
			sess->id, plugged);

	if (find->sess == NULL || plugged > find->plugged) {
		pw_log_debug("module %p: new best %" PRIu64, impl, plugged);
		find->sess = sess;
		find->plugged = plugged;
	}
	return 0;
}

static int link_nodes(struct node *node, enum pw_direction direction, struct stream *stream)
{
	struct impl *impl = node->obj.impl;
	struct pw_properties *props;
	struct port *p;

	pw_log_debug("module %p: link nodes %d %d", impl, stream->node->obj.id, node->obj.id);

	spa_list_for_each(p, &node->port_list, l) {
		pw_log_debug("module %p: port %d %d", impl, p->obj.id, p->direction);

		if (p->direction == direction)
			continue;

		props = pw_properties_new(NULL, NULL);
		pw_properties_setf(props, PW_LINK_OUTPUT_NODE_ID, "%d", stream->node->obj.id);
		pw_properties_setf(props, PW_LINK_OUTPUT_PORT_ID, "%d", -1);
		pw_properties_setf(props, PW_LINK_INPUT_NODE_ID, "%d", node->obj.id);
		pw_properties_setf(props, PW_LINK_INPUT_PORT_ID, "%d", p->obj.id);

		pw_core_proxy_create_object(impl->core_proxy,
                                          "link-factory",
                                          PW_TYPE_INTERFACE_Link,
                                          PW_VERSION_LINK,
                                          &props->dict,
					  0);
	}
	return 0;
}

static int rescan_stream(struct impl *impl, struct stream *stream)
{
	struct spa_dict *props;
        const char *str, *media, *category, *role;
        bool exclusive;
        struct find_data find;
	struct session *session;
	struct pw_node_info *info;
	struct node *peer;
	enum pw_direction direction;
	int res;

	pw_log_debug("rescan stream %p", stream);

	if (stream->node->info == NULL || stream->node->info->props == NULL) {
		pw_log_debug("stream %p has no properties", stream);
		return 0;
	}

	if (stream->session != NULL) {
		pw_log_debug("stream %p already has session %p", stream, stream->session);
		return 0;
	}

	info = stream->node->info;
	props = info->props;

        str = spa_dict_lookup(props, PW_NODE_PROP_AUTOCONNECT);
        if (str == NULL || !pw_properties_parse_bool(str)) {
		pw_log_debug("stream %p does not need autoconnect", stream);
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

	pw_log_info("module %p: '%s' '%s' '%s' exclusive:%d target %d", impl,
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

	if (strcmp(category, "Capture") == 0)
		direction = PW_DIRECTION_OUTPUT;
	else if (strcmp(category, "Playback") == 0)
		direction = PW_DIRECTION_INPUT;
	else
		return -EINVAL;

	if (exclusive || session->dsp == NULL) {
		if (exclusive && session->busy) {
			pw_log_warn("session busy, can't get exclusive access");
			return -EBUSY;
		}
		if (session->link != NULL) {
			pw_log_warn("session busy with DSP");
			return -EBUSY;
		}
		peer = session->node;
		session->exclusive = exclusive;
	}
	else {
		if (session->link == NULL) {
			if ((res = link_session_dsp(session)) < 0)
				return res;
		}
		peer = session->dsp;
	}

	pw_log_debug("module %p: linking to session '%d'", impl, session->id);

        session->busy = true;
	stream->session = session;

	link_nodes(peer, direction, stream);

//        pw_node_for_each_port(peer, direction, on_peer_port, info);

        return 1;
}

static void dsp_node_event_info(void *object, struct pw_node_info *info)
{
	struct session *s = object;
	struct node *dsp;

	pw_log_debug("dsp node session %d id %d", s->id, info->id);

	if ((dsp = find_object(s->impl, info->id)) == NULL)
		return;

	s->dsp = dsp;
	spa_hook_remove(&s->listener);
}

static const struct pw_node_proxy_events dsp_node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
	.info = dsp_node_event_info,
};

static void rescan_session(struct impl *impl)
{
	struct session *sess;
	struct stream *stream;

	pw_log_debug("rescan session");

	spa_list_for_each(sess, &impl->session_list, l) {
		if (sess->need_dsp && sess->dsp == NULL && !sess->dsp_pending) {
			struct pw_properties *props;
			void *dsp;

			if (sess->node->info->props == NULL)
				continue;

			props = pw_properties_new_dict(sess->node->info->props);
			pw_properties_setf(props, "audio-dsp.direction", "%d", sess->direction);
			pw_properties_setf(props, "audio-dsp.channels", "%d", 4);
			pw_properties_setf(props, "audio-dsp.rate", "%d", DEFAULT_SAMPLERATE);
			pw_properties_setf(props, "audio-dsp.maxbuffer", "%ld", MAX_QUANTUM_SIZE * sizeof(float));

			pw_log_debug("making audio dsp %p", sess);

			dsp = pw_core_proxy_create_object(impl->core_proxy,
					"audio-dsp",
					PW_TYPE_INTERFACE_Node,
					PW_VERSION_NODE,
					&props->dict,
					0);
			sess->dsp_pending = true;
			pw_proxy_add_proxy_listener(dsp, &sess->listener, &dsp_node_events, sess);
		}
	}
	spa_list_for_each(stream, &impl->stream_list, l)
		rescan_stream(impl, stream);
}

static void on_state_changed(void *_data, enum pw_remote_state old, enum pw_remote_state state, const char *error)
{
	struct impl *impl = _data;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		printf("remote error: %s\n", error);
		pw_main_loop_quit(impl->loop);
		break;

	case PW_REMOTE_STATE_CONNECTED:
		impl->core_proxy = pw_remote_get_core_proxy(impl->remote);
		impl->registry_proxy = pw_core_proxy_get_registry(impl->core_proxy,
                                                PW_TYPE_INTERFACE_Registry,
                                                PW_VERSION_REGISTRY, 0);
		pw_registry_proxy_add_listener(impl->registry_proxy,
                                               &impl->registry_listener,
                                               &registry_events, impl);
		schedule_rescan(impl);
		break;

	default:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));
		break;
	}
}

static void remote_sync_reply(void *data, uint32_t seq)
{
	struct impl *impl = data;

	pw_log_debug("done %d", seq);

	if (impl->seq == seq)
		rescan_session(impl);
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
	spa_list_init(&impl.stream_list);

	clock_gettime(CLOCK_MONOTONIC, &impl.now);

	pw_remote_add_listener(impl.remote, &impl.remote_listener, &remote_events, &impl);

        pw_remote_connect(impl.remote);

	pw_main_loop_run(impl.loop);

	pw_core_destroy(impl.core);
	pw_main_loop_destroy(impl.loop);

	return 0;
}
