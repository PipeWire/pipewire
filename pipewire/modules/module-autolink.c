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

#include "pipewire/client/interfaces.h"
#include "pipewire/server/core.h"
#include "pipewire/server/module.h"
#include "pipewire/server/client-node.h"

struct impl {
  struct pw_core       *core;
  struct pw_properties *properties;

  struct pw_listener global_added;
  struct pw_listener global_removed;

  struct spa_list client_list;
};

struct client_info {
  struct impl        *impl;
  struct pw_client   *client;
  struct spa_list     link;
  struct pw_listener  resource_added;
  struct pw_listener  resource_removed;
  struct spa_list     node_list;
};

struct node_info {
  struct impl        *impl;
  struct client_info *info;
  struct pw_node     *node;
  struct pw_resource *resource;
  struct spa_list     link;
  struct pw_listener  state_changed;
  struct pw_listener  port_added;
  struct pw_listener  port_removed;
  struct pw_listener  port_unlinked;
  struct pw_listener  link_state_changed;
};

static struct node_info *
find_node_info (struct client_info *cinfo, struct pw_node *node)
{
  struct node_info *info;

  spa_list_for_each (info, &cinfo->node_list, link) {
    if (info->node == node)
      return info;
  }
  return NULL;
}

static struct client_info *
find_client_info (struct impl *impl, struct pw_client *client)
{
  struct client_info *info;

  spa_list_for_each (info, &impl->client_list, link) {
    if (info->client == client)
      return info;
  }
  return NULL;
}

static void
node_info_free (struct node_info *info)
{
  spa_list_remove (&info->link);
  pw_signal_remove (&info->state_changed);
  pw_signal_remove (&info->port_added);
  pw_signal_remove (&info->port_removed);
  pw_signal_remove (&info->port_unlinked);
  pw_signal_remove (&info->link_state_changed);
  free (info);
}

static void
client_info_free (struct client_info *cinfo)
{
  struct node_info *info, *tmp;

  spa_list_remove (&cinfo->link);
  pw_signal_remove (&cinfo->resource_added);
  pw_signal_remove (&cinfo->resource_removed);

  spa_list_for_each_safe (info, tmp, &cinfo->node_list, link)
    node_info_free (info);

  free (cinfo);
}

static void try_link_port (struct pw_node *node, struct pw_port *port, struct node_info  *info);

static void
on_link_port_unlinked (struct pw_listener *listener,
                       struct pw_link     *link,
                       struct pw_port     *port)
{
  struct node_info *info = SPA_CONTAINER_OF (listener, struct node_info, port_unlinked);
  struct impl *impl = info->impl;

  pw_log_debug ("module %p: link %p: port %p unlinked", impl, link, port);
  if (port->direction == PW_DIRECTION_OUTPUT && link->input)
    try_link_port (link->input->node, link->input, info);
}

static void
on_link_state_changed (struct pw_listener  *listener,
                       struct pw_link      *link,
                       enum pw_link_state   old,
                       enum pw_link_state   state)
{
  struct node_info *info = SPA_CONTAINER_OF (listener, struct node_info, link_state_changed);
  struct impl *impl = info->impl;

  switch (state) {
    case PW_LINK_STATE_ERROR:
    {
      struct pw_resource *resource;

      pw_log_debug ("module %p: link %p: state error: %s", impl, link, link->error);

      spa_list_for_each (resource, &link->resource_list, link) {
        pw_core_notify_error (resource->client->core_resource,
                                 resource->id,
                                 SPA_RESULT_ERROR,
                                 link->error);
      }
      if (info->info->client) {
        pw_core_notify_error (info->info->client->core_resource,
                                 info->resource->id,
                                 SPA_RESULT_ERROR,
                                 link->error);
      }
      break;
    }

    case PW_LINK_STATE_UNLINKED:
      pw_log_debug ("module %p: link %p: unlinked", impl, link);
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
try_link_port (struct pw_node *node,
               struct pw_port *port,
               struct node_info  *info)
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
    pw_log_debug ("module %p: node has no properties", impl);
    return;
  }

  str = pw_properties_get (props, "pipewire.target.node");
  if (str != NULL)
    path_id = atoi (str);
  else {
    str = pw_properties_get (props, "pipewire.autoconnect");
    if (str == NULL || atoi (str) == 0) {
      pw_log_debug ("module %p: node does not need autoconnect", impl);
      return;
    }
    path_id = SPA_ID_INVALID;
  }

  pw_log_debug ("module %p: try to find and link to node '%d'", impl, path_id);

  target = pw_core_find_port (impl->core,
                                 port,
                                 path_id,
                                 NULL,
                                 0,
                                 NULL,
                                 &error);
  if (target == NULL)
    goto error;

  if (port->direction == PW_DIRECTION_OUTPUT)
    link = pw_port_link (port, target, NULL, NULL, &error);
  else
    link = pw_port_link (target, port, NULL, NULL, &error);

  if (link == NULL)
    goto error;

  pw_signal_add (&link->port_unlinked, &info->port_unlinked, on_link_port_unlinked);
  pw_signal_add (&link->state_changed, &info->link_state_changed, on_link_state_changed);

  pw_link_activate (link);

