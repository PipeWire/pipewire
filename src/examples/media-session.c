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

#include <spa/lib/pod.h>
#include <spa/lib/debug.h>

#include "pipewire/core.h"
#include "pipewire/control.h"
#include "pipewire/link.h"
#include "pipewire/log.h"
#include "pipewire/module.h"
#include "pipewire/type.h"
#include "pipewire/private.h"

#define DEFAULT_CHANNELS	2
#define DEFAULT_SAMPLE_RATE	48000

#define MIN_QUANTUM_SIZE	64
#define MAX_QUANTUM_SIZE	1024

struct type {
	struct spa_type_media_type media_type;
        struct spa_type_media_subtype media_subtype;
        struct spa_type_format_audio format_audio;
        struct spa_type_audio_format audio_format;
        struct spa_type_media_subtype_audio media_subtype_audio;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
        spa_type_media_type_map(map, &type->media_type);
        spa_type_media_subtype_map(map, &type->media_subtype);
        spa_type_format_audio_map(map, &type->format_audio);
        spa_type_audio_format_map(map, &type->audio_format);
        spa_type_media_subtype_audio_map(map, &type->media_subtype_audio);
}

struct impl {
	struct type type;

	struct timespec now;

	struct pw_main_loop *loop;
	struct pw_core *core;
	struct pw_type *t;
	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_core_proxy *core_proxy;
	struct spa_hook core_listener;

	struct pw_registry_proxy *registry_proxy;
	struct spa_hook registry_listener;

	struct spa_list stream_list;
	struct spa_list session_list;
	uint32_t seq;
};

struct session {
	struct spa_list l;

	uint32_t id;

	struct impl *impl;

	enum pw_direction direction;
	uint64_t plugged;

	struct pw_node_proxy *node;
	struct spa_hook node_listener;
	struct pw_node_info *info;
	struct pw_port_proxy *node_port;

	struct pw_node_proxy *dsp;
	struct spa_hook dsp_listener;
	struct pw_port_proxy *dsp_port;

	struct pw_link_proxy *link;

	bool enabled;
	bool busy;
	bool exclusive;
	bool need_dsp;

	struct spa_list stream_list;
};

struct stream {
	struct spa_list l;

	struct impl *impl;
	uint32_t id;
	uint32_t parent_id;
	enum pw_direction direction;

	struct pw_node_proxy *node_proxy;
	struct spa_hook node_listener;
	struct pw_node_info *info;

	struct session *session;
	uint32_t sample_rate;
	uint32_t quantum_size;

	struct spa_list links;
};

struct port {
	struct spa_list l;

	struct impl *impl;
	uint32_t id;
	uint32_t parent_id;
	enum pw_direction direction;

	struct node *parent;
	struct pw_port_proxy *port_proxy;
	struct spa_hook port_listener;
};

struct link {
	struct spa_list l;

	struct node *node;
	struct pw_link_proxy *link_proxy;
	struct spa_hook link_listener;
};

static void schedule_rescan(struct impl *impl)
{
	pw_core_proxy_sync(impl->core_proxy, ++impl->seq);
}

static void stream_node_event_info(void *object, struct pw_node_info *info)
{
	struct stream *s = object;
	pw_log_debug("update %d", s->id);
	s->info = pw_node_info_update(s->info, info);
}

static const struct pw_node_proxy_events stream_node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
	.info = stream_node_event_info,
};

static void sess_node_event_info(void *object, struct pw_node_info *info)
{
	struct session *s = object;
	pw_log_debug("update %d", s->id);
	s->info = pw_node_info_update(s->info, info);
}

static const struct pw_node_proxy_events sess_node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
	.info = sess_node_event_info,
};

static int
handle_node(struct impl *impl, uint32_t id, uint32_t parent_id,
		uint32_t type, const struct spa_dict *props)
{
	const char *str;
	bool need_dsp = false;
	enum pw_direction direction;
	struct pw_proxy *p;

	if (props == NULL)
		return 0;

	if ((str = spa_dict_lookup(props, "media.class")) == NULL)
		return 0;

	if (strstr(str, "Stream/") == str) {
		struct stream *stream;

		str += strlen("Stream/");

		if (strcmp(str, "Playback") == 0)
			direction = PW_DIRECTION_OUTPUT;
		else if (strcmp(str, "Capture") == 0)
			direction = PW_DIRECTION_INPUT;
		else
			return 0;

		p = pw_registry_proxy_bind(impl->registry_proxy,
				id, type, PW_VERSION_NODE,
				sizeof(struct stream));

		stream = pw_proxy_get_user_data(p);
		stream->impl = impl;
		stream->id = id;
		stream->parent_id = parent_id;
		stream->direction = direction;
		stream->node_proxy = (struct pw_node_proxy *) p;

		pw_proxy_add_proxy_listener(p, &stream->node_listener, &stream_node_events, stream);

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

		p = pw_registry_proxy_bind(impl->registry_proxy,
				id, type, PW_VERSION_NODE,
				sizeof(struct session));

		sess = pw_proxy_get_user_data(p);
		sess->impl = impl;
		sess->direction = direction;
		sess->id = id;
		sess->need_dsp = need_dsp;

		pw_proxy_add_proxy_listener(p, &sess->node_listener, &sess_node_events, sess);

		spa_list_init(&sess->stream_list);
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

	p = pw_registry_proxy_bind(impl->registry_proxy,
			id, type, PW_VERSION_PORT,
			sizeof(struct port));
	port = pw_proxy_get_user_data(p);
	port->impl = impl;
	port->id = id;
	port->parent_id = parent_id;

	pw_log_debug("new port %p for node %d", port, parent_id);

	return 0;
}

static void
registry_global(void *data,uint32_t id, uint32_t parent_id,
		uint32_t permissions, uint32_t type, uint32_t version,
		const struct spa_dict *props)
{
	struct impl *impl = data;
	struct pw_type *t = impl->t;

	clock_gettime(CLOCK_MONOTONIC, &impl->now);

	if (type == t->node) {
		handle_node(impl, id, parent_id, type, props);
	}
	else if (type == t->port) {
		handle_port(impl, id, parent_id, type, props);
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

static void rescan_session(struct impl *impl)
{
	struct session *sess;
	struct pw_type *t = impl->t;

	pw_log_debug("rescan session");

	spa_list_for_each(sess, &impl->session_list, l) {
		if (sess->need_dsp && sess->dsp == NULL) {
			struct pw_properties *props;

			if (sess->info->props == NULL)
				continue;

			props = pw_properties_new_dict(sess->info->props);
			pw_properties_setf(props, "audio-dsp.direction", "%d", sess->direction);
			pw_properties_setf(props, "audio-dsp.channels", "2");
			pw_properties_setf(props, "audio-dsp.rate", "48000");
			pw_properties_setf(props, "audio-dsp.maxbuffer", "8192");

			sess->dsp = pw_core_proxy_create_object(impl->core_proxy,
					"audio-dsp",
					t->node,
					PW_VERSION_NODE,
					&props->dict,
					0);
		}
	}
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
                                                impl->t->registry,
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
	impl.t = pw_core_get_type(impl.core);
        impl.remote = pw_remote_new(impl.core, NULL, 0);

	init_type(&impl.type, impl.t->map);
	spa_list_init(&impl.session_list);

	clock_gettime(CLOCK_MONOTONIC, &impl.now);

	pw_remote_add_listener(impl.remote, &impl.remote_listener, &remote_events, &impl);

        pw_remote_connect(impl.remote);

	pw_main_loop_run(impl.loop);

	pw_core_destroy(impl.core);
	pw_main_loop_destroy(impl.loop);

	return 0;
}
