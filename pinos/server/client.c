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
#include "pinos/client/pinos.h"

#include "pinos/server/client.h"

#include "pinos/dbus/org-pinos.h"

typedef struct
{
  PinosClient this;

  guint id;
  PinosClient1 *iface;

} PinosClientImpl;

static void
client_name_appeared_handler (GDBusConnection *connection,
                              const gchar     *name,
                              const gchar     *name_owner,
                              gpointer         user_data)
{
  PinosClientImpl *impl = user_data;
  PinosClient *this = &impl->this;
  PinosObjectSkeleton *skel;

  pinos_log_debug ("client %p: appeared %s %s", this, name, name_owner);

  skel = pinos_object_skeleton_new (PINOS_DBUS_OBJECT_CLIENT);
  pinos_object_skeleton_set_client1 (skel, impl->iface);

  this->global = pinos_core_add_global (this->core,
                                        this->core->registry.uri.client,
                                        this,
                                        skel);
}

static void
client_name_vanished_handler (GDBusConnection *connection,
                              const gchar     *name,
                              gpointer         user_data)
{
  PinosClientImpl *impl = user_data;
  PinosClient *this = &impl->this;

  pinos_log_debug ("client %p: vanished %s", this, name);

  pinos_core_remove_global (this->core,
                            this->global);
  this->global = NULL;

  g_bus_unwatch_name (impl->id);
}

static void
client_watch_name (PinosClient *this)
{
  PinosClientImpl *impl = SPA_CONTAINER_OF (this, PinosClientImpl, this);

  impl->id = g_bus_watch_name_on_connection (this->core->connection,
                                             this->sender,
                                             G_BUS_NAME_WATCHER_FLAGS_NONE,
                                             client_name_appeared_handler,
                                             client_name_vanished_handler,
                                             impl,
                                             (GDestroyNotify) pinos_client_destroy);
}

PinosResource *
pinos_client_add_resource (PinosClient *client,
                           uint32_t     type,
                           void        *object,
                           PinosDestroy destroy)
{
  PinosResource *resource;

  resource = calloc (1, sizeof (PinosResource));
  resource->core = client->core;
  resource->id = 0;
  resource->type = type;
  resource->object = object;
  resource->destroy = destroy;

  pinos_signal_init (&resource->destroy_signal);

  pinos_log_debug ("client %p: add resource %p", client, resource);

  spa_list_insert (client->resource_list.prev, &resource->link);

  return resource;
}

void
pinos_client_remove_resource (PinosClient   *client,
                              PinosResource *resource)
{
  pinos_log_debug ("client %p: resource %p destroy", client, resource);
  pinos_signal_emit (&resource->destroy_signal, resource);

  spa_list_remove (&resource->link);

  if (resource->destroy)
    resource->destroy (resource->object);

  free (resource);
}

bool
pinos_client_has_resource (PinosClient   *client,
                           PinosResource *resource)
{
  return false;
}

/**
 * pinos_client_new:
 * @daemon: a #PinosDaemon
 * @sender: the sender id
 * @prefix: a prefix
 * @properties: extra client properties
 *
 * Make a new #PinosClient object and register it to @daemon under the @prefix.
 *
 * Returns: a new #PinosClient
 */
PinosClient *
pinos_client_new (PinosCore       *core,
                  const gchar     *sender,
                  PinosProperties *properties)
{
  PinosClient *this;
  PinosClientImpl *impl;

  impl = calloc (1, sizeof (PinosClientImpl));
  pinos_log_debug ("client %p: new", impl);

  this = &impl->this;
  this->core = core;
  this->sender = strdup (sender);
  this->properties = properties;

  spa_list_init (&this->resource_list);
  pinos_signal_init (&this->destroy_signal);

  impl->iface = pinos_client1_skeleton_new ();

  client_watch_name (this);

  spa_list_insert (core->client_list.prev, &this->list);

  return this;
}

/**
 * pinos_client_destroy:
 * @client: a #PinosClient
 *
 * Trigger removal of @client
 */
SpaResult
pinos_client_destroy (PinosClient * client)
{
  PinosClientImpl *impl = SPA_CONTAINER_OF (client, PinosClientImpl, this);
  PinosResource *resource, *tmp;

  pinos_log_debug ("client %p: destroy", client);
  pinos_signal_emit (&client->destroy_signal, client);

  spa_list_for_each_safe (resource, tmp, &client->resource_list, link)
    pinos_client_remove_resource (client, resource);

  spa_list_remove (&client->list);

  free (client->sender);
  if (client->properties)
    pinos_properties_free (client->properties);

  g_clear_object (&impl->iface);
  free (impl);

  return SPA_RESULT_OK;
}
