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

#include "pinos/client/interfaces.h"
#include "pinos/server/core.h"
#include "pinos/server/module.h"
#include "pinos/server/client-node.h"

typedef struct {
  PinosCore       *core;
  PinosProperties *properties;

  PinosListener global_added;
  PinosListener global_removed;

  SpaList node_list;
  SpaList client_list;
} ModuleImpl;

typedef struct {
  ModuleImpl    *impl;
  PinosClient   *client;
  SpaList        link;
  PinosListener  resource_added;
  PinosListener  resource_removed;
} ClientInfo;

typedef struct {
  ModuleImpl    *impl;
  ClientInfo    *info;
  PinosNode     *node;
  PinosResource *resource;
  SpaList        link;
  PinosListener  state_changed;
  PinosListener  port_added;
  PinosListener  port_removed;
  PinosListener  port_unlinked;
  PinosListener  link_state_changed;
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

static ClientInfo *
find_client_info (ModuleImpl *impl, PinosClient *client)
{
  ClientInfo *info;

  spa_list_for_each (info, &impl->client_list, link) {
    if (info->client == client)
      return info;
  }
  return NULL;
}

static void
node_info_free (NodeInfo *info)
{
  spa_list_remove (&info->link);
  pinos_signal_remove (&info->state_changed);
  pinos_signal_remove (&info->port_added);
  pinos_signal_remove (&info->port_removed);
  pinos_signal_remove (&info->port_unlinked);
  pinos_signal_remove (&info->link_state_changed);
  free (info);
}

static void
client_info_free (ClientInfo *cinfo)
{
  spa_list_remove (&cinfo->link);
  pinos_signal_remove (&cinfo->resource_added);
  pinos_signal_remove (&cinfo->resource_removed);
  free (cinfo);
}

static void try_link_port (PinosNode *node, PinosPort *port, NodeInfo  *info);

static void
on_link_port_unlinked (PinosListener *listener,
                       PinosLink     *link,
                       PinosPort     *port)
{
  NodeInfo *info = SPA_CONTAINER_OF (listener, NodeInfo, port_unlinked);
  ModuleImpl *impl = info->impl;

  pinos_log_debug ("module %p: link %p: port %p unlinked", impl, link, port);
  if (port->direction == PINOS_DIRECTION_OUTPUT && link->input)
    try_link_port (link->input->node, link->input, info);
}

static void
on_link_state_changed (PinosListener  *listener,
                       PinosLink      *link,
                       PinosLinkState  old,
                       PinosLinkState  state)
{
  NodeInfo *info = SPA_CONTAINER_OF (listener, NodeInfo, link_state_changed);
  ModuleImpl *impl = info->impl;

  switch (state) {
    case PINOS_LINK_STATE_ERROR:
    {
      PinosResource *resource;

      pinos_log_debug ("module %p: link %p: state error: %s", impl, link, link->error);

      spa_list_for_each (resource, &link->resource_list, link) {
        pinos_core_notify_error (resource->client->core_resource,
                                 resource->id,
                                 SPA_RESULT_ERROR,
                                 link->error);
      }
      if (info->info->client) {
        pinos_core_notify_error (info->info->client->core_resource,
                                 info->resource->id,
                                 SPA_RESULT_ERROR,
                                 link->error);
      }
      break;
    }

    case PINOS_LINK_STATE_UNLINKED:
      pinos_log_debug ("module %p: link %p: unlinked", impl, link);
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
try_link_port (PinosNode *node,
               PinosPort *port,
               NodeInfo  *info)
{
  ModuleImpl *impl = info->impl;
  PinosProperties *props;
  const char *path;
  uint32_t path_id;
  char *error = NULL;
  PinosLink *link;
  PinosPort *target;

  props = node->properties;
  if (props == NULL) {
    pinos_log_debug ("module %p: node has no properties", impl);
    return;
  }

  path = pinos_properties_get (props, "pinos.target.node");
  if (path == NULL)
    path_id = SPA_ID_INVALID;
  else
    path_id = atoi (path);

  pinos_log_debug ("module %p: try to find and link to node '%s'", impl, path);

  target = pinos_core_find_port (impl->core,
                                 port,
                                 path_id,
                                 NULL,
                                 0,
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

  pinos_signal_add (&link->port_unlinked, &info->port_unlinked, on_link_port_unlinked);
  pinos_signal_add (&link->state_changed, &info->link_state_changed, on_link_state_changed);

  pinos_link_activate (link);

  return;

error:
  {
    pinos_log_error ("module %p: can't link node '%s'", impl, error);
    if (info->info->client && info->info->client->core_resource) {
      pinos_core_notify_error (info->info->client->core_resource,
                               info->resource->id,
                               SPA_RESULT_ERROR,
                               error);
    }
    free (error);
    return;
  }
}

static void
on_port_added (PinosListener *listener,
               PinosNode     *node,
               PinosPort     *port)
{
  NodeInfo *info = SPA_CONTAINER_OF (listener, NodeInfo, port_added);

  try_link_port (node, port, info);
}

static void
on_port_removed (PinosListener *listener,
                 PinosNode     *node,
                 PinosPort     *port)
{
}

static void
on_node_created (PinosNode  *node,
                 NodeInfo   *info)
{
  PinosPort *port;

  spa_list_for_each (port, &node->input_ports, link)
    on_port_added (&info->port_added, node, port);

  spa_list_for_each (port, &node->output_ports, link)
    on_port_added (&info->port_added, node, port);
}

static void
on_state_changed (PinosListener  *listener,
                  PinosNode      *node,
                  PinosNodeState  old,
                  PinosNodeState  state)
{
  NodeInfo *info = SPA_CONTAINER_OF (listener, NodeInfo, state_changed);

  if (old == PINOS_NODE_STATE_CREATING && state == PINOS_NODE_STATE_SUSPENDED)
    on_node_created (node, info);
}

static void
on_node_added (ModuleImpl    *impl,
               PinosNode     *node,
               PinosResource *resource,
               ClientInfo    *cinfo)
{
  NodeInfo *info;

  info = calloc (1, sizeof (NodeInfo));
  info->impl = impl;
  info->node = node;
  info->resource = resource;
  info->info = cinfo;
  spa_list_insert (impl->node_list.prev, &info->link);

  spa_list_init (&info->port_unlinked.link);
  spa_list_init (&info->link_state_changed.link);
  pinos_signal_add (&node->port_added, &info->port_added, on_port_added);
  pinos_signal_add (&node->port_removed, &info->port_removed, on_port_removed);
  pinos_signal_add (&node->state_changed, &info->state_changed, on_state_changed);

  pinos_log_debug ("module %p: node %p added", impl, node);

  if (node->state > PINOS_NODE_STATE_CREATING)
    on_node_created (node, info);
}

static void
on_resource_added (PinosListener *listener,
                   PinosClient   *client,
                   PinosResource *resource)
{
  ClientInfo *cinfo = SPA_CONTAINER_OF (listener, ClientInfo, resource_added);
  ModuleImpl *impl = cinfo->impl;

  if (resource->type == impl->core->type.client_node) {
    PinosClientNode *cnode = resource->object;
    on_node_added (impl, cnode->node, resource, cinfo);
  }
}

static void
on_resource_removed (PinosListener *listener,
                     PinosClient   *client,
                     PinosResource *resource)
{
  ClientInfo *cinfo = SPA_CONTAINER_OF (listener, ClientInfo, resource_removed);
  ModuleImpl *impl = cinfo->impl;

  if (resource->type == impl->core->type.client_node) {
    PinosClientNode *cnode = resource->object;
    NodeInfo *ninfo;

    if ((ninfo = find_node_info (impl, cnode->node)))
      node_info_free (ninfo);

    pinos_log_debug ("module %p: node %p removed", impl, cnode->node);
  }
}

static void
on_global_added (PinosListener *listener,
                 PinosCore     *core,
                 PinosGlobal   *global)
{
  ModuleImpl *impl = SPA_CONTAINER_OF (listener, ModuleImpl, global_added);

  if (global->type == impl->core->type.client) {
    PinosClient *client = global->object;
    ClientInfo *cinfo;

    cinfo = calloc (1, sizeof (ClientInfo));
    cinfo->impl = impl;
    cinfo->client = global->object;
    spa_list_insert (impl->client_list.prev, &cinfo->link);

    pinos_signal_add (&client->resource_added, &cinfo->resource_added, on_resource_added);
    pinos_signal_add (&client->resource_removed, &cinfo->resource_removed, on_resource_removed);

    pinos_log_debug ("module %p: client %p added", impl, cinfo->client);
  }
}

static void
on_global_removed (PinosListener *listener,
                   PinosCore     *core,
                   PinosGlobal   *global)
{
  ModuleImpl *impl = SPA_CONTAINER_OF (listener, ModuleImpl, global_removed);

  if (global->type == impl->core->type.client) {
    PinosClient *client = global->object;
    ClientInfo *cinfo;

    if ((cinfo = find_client_info (impl, client)))
      client_info_free (cinfo);

    pinos_log_debug ("module %p: client %p removed", impl, client);
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
  spa_list_init (&impl->client_list);

  pinos_signal_add (&core->global_added, &impl->global_added, on_global_added);
  pinos_signal_add (&core->global_removed, &impl->global_removed, on_global_removed);

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
  module->user_data = module_new (module->core, NULL);
  return true;
}
