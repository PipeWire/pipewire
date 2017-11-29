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

static void try_link_port(struct pw_node *node, struct pw_port *port, struct node_info *info);

static void
link_port_unlinked(void *data, struct pw_port *port)
{
	struct link_data *ld = data;
	struct node_info *info = ld->node_info;
	struct pw_link *link = ld->link;
	struct impl *impl = info->impl;
	struct pw_port *input = pw_link_get_input(link);

	pw_log_debug("module %p: link %p: port %p unlinked", impl, link, port);

	if (pw_port_get_direction(port) == PW_DIRECTION_OUTPUT && input)
		try_link_port(pw_port_get_node(input), input, info);
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

static void try_link_port(struct pw_node *node, struct pw_port *port, struct node_info *info)
{
	struct impl *impl = info->impl;
	const struct pw_properties *props;
	const char *str;
	uint32_t path_id;
	char *error = NULL;
	struct pw_link *link;
	struct pw_port *target;
	struct link_data *ld;

	props = pw_node_get_properties(node);

	str = pw_properties_get(props, PW_NODE_PROP_TARGET_NODE);
	if (str != NULL)
		path_id = atoi(str);
	else {
		str = pw_properties_get(props, PW_NODE_PROP_AUTOCONNECT);
		if (str == NULL || !pw_properties_parse_bool(str)) {
			pw_log_debug("module %p: node does not need autoconnect", impl);
			return;
		}
		path_id = SPA_ID_INVALID;
	}

	pw_log_debug("module %p: try to find and link to node '%d'", impl, path_id);

	target = pw_core_find_port(impl->core, port, path_id, NULL, 0, NULL, &error);
	if (target == NULL)
		goto error;

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
		goto error;

	ld = pw_link_get_user_data(link);
	ld->link = link;
	ld->node_info = info;
	pw_link_add_listener(link, &ld->link_listener, &link_events, ld);

	spa_list_append(&info->links, &ld->l);
	pw_link_register(link, NULL, pw_module_get_global(impl->module));

	return;

      error:
	pw_log_error("module %p: can't link node '%s'", impl, error);
	{
		struct pw_global *global = pw_node_get_global(info->node);
		struct pw_client *owner = pw_global_get_owner(global);
		if (owner)
			pw_resource_error(pw_client_get_core_resource(owner), -EINVAL, error);
	}
	free(error);
	return;
}

static void node_port_added(void *data, struct pw_port *port)
{
	struct node_info *info = data;
	try_link_port(info->node, port, info);
}

static void node_port_removed(void *data, struct pw_port *port)
{
}

static int on_node_port_added(void *data, struct pw_port *port)
{
	node_port_added(data, port);
	return 0;
}

static void on_node_created(struct pw_node *node, struct node_info *info)
{
	pw_node_for_each_port(node, PW_DIRECTION_INPUT, on_node_port_added, info);
	pw_node_for_each_port(node, PW_DIRECTION_OUTPUT, on_node_port_added, info);
}

static void
node_state_changed(void *data, enum pw_node_state old, enum pw_node_state state, const char *error)
{
	struct node_info *info = data;

	if (old == PW_NODE_STATE_CREATING && state == PW_NODE_STATE_SUSPENDED)
		on_node_created(info->node, info);
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
			on_node_created(node, ninfo);
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

const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
        .destroy = module_destroy,
};

const struct pw_core_events core_events = {
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
static bool module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	pw_log_debug("module %p: new", impl);

	impl->core = core;
	impl->t = pw_core_get_type(core);
	impl->module = module;
	impl->properties = properties;

	spa_list_init(&impl->node_list);

	pw_core_add_listener(core, &impl->core_listener, &core_events, impl);
	pw_module_add_listener(module, &impl->module_listener, &module_events, impl);

	return impl;
}

bool pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
