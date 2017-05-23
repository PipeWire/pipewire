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

#include "pipewire/server/core.h"
#include "pipewire/server/module.h"

struct impl {
  struct pw_core       *core;
  struct pw_properties *properties;

  struct pw_listener global_added;
  struct pw_listener global_removed;

  SpaList node_list;
};

typedef struct {
  struct impl    *impl;
  struct pw_node *node;
  SpaList         link;
  struct pw_listener  node_state_request;
  struct pw_listener  node_state_changed;
  SpaSource     *idle_timeout;
} NodeInfo;

static NodeInfo *
find_node_info (struct impl *impl, struct pw_node *node)
{
  NodeInfo *info;

  spa_list_for_each (info, &impl->node_list, link) {
    if (info->node == node)
      return info;
  }
  return NULL;
}

static void
remove_idle_timeout (NodeInfo *info)
{
  if (info->idle_timeout) {
    pw_loop_destroy_source (info->impl->core->main_loop->loop, info->idle_timeout);
    info->idle_timeout = NULL;
  }
}

static void
node_info_free (NodeInfo *info)
{
  spa_list_remove (&info->link);
  remove_idle_timeout (info);
  pw_signal_remove (&info->node_state_request);
  pw_signal_remove (&info->node_state_changed);
  free (info);
}

static void
idle_timeout (SpaLoopUtils *utils,
              SpaSource    *source,
              void         *data)
{
  NodeInfo *info = data;

  pw_log_debug ("module %p: node %p idle timeout", info->impl, info->node);
  remove_idle_timeout (info);
  pw_node_set_state (info->node, PW_NODE_STATE_SUSPENDED);
}

static void
on_node_state_request (struct pw_listener *listener,
                       struct pw_node     *node,
                       enum pw_node_state  state)
{
  NodeInfo *info = SPA_CONTAINER_OF (listener, NodeInfo, node_state_request);
  remove_idle_timeout (info);
}

static void
on_node_state_changed (struct pw_listener *listener,
                       struct pw_node     *node,
                       enum pw_node_state  old,
                       enum pw_node_state  state)
{
  NodeInfo *info = SPA_CONTAINER_OF (listener, NodeInfo, node_state_changed);
  struct impl *impl = info->impl;

  if (state != PW_NODE_STATE_IDLE) {
    remove_idle_timeout (info);
  } else {
    struct timespec value;

    pw_log_debug ("module %p: node %p became idle", impl, node);
    info->idle_timeout = pw_loop_add_timer (impl->core->main_loop->loop,
                                            idle_timeout,
                                            info);
    value.tv_sec = 3;
    value.tv_nsec = 0;
    pw_loop_update_timer (impl->core->main_loop->loop,
                          info->idle_timeout,
                          &value,
                          NULL,
                          false);
  }
}

static void
on_global_added (struct pw_listener *listener,
                 struct pw_core     *core,
                 struct pw_global   *global)
{
  struct impl *impl = SPA_CONTAINER_OF (listener, struct impl, global_added);

  if (global->type == impl->core->type.node) {
    struct pw_node *node = global->object;
    NodeInfo *info;

    info = calloc (1, sizeof (NodeInfo));
    info->impl = impl;
    info->node = node;
    spa_list_insert (impl->node_list.prev, &info->link);
    pw_signal_add (&node->state_request, &info->node_state_request, on_node_state_request);
    pw_signal_add (&node->state_changed, &info->node_state_changed, on_node_state_changed);

    pw_log_debug ("module %p: node %p added", impl, node);
  }
}

static void
on_global_removed (struct pw_listener *listener,
                   struct pw_core     *core,
                   struct pw_global   *global)
{
  struct impl *impl = SPA_CONTAINER_OF (listener, struct impl, global_removed);

  if (global->type == impl->core->type.node) {
    struct pw_node *node = global->object;
    NodeInfo *info;

    if ((info = find_node_info (impl, node)))
      node_info_free (info);

    pw_log_debug ("module %p: node %p removed", impl, node);
  }
}


/**
 * module_new:
 * @core: #struct pw_core
 * @properties: #struct pw_properties
 *
 * Make a new #struct impl object with given @properties
 *
 * Returns: a new #struct impl
 */
static struct impl *
module_new (struct pw_core       *core,
            struct pw_properties *properties)
{
  struct impl *impl;

  impl = calloc (1, sizeof (struct impl));
  pw_log_debug ("module %p: new", impl);

  impl->core = core;
  impl->properties = properties;

  spa_list_init (&impl->node_list);

  pw_signal_add (&core->global_added, &impl->global_added, on_global_added);
  pw_signal_add (&core->global_removed, &impl->global_removed, on_global_removed);

  return impl;
}

#if 0
static void
module_destroy (struct impl *impl)
{
  pw_log_debug ("module %p: destroy", impl);

  pw_global_destroy (impl->global);

  free (impl);
}
#endif

bool
pipewire__module_init (struct pw_module * module, const char * args)
{
  module_new (module->core, NULL);
  return true;
}
