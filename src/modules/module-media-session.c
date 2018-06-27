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

#include "module-media-session/audio-dsp.h"

static const struct spa_dict_item module_props[] = {
	{ PW_MODULE_PROP_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_MODULE_PROP_DESCRIPTION, "Manage media sessions" },
	{ PW_MODULE_PROP_VERSION, PACKAGE_VERSION },
};

#define DEFAULT_CHANNELS	2
#define DEFAULT_SAMPLE_RATE	48000
#define DEFAULT_BUFFER_SIZE	(64 * sizeof(float))
#define MAX_BUFFER_SIZE		(1024 * sizeof(float))

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

	struct pw_core *core;
	struct pw_type *t;
	struct pw_module *module;
	struct spa_hook core_listener;
	struct spa_hook module_listener;
	struct pw_properties *properties;

	struct spa_list session_list;
};

struct session {
	struct spa_list l;

	uint32_t id;

	struct impl *impl;

	enum pw_direction direction;
	uint64_t plugged;

	struct pw_node *node;
	struct spa_hook node_listener;
	struct pw_port *node_port;

	struct pw_node *dsp;
	struct spa_hook dsp_listener;
	struct pw_port *dsp_port;

	struct pw_link *link;

	bool enabled;
	bool busy;
	bool exclusive;
	int sample_rate;
	int buffer_size;

	struct spa_list node_list;
};

struct node_info {
	struct spa_list l;

	struct impl *impl;
	struct session *session;
	struct pw_node *node;
	struct spa_hook node_listener;

	uint32_t sample_rate;
	uint32_t buffer_size;

	struct spa_list links;
};

struct link_data {
	struct spa_list l;

	struct node_info *node_info;
	struct pw_link *link;
	struct spa_hook link_listener;
};


static int handle_autoconnect(struct impl *impl, struct pw_node *node,
		const struct pw_properties *props);

/** \endcond */

static void link_data_remove(struct link_data *data)
{
	spa_list_remove(&data->l);
	spa_hook_remove(&data->link_listener);
}

static void node_info_free(struct node_info *info)
{
	struct link_data *ld, *t;

	spa_list_remove(&info->l);
	spa_hook_remove(&info->node_listener);
	spa_list_for_each_safe(ld, t, &info->links, l)
		link_data_remove(ld);
	free(info);
}

static void session_destroy(struct session *sess)
{
	struct node_info *ni, *t;

	spa_list_remove(&sess->l);
	spa_hook_remove(&sess->node_listener);
	if (sess->dsp) {
		spa_hook_remove(&sess->dsp_listener);
		pw_node_destroy(sess->dsp);
	}
	spa_list_for_each_safe(ni, t, &sess->node_list, l) {
		pw_node_set_state(ni->node, PW_NODE_STATE_SUSPENDED);
		pw_node_set_driver(ni->node, NULL);
		handle_autoconnect(ni->impl, ni->node,
			pw_node_get_properties(ni->node));
		node_info_free(ni);
	}
	free(sess);
}

static void
link_port_unlinked(void *data, struct pw_port *port)
{
	struct link_data *ld = data;
	struct node_info *info = ld->node_info;
	struct pw_link *link = ld->link;
	struct impl *impl = info->impl;

	pw_log_debug("module %p: link %p: port %p unlinked", impl, link, port);
}

static void
link_state_changed(void *data, enum pw_link_state old, enum pw_link_state state, const char *error)
{
	struct link_data *ld = data;
	struct node_info *info = ld->node_info;
	struct pw_link *link = ld->link;
	struct impl *impl = info->impl;

	switch (state) {
	case PW_LINK_STATE_ERROR:
	{
		struct pw_global *global = pw_node_get_global(info->node);
		struct pw_client *owner = pw_global_get_owner(global);

		pw_log_debug("module %p: link %p: state error: %s", impl, link, error);
		if (owner)
			pw_resource_error(pw_client_get_core_resource(owner), -ENODEV, error);

		break;
	}

	case PW_LINK_STATE_UNLINKED:
		pw_log_debug("module %p: link %p: unlinked", impl, link);
		break;

	case PW_LINK_STATE_INIT:
	case PW_LINK_STATE_NEGOTIATING:
	case PW_LINK_STATE_ALLOCATING:
	case PW_LINK_STATE_PAUSED:
	case PW_LINK_STATE_RUNNING:
		break;
	}
}

