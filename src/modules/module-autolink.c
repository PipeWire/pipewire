/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#include "config.h"

#include "pipewire/core.h"
#include "pipewire/interfaces.h"
#include "pipewire/link.h"
#include "pipewire/log.h"
#include "pipewire/module.h"
#include "pipewire/control.h"
#include "pipewire/private.h"

struct impl {
	struct pw_core *core;
	struct pw_type *t;
	struct pw_module *module;
	struct pw_properties *properties;

	struct spa_hook core_listener;
	struct spa_hook module_listener;

	struct spa_list node_list;
};

struct node_info {
	struct spa_list l;

	struct impl *impl;
	struct pw_node *node;
	struct spa_hook node_listener;

	struct spa_list links;
};

struct link_data {
	struct spa_list l;

	struct node_info *node_info;
	struct pw_link *link;
	struct spa_hook link_listener;
};

static struct node_info *find_node_info(struct impl *impl, struct pw_node *node)
{
	struct node_info *info;

	spa_list_for_each(info, &impl->node_list, l) {
		if (info->node == node)
			return info;
	}
	return NULL;
}

static void link_data_remove(struct link_data *data)
{
	if (data->node_info) {
		spa_list_remove(&data->l);
		spa_hook_remove(&data->link_listener);
		data->node_info = NULL;
	}
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

static void node_port_added(void *data, struct pw_port *port)
{
}

static void node_port_removed(void *data, struct pw_port *port)
{
}

#if 0
static int on_node_port_added(void *data, struct pw_port *port)
{
	node_port_added(data, port);
	return 0;
}
#endif

static int on_peer_port(void *data, struct pw_port *port)
{
	struct node_info *info = data;
	struct pw_port *p;

	p = pw_node_get_free_port(info->node, pw_direction_reverse(port->direction));
	if (p == NULL)
		return 0;

	return link_ports(info, p, port);
}

struct find_data {
	struct node_info *info;
	uint32_t path_id;
	const char *media_class;
	struct pw_global *global;
	uint64_t plugged;
};

static int find_global(void *data, struct pw_global *global)
{
	struct find_data *find = data;
	struct node_info *info = find->info;
	struct impl *impl = info->impl;
	struct pw_node *node;
	const struct pw_properties *props;
	const char *str;
	uint64_t plugged;

	if (pw_global_get_type(global) != impl->t->node)
		return 0;

	pw_log_debug("module %p: looking at node '%d'", impl, pw_global_get_id(global));

	node = pw_global_get_object(global);

	if ((props = pw_node_get_properties(node)) == NULL)
		return 0;

	if ((str = pw_properties_get(props, "media.class")) == NULL)
		return 0;

	if (strcmp(str, find->media_class) != 0)
		return 0;

	if (find->path_id != SPA_ID_INVALID && global->id != find->path_id)
		return 0;

	if ((str = pw_properties_get(props, "node.plugged")) != NULL)
		plugged = pw_properties_parse_uint64(str);
	else
		plugged = 0;

	pw_log_debug("module %p: found node '%d' %" PRIu64, impl,
			pw_global_get_id(global), plugged);

	if (find->global == NULL || plugged > find->plugged) {
		pw_log_debug("module %p: new best %" PRIu64, impl, plugged);
		find->global = global;
		find->plugged = plugged;
	}
	return 0;
}

static void on_node_created(struct node_info *info)
{
	struct impl *impl = info->impl;
	const struct pw_properties *props;
	struct pw_node *peer;
	const char *media, *category, *role, *str;
	bool exclusive;
	struct find_data find;
	enum pw_direction direction;

	find.info = info;

	props = pw_node_get_properties(info->node);
	if (props == NULL)
		return;

	str = pw_properties_get(props, PW_NODE_PROP_AUTOCONNECT);
	if (str == NULL || !pw_properties_parse_bool(str))
		return;

	if ((media = pw_properties_get(props, PW_NODE_PROP_MEDIA)) == NULL)
		media = "Audio";

	if ((category = pw_properties_get(props, PW_NODE_PROP_CATEGORY)) == NULL) {
		if (strcmp(media, "Video") == 0)
			category = "Capture";
		else
			category = "Playback";
	}

	if ((role = pw_properties_get(props, PW_NODE_PROP_ROLE)) == NULL) {
		if (strcmp(media, "Audio") == 0)
			role = "Music";
		else if (strcmp(media, "Video") == 0)
			role = "Camera";

	}
	if ((str = pw_properties_get(props, PW_NODE_PROP_EXCLUSIVE)) != NULL)
		exclusive = pw_properties_parse_bool(str);
	else
		exclusive = false;

	pw_log_debug("module %p: '%s' '%s' '%s' %d", impl, media, category, role, exclusive);

	if (strcmp(media, "Audio") == 0) {
		if (strcmp(category, "Playback") == 0)
			find.media_class = exclusive ? "Audio/Sink" : "Audio/DSP/Playback";
		else if (strcmp(category, "Capture") == 0)
			find.media_class = exclusive ? "Audio/Source" : "Audio/DSP/Capture";
		else
			return;
	}
	else if (strcmp(media, "Video") == 0) {
		if (strcmp(category, "Capture") == 0)
			find.media_class = "Video/Source";
		else
			return;
	}
	else
		return;

	str = pw_properties_get(props, PW_NODE_PROP_TARGET_NODE);
	if (str != NULL)
		find.path_id = atoi(str);
	else
		find.path_id = SPA_ID_INVALID;

	pw_log_debug("module %p: try to find and link to node '%d'", impl, find.path_id);

	find.global = NULL;
	if (pw_core_for_each_global(impl->core, find_global, &find) < 0)
		return;

	if (find.global == NULL)
		return;

	peer = pw_global_get_object(find.global);

	if (strcmp(category, "Capture") == 0)
		direction = PW_DIRECTION_OUTPUT;
	if (strcmp(category, "Playback") == 0)
		direction = PW_DIRECTION_INPUT;
	else
		return;

	pw_node_for_each_port(peer, direction, on_peer_port, info);
}

static void
node_state_changed(void *data, enum pw_node_state old, enum pw_node_state state, const char *error)
{
	struct node_info *info = data;

	if (old == PW_NODE_STATE_CREATING && state == PW_NODE_STATE_SUSPENDED)
		on_node_created(info);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.port_added = node_port_added,
	.port_removed = node_port_removed,
	.state_changed = node_state_changed,
};

static void
core_global_added(void *data, struct pw_global *global)
{
	struct impl *impl = data;

	if (pw_global_get_type(global) == impl->t->node) {
		struct pw_node *node = pw_global_get_object(global);
		struct node_info *ninfo;

		ninfo = calloc(1, sizeof(struct node_info));
		ninfo->impl = impl;
		ninfo->node = node;
		spa_list_init(&ninfo->links);

		spa_list_append(&impl->node_list, &ninfo->l);
		pw_node_add_listener(node, &ninfo->node_listener, &node_events, ninfo);

		pw_log_debug("module %p: node %p added", impl, node);

		if (pw_node_get_info(node)->state > PW_NODE_STATE_CREATING)
			on_node_created(ninfo);
	}
}

static void
core_global_removed(void *data, struct pw_global *global)
{
	struct impl *impl = data;

	if (pw_global_get_type(global) == impl->t->node) {
		struct pw_node *node = pw_global_get_object(global);
		struct node_info *ninfo;

		if ((ninfo = find_node_info(impl, node)))
			node_info_free(ninfo);

		pw_log_debug("module %p: node %p removed", impl, node);
	}
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	struct node_info *info, *t;

	spa_list_for_each_safe(info, t, &impl->node_list, l)
		node_info_free(info);

	spa_hook_remove(&impl->core_listener);
	spa_hook_remove(&impl->module_listener);

	if (impl->properties)
		pw_properties_free(impl->properties);

	free(impl);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
        .destroy = module_destroy,
};

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
        .global_added = core_global_added,
        .global_removed = core_global_removed,
};

/**
 * module_new:
 * @core: #struct pw_core
 * @properties: #struct pw_properties
 *
 * Make a new #struct impl object with given @properties
 *
 * Returns: a new #struct impl
 */
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

	spa_list_init(&impl->node_list);

	pw_core_add_listener(core, &impl->core_listener, &core_events, impl);
	pw_module_add_listener(module, &impl->module_listener, &module_events, impl);

	return 0;
}

int pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
