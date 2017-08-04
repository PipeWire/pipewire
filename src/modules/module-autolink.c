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

#include "pipewire/interfaces.h"
#include "pipewire/private.h"
#include "pipewire/core.h"
#include "pipewire/module.h"

struct impl {
	struct pw_core *core;
	struct pw_module *module;
	struct pw_properties *properties;

	struct pw_callback_info core_callbacks;

	struct spa_list node_list;
};

struct node_info {
	struct spa_list l;

	struct impl *impl;
	struct pw_node *node;
	struct pw_callback_info node_callbacks;

	struct pw_link *link;
	struct pw_callback_info link_callbacks;
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

static void node_info_free(struct node_info *info)
{
	spa_list_remove(&info->l);
	pw_callback_remove(&info->node_callbacks);
	pw_callback_remove(&info->link_callbacks);
	free(info);
}

static void try_link_port(struct pw_node *node, struct pw_port *port, struct node_info *info);

static void
link_port_unlinked(void *data, struct pw_port *port)
{
	struct node_info *info = data;
	struct pw_link *link = info->link;
	struct impl *impl = info->impl;

	pw_log_debug("module %p: link %p: port %p unlinked", impl, link, port);
	if (port->direction == PW_DIRECTION_OUTPUT && link->input)
		try_link_port(link->input->node, link->input, info);
}

static void
link_state_changed(void *data, enum pw_link_state old, enum pw_link_state state, const char *error)
{
	struct node_info *info = data;
	struct pw_link *link = info->link;
	struct impl *impl = info->impl;

	switch (state) {
	case PW_LINK_STATE_ERROR:
		{
			struct pw_resource *resource;

			pw_log_debug("module %p: link %p: state error: %s", impl, link, error);

			spa_list_for_each(resource, &link->resource_list, link) {
				pw_core_resource_error(resource->client->core_resource,
						       resource->id, SPA_RESULT_ERROR, error);
			}
			if (info->node->owner) {
				pw_core_resource_error(info->node->owner->client->core_resource,
						       info->node->owner->id,
						       SPA_RESULT_ERROR, error);
			}
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
	struct node_info *info = data;
	struct pw_link *link = info->link;
	struct impl *impl = info->impl;

	pw_log_debug("module %p: link %p destroyed", impl, link);
	pw_callback_remove(&info->link_callbacks);
        spa_list_init(&info->link_callbacks.link);
}

static const struct pw_link_callbacks link_callbacks = {
	PW_VERSION_LINK_CALLBACKS,
	.destroy = link_destroy,
	.port_unlinked = link_port_unlinked,
	.state_changed = link_state_changed,
};

static void try_link_port(struct pw_node *node, struct pw_port *port, struct node_info *info)
{
	struct impl *impl = info->impl;
	struct pw_properties *props;
	const char *str;
	uint32_t path_id;
	char *error = NULL;
	struct pw_link *link;
	struct pw_port *target;

	props = node->properties;
	if (props == NULL) {
		pw_log_debug("module %p: node has no properties", impl);
		return;
	}

	str = pw_properties_get(props, "pipewire.target.node");
	if (str != NULL)
		path_id = atoi(str);
	else {
		str = pw_properties_get(props, "pipewire.autoconnect");
		if (str == NULL || atoi(str) == 0) {
			pw_log_debug("module %p: node does not need autoconnect", impl);
			return;
		}
		path_id = SPA_ID_INVALID;
	}

	pw_log_debug("module %p: try to find and link to node '%d'", impl, path_id);

	target = pw_core_find_port(impl->core, port, path_id, NULL, 0, NULL, &error);
	if (target == NULL)
		goto error;

	if (port->direction == PW_DIRECTION_OUTPUT)
		link = pw_link_new(impl->core, impl->module->global, port, target, NULL, NULL, &error);
	else
		link = pw_link_new(impl->core, impl->module->global, target, port, NULL, NULL, &error);

	if (link == NULL)
		goto error;

	info->link = link;

	pw_link_add_callbacks(link, &info->link_callbacks, &link_callbacks, info);
	pw_link_activate(link);

	return;

      error:
	pw_log_error("module %p: can't link node '%s'", impl, error);
	if (info->node->owner && info->node->owner->client->core_resource) {
		pw_core_resource_error(info->node->owner->client->core_resource,
				       info->node->owner->id, SPA_RESULT_ERROR, error);
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

static void on_node_created(struct pw_node *node, struct node_info *info)
{
	struct pw_port *port;

	spa_list_for_each(port, &node->input_ports, link)
	    node_port_added(info, port);

	spa_list_for_each(port, &node->output_ports, link)
	    node_port_added(info, port);
}

static void
node_state_changed(void *data, enum pw_node_state old, enum pw_node_state state, const char *error)
{
	struct node_info *info = data;

	if (old == PW_NODE_STATE_CREATING && state == PW_NODE_STATE_SUSPENDED)
		on_node_created(info->node, info);
}

static const struct pw_node_callbacks node_callbacks = {
	PW_VERSION_NODE_CALLBACKS,
	.port_added = node_port_added,
	.port_removed = node_port_removed,
	.state_changed = node_state_changed,
};

static void
core_global_added(void *data, struct pw_global *global)
{
	struct impl *impl = data;

	if (global->type == impl->core->type.node) {
		struct pw_node *node = global->object;
		struct node_info *ninfo;

		ninfo = calloc(1, sizeof(struct node_info));
		ninfo->impl = impl;
		ninfo->node = node;

		spa_list_insert(impl->node_list.prev, &ninfo->l);

		pw_node_add_callbacks(node, &ninfo->node_callbacks, &node_callbacks, ninfo);
		spa_list_init(&ninfo->link_callbacks.link);

		pw_log_debug("module %p: node %p added", impl, node);

		if (node->info.state > PW_NODE_STATE_CREATING)
			on_node_created(node, ninfo);
	}
}

static void
core_global_removed(void *data, struct pw_global *global)
{
	struct impl *impl = data;

	if (global->type == impl->core->type.node) {
		struct pw_node *node = global->object;
		struct node_info *ninfo;

		if ((ninfo = find_node_info(impl, node)))
			node_info_free(ninfo);

		pw_log_debug("module %p: node %p removed", impl, node);
	}
}


const struct pw_core_callbacks core_callbacks = {
	PW_VERSION_CORE_CALLBACKS,
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
	struct pw_core *core = module->core;
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	pw_log_debug("module %p: new", impl);

	impl->core = core;
	impl->module = module;
	impl->properties = properties;

	spa_list_init(&impl->node_list);

	pw_core_add_callbacks(core, &impl->core_callbacks, &core_callbacks, impl);

	return impl;
}

#if 0
static void module_destroy(struct impl *impl)
{
	pw_log_debug("module %p: destroy", impl);

	pw_global_destroy(impl->global);

	pw_signal_remove(&impl->global_added);
	pw_signal_remove(&impl->global_removed);
	pw_signal_remove(&impl->port_added);
	pw_signal_remove(&impl->port_removed);
	pw_signal_remove(&impl->port_unlinked);
	pw_signal_remove(&impl->link_state_changed);
	free(impl);
}
#endif

bool pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