static void try_link_controls(struct impl *impl, struct pw_port *port, struct pw_port *target)
{
	struct pw_control *cin, *cout;
	int res;

	pw_log_debug("module %p: trying controls", impl);
	spa_list_for_each(cout, &port->control_list[SPA_DIRECTION_OUTPUT], port_link) {
		spa_list_for_each(cin, &target->control_list[SPA_DIRECTION_INPUT], port_link) {
			if (cin->prop_id == cout->prop_id) {
				if ((res = pw_control_link(cout, cin)) < 0)
					pw_log_error("failed to link controls: %s", spa_strerror(res));
			}
		}
	}
	spa_list_for_each(cin, &port->control_list[SPA_DIRECTION_INPUT], port_link) {
		spa_list_for_each(cout, &target->control_list[SPA_DIRECTION_OUTPUT], port_link) {
			if (cin->prop_id == cout->prop_id) {
				if ((res = pw_control_link(cout, cin)) < 0)
					pw_log_error("failed to link controls: %s", spa_strerror(res));
			}
		}
	}


}

static void
link_destroy(void *data)
{
	struct link_data *ld = data;
	pw_log_debug("module %p: link %p destroyed", ld->node_info->impl, ld->link);
	link_data_remove(ld);
}

static const struct pw_link_events link_events = {
	PW_VERSION_LINK_EVENTS,
	.destroy = link_destroy,
	.port_unlinked = link_port_unlinked,
	.state_changed = link_state_changed,
};

static int link_ports(struct node_info *info, struct pw_port *port, struct pw_port *target)
{
	struct impl *impl = info->impl;
	struct pw_link *link;
	struct link_data *ld;
	char *error = NULL;

	if (pw_port_get_direction(port) == PW_DIRECTION_INPUT) {
	        struct pw_port *tmp = target;
		target = port;
		port = tmp;
	}

	link = pw_link_new(impl->core,
			   port, target,
			   NULL, NULL,
			   &error,
			   sizeof(struct link_data));
	if (link == NULL)
		return -ENOMEM;

	ld = pw_link_get_user_data(link);
	ld->link = link;
	ld->node_info = info;
	pw_link_add_listener(link, &ld->link_listener, &link_events, ld);

	spa_list_append(&info->links, &ld->l);
	pw_link_register(link, NULL, pw_module_get_global(impl->module), NULL);

	try_link_controls(impl, port, target);
	return 0;
}

static int on_peer_port(void *data, struct pw_port *port)
{
	struct node_info *info = data;
	struct pw_port *p;

	p = pw_node_get_free_port(info->node, pw_direction_reverse(port->direction));
	if (p == NULL)
		return 0;

	return link_ports(info, p, port);
}

static void reconfigure_session(struct session *sess)
{
	struct node_info *ni;
	struct impl *impl = sess->impl;
	uint32_t buffer_size = MAX_BUFFER_SIZE;

	spa_list_for_each(ni, &sess->node_list, l) {
		if (ni->buffer_size > 0)
			buffer_size = SPA_MIN(buffer_size, ni->buffer_size);
	}
	if (spa_list_is_empty(&sess->node_list)) {
		sess->exclusive = false;
		sess->busy = false;
	}

	sess->buffer_size = buffer_size;

	sess->node->rt.quantum->rate.num = 1;
	sess->node->rt.quantum->rate.denom = sess->sample_rate;
	sess->node->rt.quantum->size = sess->buffer_size;

	pw_log_info("module %p: driver node:%p quantum:%d/%d",
			impl, sess->node, sess->sample_rate, buffer_size);
}

static void node_info_destroy(void *data)
{
	struct node_info *info = data;
	struct session *session = info->session;

	node_info_free(info);

	reconfigure_session(session);
}

static const struct pw_node_events node_info_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = node_info_destroy,
};