  return;

error:
  {
    pw_log_error ("module %p: can't link node '%s'", impl, error);
    if (info->info->client && info->info->client->core_resource) {
      pw_core_notify_error (info->info->client->core_resource,
                               info->resource->id,
                               SPA_RESULT_ERROR,
                               error);
    }
    free (error);
    return;
  }
}

static void
on_port_added (struct pw_listener *listener,
               struct pw_node     *node,
               struct pw_port     *port)
{
  struct node_info *info = SPA_CONTAINER_OF (listener, struct node_info, port_added);

  try_link_port (node, port, info);
}

static void
on_port_removed (struct pw_listener *listener,
                 struct pw_node     *node,
                 struct pw_port     *port)
{
}

static void
on_node_created (struct pw_node  *node,
                 struct node_info   *info)
{
  struct pw_port *port;

  spa_list_for_each (port, &node->input_ports, link)
    on_port_added (&info->port_added, node, port);

  spa_list_for_each (port, &node->output_ports, link)
    on_port_added (&info->port_added, node, port);
}

static void
on_state_changed (struct pw_listener  *listener,
                  struct pw_node      *node,
                  enum pw_node_state   old,
                  enum pw_node_state   state)
{
  struct node_info *info = SPA_CONTAINER_OF (listener, struct node_info, state_changed);

  if (old == PW_NODE_STATE_CREATING && state == PW_NODE_STATE_SUSPENDED)
    on_node_created (node, info);
}

static void
on_node_added (struct impl    *impl,
               struct pw_node     *node,
               struct pw_resource *resource,
               struct client_info    *cinfo)
{
  struct node_info *info;

  info = calloc (1, sizeof (struct node_info));
  info->impl = impl;
  info->node = node;
  info->resource = resource;
  info->info = cinfo;
  spa_list_insert (cinfo->node_list.prev, &info->link);

  spa_list_init (&info->port_unlinked.link);
  spa_list_init (&info->link_state_changed.link);
  pw_signal_add (&node->port_added, &info->port_added, on_port_added);
  pw_signal_add (&node->port_removed, &info->port_removed, on_port_removed);
  pw_signal_add (&node->state_changed, &info->state_changed, on_state_changed);

  pw_log_debug ("module %p: node %p added", impl, node);

  if (node->state > PW_NODE_STATE_CREATING)
    on_node_created (node, info);
}

static void
on_resource_added (struct pw_listener *listener,
                   struct pw_client   *client,
                   struct pw_resource *resource)
{
  struct client_info *cinfo = SPA_CONTAINER_OF (listener, struct client_info, resource_added);
  struct impl *impl = cinfo->impl;

  if (resource->type == impl->core->type.client_node) {
    struct pw_client_node *cnode = resource->object;
    on_node_added (impl, cnode->node, resource, cinfo);
  }
}

static void
on_resource_removed (struct pw_listener *listener,
                     struct pw_client   *client,
                     struct pw_resource *resource)
{
  struct client_info *cinfo = SPA_CONTAINER_OF (listener, struct client_info, resource_removed);
  struct impl *impl = cinfo->impl;

  if (resource->type == impl->core->type.client_node) {
    struct pw_client_node *cnode = resource->object;
    struct node_info *ninfo;

    if ((ninfo = find_node_info (cinfo, cnode->node)))
      node_info_free (ninfo);

    pw_log_debug ("module %p: node %p removed", impl, cnode->node);
  }
}

static void
on_global_added (struct pw_listener *listener,
                 struct pw_core     *core,
                 struct pw_global   *global)
{
  struct impl *impl = SPA_CONTAINER_OF (listener, struct impl, global_added);

  if (global->type == impl->core->type.client) {
    struct pw_client *client = global->object;
    struct client_info *cinfo;

    cinfo = calloc (1, sizeof (struct client_info));
    cinfo->impl = impl;
    cinfo->client = global->object;
    spa_list_init (&cinfo->node_list);

    spa_list_insert (impl->client_list.prev, &cinfo->link);

    pw_signal_add (&client->resource_added, &cinfo->resource_added, on_resource_added);
    pw_signal_add (&client->resource_removed, &cinfo->resource_removed, on_resource_removed);

    pw_log_debug ("module %p: client %p added", impl, cinfo->client);
  }
}

static void
on_global_removed (struct pw_listener *listener,
                   struct pw_core     *core,
                   struct pw_global   *global)
{
  struct impl *impl = SPA_CONTAINER_OF (listener, struct impl, global_removed);

  if (global->type == impl->core->type.client) {
    struct pw_client *client = global->object;
    struct client_info *cinfo;

    if ((cinfo = find_client_info (impl, client)))
      client_info_free (cinfo);

    pw_log_debug ("module %p: client %p removed", impl, client);
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

  spa_list_init (&impl->client_list);

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

  pw_signal_remove (&impl->global_added);
  pw_signal_remove (&impl->global_removed);
  pw_signal_remove (&impl->port_added);
  pw_signal_remove (&impl->port_removed);
  pw_signal_remove (&impl->port_unlinked);
  pw_signal_remove (&impl->link_state_changed);
  free (impl);
}
#endif

bool
pipewire__module_init (struct pw_module * module, const char * args)
{
  module->user_data = module_new (module->core, NULL);
  return true;
}
