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
#include "pipewire/module.h"

struct impl {
	struct pw_core *core;
	struct pw_type *t;
	struct pw_properties *properties;

	struct pw_callback_info core_callbacks;

	struct spa_list node_list;
};

struct node_info {
	struct spa_list link;
	struct impl *impl;
	struct pw_node *node;
	struct pw_callback_info node_callbacks;
	struct spa_source *idle_timeout;
};

static struct node_info *find_node_info(struct impl *impl, struct pw_node *node)
{
	struct node_info *info;

	spa_list_for_each(info, &impl->node_list, link) {
		if (info->node == node)
			return info;
	}
	return NULL;
}

static void remove_idle_timeout(struct node_info *info)
{
	if (info->idle_timeout) {
		pw_loop_destroy_source(pw_core_get_main_loop(info->impl->core), info->idle_timeout);
		info->idle_timeout = NULL;
	}
}

static void node_info_free(struct node_info *info)
{
	spa_list_remove(&info->link);
	remove_idle_timeout(info);
	pw_callback_remove(&info->node_callbacks);
	free(info);
}

static void idle_timeout(struct spa_loop_utils *utils, struct spa_source *source, void *data)
{
	struct node_info *info = data;

	pw_log_debug("module %p: node %p idle timeout", info->impl, info->node);
	remove_idle_timeout(info);
	pw_node_set_state(info->node, PW_NODE_STATE_SUSPENDED);
}

static void
node_state_request(void *data, enum pw_node_state state)
{
	struct node_info *info = data;
	remove_idle_timeout(info);
}

static void
node_state_changed(void *data, enum pw_node_state old, enum pw_node_state state, const char *error)
{
	struct node_info *info = data;
	struct impl *impl = info->impl;

	if (state != PW_NODE_STATE_IDLE) {
		remove_idle_timeout(info);
	} else {
		struct timespec value;
		struct pw_loop *main_loop = pw_core_get_main_loop(impl->core);

		pw_log_debug("module %p: node %p became idle", impl, info->node);
		info->idle_timeout = pw_loop_add_timer(main_loop, idle_timeout, info);
		value.tv_sec = 3;
		value.tv_nsec = 0;
		pw_loop_update_timer(main_loop, info->idle_timeout, &value, NULL, false);
	}
}

static const struct pw_node_callbacks node_callbacks = {
	PW_VERSION_NODE_CALLBACKS,
	.state_request = node_state_request,
	.state_changed = node_state_changed,
};

static void
core_global_added(void *data, struct pw_global *global)
{
	struct impl *impl = data;

	if (pw_global_get_type(global) == impl->t->node) {
		struct pw_node *node = pw_global_get_object(global);
		struct node_info *info;

		info = calloc(1, sizeof(struct node_info));
		info->impl = impl;
		info->node = node;
		spa_list_insert(impl->node_list.prev, &info->link);

		pw_node_add_callbacks(node, &info->node_callbacks, &node_callbacks, info);

		pw_log_debug("module %p: node %p added", impl, node);
	}
}

static void
core_global_removed(void *data, struct pw_global *global)
{
	struct impl *impl = data;

	if (pw_global_get_type(global) == impl->t->node) {
		struct pw_node *node = pw_global_get_object(global);
		struct node_info *info;

		if ((info = find_node_info(impl, node)))
			node_info_free(info);

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
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	pw_log_debug("module %p: new", impl);

	impl->core = pw_module_get_core(module);
	impl->t = pw_core_get_type(impl->core);
	impl->properties = properties;

	spa_list_init(&impl->node_list);

	pw_core_add_callbacks(impl->core, &impl->core_callbacks, &core_callbacks, impl);

	return impl;
}

#if 0
static void module_destroy(struct impl *impl)
{
	pw_log_debug("module %p: destroy", impl);

	pw_global_destroy(impl->global);

	free(impl);
}
#endif

bool pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