static int link_session_dsp(struct session *session)
{
	struct impl *impl = session->impl;
	struct pw_port *op, *ip;
	char *error = NULL;

	pw_log_debug("module %p: link session dsp '%d'", impl, session->id);

	if (session->direction == PW_DIRECTION_OUTPUT) {
		op = session->dsp_port;
		ip = session->node_port;
	}
	else {
		op = session->node_port;
		ip = session->dsp_port;
	}

	session->link = pw_link_new(impl->core,
			   op,
			   ip,
			   NULL,
			   pw_properties_new(PW_LINK_PROP_PASSIVE, "true", NULL),
			   &error, 0);

	if (session->link == NULL) {
		pw_log_error("can't create link: %s", error);
		free(error);
		return -ENOMEM;
	}
	pw_link_register(session->link, NULL, pw_module_get_global(impl->module), NULL);

	reconfigure_session(session);

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
	const struct pw_properties *props;
	const char *str;
	uint64_t plugged = 0;

	pw_log_debug("module %p: looking at session '%d' enabled:%d busy:%d exclusive:%d",
			impl, sess->id, sess->enabled, sess->busy, sess->exclusive);

	if (!sess->enabled)
		return 0;

	if (find->path_id != SPA_ID_INVALID && sess->id != find->path_id)
		return 0;

	if (find->path_id == SPA_ID_INVALID) {
		if ((props = pw_node_get_properties(sess->node)) == NULL)
			return 0;

		if ((str = pw_properties_get(props, "media.class")) == NULL)
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

static uint32_t flp2(uint32_t x)
{
	x = x | (x >> 1);
	x = x | (x >> 2);
	x = x | (x >> 4);
	x = x | (x >> 8);
	x = x | (x >> 16);
	return x - (x >> 1);
}

static int handle_autoconnect(struct impl *impl, struct pw_node *node,
		const struct pw_properties *props)
{
        struct pw_node *peer;
        const char *media, *category, *role, *str;
        bool exclusive;
        struct find_data find;
        enum pw_direction direction;
	struct session *session;
	struct node_info *info;
	uint32_t sample_rate, buffer_size;
	int res;

	str = pw_properties_get(props, PW_NODE_PROP_AUTOCONNECT);
        if (str == NULL || !pw_properties_parse_bool(str))
		return 0;

	if ((media = pw_properties_get(props, PW_NODE_PROP_MEDIA)) == NULL)
		media = "Audio";

	if ((category = pw_properties_get(props, PW_NODE_PROP_CATEGORY)) == NULL) {
		if (node->info.n_input_ports > 0 && node->info.n_output_ports == 0)
			category = "Capture";
		else if (node->info.n_output_ports > 0 && node->info.n_input_ports == 0)
			category = "Playback";
		else
			return -EINVAL;
	}

	if ((role = pw_properties_get(props, PW_NODE_PROP_ROLE)) == NULL)
		role = "Music";

	if ((str = pw_properties_get(props, PW_NODE_PROP_EXCLUSIVE)) != NULL)
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

	str = pw_properties_get(props, PW_NODE_PROP_TARGET_NODE);
	if (str != NULL)
		find.path_id = atoi(str);
	else
		find.path_id = SPA_ID_INVALID;


	pw_log_debug("module %p: try to find and link to node '%d'", impl, find.path_id);

	find.impl = impl;
	find.sess = NULL;
	find.plugged = 0;
	find.exclusive = exclusive;
	spa_list_for_each(session, &impl->session_list, l)
		find_session(&find, session);
	if (find.sess == NULL)
		return -ENOENT;

	session = find.sess;

	sample_rate = session->sample_rate;
	buffer_size = session->buffer_size;

	if ((str = pw_properties_get(props, "node.latency")) != NULL) {
		uint32_t num, denom;
		pw_log_info("module %p: '%s'", impl, str);
		if (sscanf(str, "%u/%u", &num, &denom) == 2 && denom != 0) {
			buffer_size = flp2((num * sample_rate / denom) * sizeof(float));
		}
	}

	pw_log_info("module %p: '%s' '%s' '%s' exclusive:%d quantum:%d/%d", impl,
			media, category, role, exclusive,
			sample_rate, buffer_size);

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

	info = calloc(1, sizeof(struct node_info));
	info->impl = impl;
	info->session = session;
	info->node = node;
	info->sample_rate = sample_rate;
	info->buffer_size = buffer_size;
	spa_list_init(&info->links);

	spa_list_append(&session->node_list, &info->l);
	session->busy = true;

	pw_node_add_listener(node, &info->node_listener, &node_info_events, info);

	pw_node_for_each_port(peer, direction, on_peer_port, info);

	reconfigure_session(session);

	return 1;
}

static void node_destroy(void *data)
{
	struct session *sess = data;
	session_destroy(sess);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = node_destroy,
};

static void
dsp_state_changed(void *data, enum pw_node_state old,
		enum pw_node_state state, const char *error)
{
	struct session *sess = data;

	switch(state) {
	case PW_NODE_STATE_RUNNING:
		if (sess->link == NULL) {
			if (link_session_dsp(sess) < 0)
				return;
			pw_link_activate(sess->link);
		}
		break;
	case PW_NODE_STATE_SUSPENDED:
		if (sess->link != NULL) {
			pw_link_destroy(sess->link);
			sess->link = NULL;
		}
		break;
	default:
		break;
	}
}

static const struct pw_node_events dsp_events = {
	PW_VERSION_NODE_EVENTS,
	.state_changed = dsp_state_changed,
};

struct channel_data {
	struct impl *impl;
	uint32_t channels;
	uint32_t rate;
};

static int collect_audio_format(void *data, uint32_t id,
		uint32_t index, uint32_t next, struct spa_pod *param)
{
	struct channel_data *d = data;
	struct impl *impl = d->impl;
	uint32_t media_type, media_subtype;
	struct spa_audio_info_raw info;

	spa_pod_object_parse(param,
			"I", &media_type,
			"I", &media_subtype);

	if (media_type != impl->type.media_type.audio ||
	    media_subtype != impl->type.media_subtype.raw)
		return 0;

        spa_pod_fixate(param);
        spa_debug_pod(param, SPA_DEBUG_FLAG_FORMAT);

	if (spa_format_audio_raw_parse(param, &info, &impl->type.format_audio) < 0)
		return 0;

	if (info.channels > d->channels) {
		d->channels = info.channels;
		d->rate = info.rate;
	}
	return 0;
}


static int find_port_format(struct impl *impl, struct pw_port *port,
		uint32_t *channels, uint32_t *rate)
{
	struct pw_type *t = impl->t;
	struct channel_data data = { impl, 0, 0 };

	pw_port_for_each_param(port,
			t->param.idEnumFormat,
			0, 0, NULL,
			collect_audio_format, &data);

	pw_log_debug("port channels %d rate %d", data.channels, data.rate);

	*channels = data.channels;
	*rate = data.rate;

	return data.channels > 0 ? 0 : -1;
}

static int on_global(void *data, struct pw_global *global)
{
	struct impl *impl = data;
	struct pw_node *node, *dsp;
	struct session *sess;
	const struct pw_properties *properties;
	const char *str;
	enum pw_direction direction;
	struct pw_port *node_port, *dsp_port;
	uint32_t id, channels, rate;
	bool need_dsp;
	uint64_t plugged;

	if (pw_global_get_type(global) != impl->t->node)
		return 0;

	node = pw_global_get_object(global);
	id = pw_global_get_id(global);

	pw_log_debug("global added %d", id);

	properties = pw_node_get_properties(node);

	if ((str = pw_properties_get(properties, "node.plugged")) != NULL)
		plugged = pw_properties_parse_uint64(str);
	else
		plugged = impl->now.tv_sec * SPA_NSEC_PER_SEC + impl->now.tv_nsec;

	if (handle_autoconnect(impl, node, properties) == 1) {
		return 0;
	}
	else if ((str = pw_properties_get(properties, "media.class")) == NULL)
		return 0;

	if (strstr(str, "Audio/") == str) {
		need_dsp = true;
		str += strlen("Audio/");
	}
	else if (strstr(str, "Video/") == str) {
		need_dsp = false;
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

	if ((node_port = pw_node_get_free_port(node, pw_direction_reverse(direction))) == NULL)
		return 0;

	sess = calloc(1, sizeof(struct session));
	sess->impl = impl;
	sess->direction = direction;
	sess->id = id;
	sess->node = node;
	sess->node_port = node_port;
	sess->plugged = plugged;

	spa_list_init(&sess->node_list);
	spa_list_append(&impl->session_list, &sess->l);
	pw_log_debug("new session %p for node %d", sess, id);

	pw_node_add_listener(node, &sess->node_listener, &node_events, sess);

	if (need_dsp) {
		if (find_port_format(impl, node_port, &channels, &rate) < 0)
			return 0;

		dsp = pw_audio_dsp_new(impl->core,
				properties,
				direction,
				channels,
				rate,
				MAX_BUFFER_SIZE,
				0);
		if (dsp == NULL)
			return 0;

		if ((dsp_port = pw_node_get_free_port(dsp, direction)) == NULL)
			return 0;

		pw_node_add_listener(dsp, &sess->dsp_listener, &dsp_events, sess);

		sess->dsp = dsp;
		sess->dsp_port = dsp_port;
		sess->sample_rate = rate;
		sess->buffer_size = MAX_BUFFER_SIZE;
		sess->enabled = true;

		pw_node_register(dsp, NULL, pw_module_get_global(impl->module), NULL);
		pw_node_set_active(dsp, true);

	}
	else {
		sess->enabled = true;
	}
	return 0;
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	struct session *s, *t;

	spa_hook_remove(&impl->module_listener);
	spa_hook_remove(&impl->core_listener);

	spa_list_for_each_safe(s, t, &impl->session_list, l)
		session_destroy(s);

	if (impl->properties)
		pw_properties_free(impl->properties);

	free(impl);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
};

static void
core_global_added(void *data, struct pw_global *global)
{
	struct impl *impl = data;
	clock_gettime(CLOCK_MONOTONIC, &impl->now);
	on_global(data, global);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
        .global_added = core_global_added,
};

static int module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

	pw_log_debug("module %p: new", impl);

	impl->core = core;
	impl->t = pw_core_get_type(core);
	impl->module = module;
	impl->properties = properties;

	init_type(&impl->type, core->type.map);

	spa_list_init(&impl->session_list);

	clock_gettime(CLOCK_MONOTONIC, &impl->now);
	pw_core_for_each_global(core, on_global, impl);

	pw_core_add_listener(core, &impl->core_listener, &core_events, impl);
	pw_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
}

int pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
