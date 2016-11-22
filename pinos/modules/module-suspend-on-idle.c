/* Pinos
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

#include "pinos/server/core.h"
#include "pinos/server/module.h"

#define MODULE_URI                            "http://pinos.org/ns/module-suspend-on-idle"
#define MODULE_PREFIX                         MODULE_URI "#"

typedef struct {
  PinosCore       *core;
  PinosProperties *properties;
  PinosGlobal     *global;

  struct {
    uint32_t module;
  } uri;

  PinosListener global_added;
  PinosListener global_removed;
  PinosListener node_state_request;
  PinosListener node_state_changed;

  SpaList node_list;
} ModuleImpl;

typedef struct {
  ModuleImpl  *impl;
  PinosNode   *node;
  SpaList      link;
  SpaSource   *idle_timeout;
} NodeInfo;

static NodeInfo *
find_node_info (ModuleImpl *impl, PinosNode *node)
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
    pinos_loop_destroy_source (info->impl->core->main_loop->loop, info->idle_timeout);
    info->idle_timeout = NULL;
  }
}

static void
idle_timeout (SpaSource *source,
              void      *data)
{
  NodeInfo *info = data;

  pinos_log_debug ("module %p: node %p idle timeout", info->impl, info->node);
  remove_idle_timeout (info);
  pinos_node_set_state (info->node, PINOS_NODE_STATE_SUSPENDED);
}

static void
on_node_state_request (PinosListener  *listener,
                       PinosNode      *node,
                       PinosNodeState  state)
{
  ModuleImpl *impl = SPA_CONTAINER_OF (listener, ModuleImpl, node_state_changed);
  NodeInfo *info;

  if ((info = find_node_info (impl, node)) == NULL)
    return;

  remove_idle_timeout (info);
}

static void
on_node_state_changed (PinosListener  *listener,
                       PinosNode      *node,
                       PinosNodeState  old,
                       PinosNodeState  state)
{
  ModuleImpl *impl = SPA_CONTAINER_OF (listener, ModuleImpl, node_state_changed);
  NodeInfo *info;

  if ((info = find_node_info (impl, node)) == NULL)
    return;

  if (state != PINOS_NODE_STATE_IDLE) {
    remove_idle_timeout (info);
  } else {
    struct timespec value;

    pinos_log_debug ("module %p: node %p became idle", impl, node);
    info->idle_timeout = pinos_loop_add_timer (impl->core->main_loop->loop,
                                              idle_timeout,
                                              info);
    value.tv_sec = 3;
    value.tv_nsec = 0;
    pinos_loop_update_timer (impl->core->main_loop->loop,
                             info->idle_timeout,
                             &value,
                             NULL,
                             false);
  }
}

static void
on_global_added (PinosListener *listener,
                 PinosCore     *core,
                 PinosGlobal   *global)
{
  ModuleImpl *impl = SPA_CONTAINER_OF (listener, ModuleImpl, global_added);

  if (global->type == impl->core->uri.node) {
    PinosNode *node = global->object;
    NodeInfo *info;

    info = calloc (1, sizeof (NodeInfo));
    info->impl = impl;
    info->node = node;
    spa_list_insert (impl->node_list.prev, &info->link);
  }
}

static void
on_global_removed (PinosListener *listener,
                   PinosCore     *core,
                   PinosGlobal   *global)
{
  ModuleImpl *impl = SPA_CONTAINER_OF (listener, ModuleImpl, global_removed);

  if (global->type == impl->core->uri.node) {
    PinosNode *node = global->object;
    NodeInfo *info;

    if ((info = find_node_info (impl, node))) {
      remove_idle_timeout (info);
      spa_list_remove (&info->link);
      free (info);
    }
  }
}


/**
 * module_new:
 * @core: #PinosCore
 * @properties: #PinosProperties
 *
 * Make a new #ModuleImpl object with given @properties
 *
 * Returns: a new #ModuleImpl
 */
static ModuleImpl *
module_new (PinosCore       *core,
            PinosProperties *properties)
{
  ModuleImpl *impl;

  impl = calloc (1, sizeof (ModuleImpl));
  pinos_log_debug ("module %p: new", impl);

  impl->core = core;
  impl->properties = properties;

  spa_list_init (&impl->node_list);

  pinos_signal_add (&core->global_added, &impl->global_added, on_global_added);
  pinos_signal_add (&core->global_removed, &impl->global_removed, on_global_removed);
  pinos_signal_add (&core->node_state_request, &impl->node_state_request, on_node_state_request);
  pinos_signal_add (&core->node_state_changed, &impl->node_state_changed, on_node_state_changed);

  impl->uri.module = spa_id_map_get_id (core->uri.map, MODULE_URI);

  impl->global = pinos_core_add_global (core,
                                        impl->uri.module,
                                        impl);
  return impl;
}

#if 0
static void
module_destroy (ModuleImpl *impl)
{
  pinos_log_debug ("module %p: destroy", impl);

  pinos_global_destroy (impl->global);

  pinos_signal_remove (&impl->node_state_changed);
  free (impl);
}
#endif

bool
pinos__module_init (PinosModule * module, const char * args)
{
  module_new (module->core, NULL);
  return TRUE;
}
