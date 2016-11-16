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

#define MODULE_URI                            "http://pinos.org/ns/module-autolink"
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
  PinosListener port_added;
  PinosListener port_removed;
  PinosListener port_unlinked;
  PinosListener node_state_changed;
  PinosListener link_state_changed;
} ModuleImpl;

static void
try_link_port (PinosNode *node, PinosPort *port, ModuleImpl *impl)
{
  //PinosClient *client;
  PinosProperties *props;
  const char *path;
  char *error = NULL;
  PinosLink *link;

  props = node->properties;
  if (props == NULL)
    return;

  path = pinos_properties_get (props, "pinos.target.node");

  pinos_log_debug ("module %p: try to find and link to node %s", impl, path);

  if (path) {
    PinosPort *target;

    target = pinos_core_find_port (impl->core,
                                   port,
                                   atoi (path),
                                   NULL,
                                   NULL,
                                   &error);
    if (target == NULL)
      goto error;

    if (port->direction == PINOS_DIRECTION_OUTPUT)
      link = pinos_port_link (port, target, NULL, NULL, &error);
    else
      link = pinos_port_link (target, port, NULL, NULL, &error);

    if (link == NULL)
      goto error;

#if 0
    client = pinos_node_get_client (node);
    if (client)
      pinos_client_add_object (client, &link->object);
#endif

    pinos_link_activate (link);
  }
  return;

error:
  {
    pinos_node_report_error (node, error);
    return;
  }
}


static void
on_link_port_unlinked (PinosListener *listener,
                       PinosLink     *link,
                       PinosPort     *port)
{
  ModuleImpl *impl = SPA_CONTAINER_OF (listener, ModuleImpl, port_unlinked);

  pinos_log_debug ("module %p: link %p: port %p unlinked", impl, link, port);
  if (port->direction == PINOS_DIRECTION_OUTPUT && link->input)
    try_link_port (link->input->node, link->input, impl);
}

static void
on_link_state_changed (PinosListener *listener,
                       PinosLink     *link)
{
  ModuleImpl *impl = SPA_CONTAINER_OF (listener, ModuleImpl, link_state_changed);
  PinosLinkState state;

  state = link->state;
  switch (state) {
    case PINOS_LINK_STATE_ERROR:
    {
      pinos_log_debug ("module %p: link %p: state error: %s", impl, link, link->error);

      if (link->input && link->input->node)
        pinos_node_report_error (link->input->node, strdup (link->error));
      if (link->output && link->output->node)
        pinos_node_report_error (link->output->node, strdup (link->error));
      break;
    }

    case PINOS_LINK_STATE_UNLINKED:
      pinos_log_debug ("module %p: link %p: unlinked", impl, link);

#if 0
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_NODE_LINK,
                   "error node unlinked");

      if (link->input && link->input->node)
        pinos_node_report_error (link->input->node, g_error_copy (error));
      if (link->output && link->output->node)
        pinos_node_report_error (link->output->node, g_error_copy (error));
#endif
      break;

    case PINOS_LINK_STATE_INIT:
    case PINOS_LINK_STATE_NEGOTIATING:
    case PINOS_LINK_STATE_ALLOCATING:
    case PINOS_LINK_STATE_PAUSED:
    case PINOS_LINK_STATE_RUNNING:
      break;
  }
}

static void
on_port_added (PinosListener *listener,
               PinosNode     *node,
               PinosPort     *port)
{
  ModuleImpl *impl = SPA_CONTAINER_OF (listener, ModuleImpl, port_added);

  try_link_port (node, port, impl);
}

static void
on_port_removed (PinosListener *listener,
                 PinosNode     *node,
                 PinosPort     *port)
{
}

static void
on_node_created (PinosNode       *node,
                 ModuleImpl *impl)
{
  PinosPort *port;

  spa_list_for_each (port, &node->input_ports, link)
    on_port_added (&impl->port_added, node, port);

  spa_list_for_each (port, &node->output_ports, link)
    on_port_added (&impl->port_added, node, port);
}

static void
on_node_state_changed (PinosListener  *listener,
                       PinosNode      *node,
                       PinosNodeState  old,
                       PinosNodeState  state)
{
  ModuleImpl *impl = SPA_CONTAINER_OF (listener, ModuleImpl, node_state_changed);

  pinos_log_debug ("module %p: node %p state change %s -> %s", impl, node,
                        pinos_node_state_as_string (old),
                        pinos_node_state_as_string (state));

  if (old == PINOS_NODE_STATE_CREATING && state == PINOS_NODE_STATE_SUSPENDED)
    on_node_created (node, impl);
}

static void
on_node_added (ModuleImpl *impl, PinosNode *node)
{
  pinos_log_debug ("module %p: node %p added", impl, node);

  if (node->state > PINOS_NODE_STATE_CREATING)
    on_node_created (node, impl);
}

static void
on_node_removed (ModuleImpl *impl, PinosNode *node)
{
  pinos_log_debug ("module %p: node %p removed", impl, node);
}

static void
on_global_added (PinosListener *listener,
                 PinosCore     *core,
                 PinosGlobal   *global)
{
  ModuleImpl *impl = SPA_CONTAINER_OF (listener, ModuleImpl, global_added);

  if (global->type == impl->core->registry.uri.node) {
    PinosNode *node = global->object;
    on_node_added (impl, node);
  }
}

static void
on_global_removed (PinosListener *listener,
                   PinosCore     *core,
                   PinosGlobal   *global)
{
  ModuleImpl *impl = SPA_CONTAINER_OF (listener, ModuleImpl, global_removed);

  if (global->type == impl->core->registry.uri.node) {
    PinosNode *node = global->object;
    on_node_removed (impl, node);
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

  pinos_signal_add (&core->global_added, &impl->global_added, on_global_added);
  pinos_signal_add (&core->global_removed, &impl->global_removed, on_global_removed);
  pinos_signal_add (&core->node_state_changed, &impl->node_state_changed, on_node_state_changed);
  pinos_signal_add (&core->port_added, &impl->port_added, on_port_added);
  pinos_signal_add (&core->port_removed, &impl->port_removed, on_port_removed);
  pinos_signal_add (&core->port_unlinked, &impl->port_unlinked, on_link_port_unlinked);
  pinos_signal_add (&core->link_state_changed, &impl->link_state_changed, on_link_state_changed);

  impl->uri.module = spa_id_map_get_id (core->registry.map, MODULE_URI);

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

  pinos_signal_remove (&impl->global_added);
  pinos_signal_remove (&impl->global_removed);
  pinos_signal_remove (&impl->node_state_changed);
  pinos_signal_remove (&impl->port_added);
  pinos_signal_remove (&impl->port_removed);
  pinos_signal_remove (&impl->port_unlinked);
  pinos_signal_remove (&impl->link_state_changed);
  free (impl);
}
#endif

bool
pinos__module_init (PinosModule * module, const char * args)
{
  module_new (module->core, NULL);
  return TRUE;
}
