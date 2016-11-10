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
  PinosClient client;
  PinosObject object;
  PinosInterface ifaces[1];
  PinosDaemon *daemon;

  guint id;
  PinosClient1 *iface;
  gchar *object_path;

  GList *objects;
} PinosClientImpl;

static void
client_name_appeared_handler (GDBusConnection *connection,
                              const gchar     *name,
                              const gchar     *name_owner,
                              gpointer         user_data)
{
  PinosClientImpl *impl = user_data;
  PinosClient *client = &impl->client;

  pinos_log_debug ("client %p: appeared %s %s", client, name, name_owner);
  pinos_signal_emit (&client->appeared, client, NULL);
}

static void
client_name_vanished_handler (GDBusConnection *connection,
                              const gchar     *name,
                              gpointer         user_data)
{
  PinosClientImpl *impl = user_data;
  PinosClient *client = &impl->client;

  pinos_log_debug ("client %p: vanished %s", client, name);

  pinos_signal_emit (&client->vanished, client, NULL);
  g_bus_unwatch_name (impl->id);
}

static void
client_watch_name (PinosClient *client)
{
  PinosClientImpl *impl = SPA_CONTAINER_OF (client, PinosClientImpl, client);
  GDBusConnection *connection = NULL;

//  g_object_get (impl->daemon, "connection", &connection, NULL);

  impl->id = g_bus_watch_name_on_connection (connection,
                                             client->sender,
                                             G_BUS_NAME_WATCHER_FLAGS_NONE,
                                             client_name_appeared_handler,
                                             client_name_vanished_handler,
                                             impl,
                                             (GDestroyNotify) pinos_client_destroy);
}

static void
client_register_object (PinosClient *client)
{
  PinosClientImpl *impl = SPA_CONTAINER_OF (client, PinosClientImpl, client);
  PinosDaemon *daemon = impl->daemon;
  PinosObjectSkeleton *skel;

  skel = pinos_object_skeleton_new (PINOS_DBUS_OBJECT_CLIENT);

  pinos_object_skeleton_set_client1 (skel, impl->iface);

  g_free (impl->object_path);
  impl->object_path = pinos_daemon_export_uniquely (daemon, G_DBUS_OBJECT_SKELETON (skel));
  g_object_unref (skel);

  pinos_log_debug ("client %p: register %s", client, impl->object_path);
}

static void
client_unregister_object (PinosClient *client)
{
  PinosClientImpl *impl = SPA_CONTAINER_OF (client, PinosClientImpl, client);
  PinosDaemon *daemon = impl->daemon;

  pinos_log_debug ("client %p: unregister", client);
  pinos_daemon_unexport (daemon, impl->object_path);
}

static void
client_destroy (PinosObject * object)
{
  PinosClientImpl *impl = SPA_CONTAINER_OF (object, PinosClientImpl, object);
  PinosClient *client = &impl->client;
  GList *copy;

  pinos_log_debug ("client %p: destroy", client);
  pinos_registry_remove_object (&client->core->registry, &impl->object);

  copy = g_list_copy (impl->objects);
  g_list_free_full (copy, NULL);
  g_list_free (impl->objects);

  client_unregister_object (client);

  free (client->sender);
  if (client->properties)
    pinos_properties_free (client->properties);

  g_clear_object (&impl->iface);
  free (impl->object_path);
  free (object);
}


static void
client_add_object (PinosClient *client,
                   PinosObject *object)
{
  PinosClientImpl *impl = SPA_CONTAINER_OF (client, PinosClientImpl, client);

  g_return_if_fail (client);
  g_return_if_fail (object);

  impl->objects = g_list_prepend (impl->objects, object);
}

static void
client_remove_object (PinosClient *client,
                      PinosObject *object)
{
  PinosClientImpl *impl = SPA_CONTAINER_OF (client, PinosClientImpl, client);

  g_return_if_fail (client);
  g_return_if_fail (object);

  impl->objects = g_list_remove (impl->objects, object);
  pinos_object_destroy (object);
}

static bool
client_has_object (PinosClient *client,
                   PinosObject *object)
{
  PinosClientImpl *impl = SPA_CONTAINER_OF (client, PinosClientImpl, client);
  GList *found;

  g_return_val_if_fail (client, false);
  g_return_val_if_fail (object, false);

  found = g_list_find (impl->objects, object);

  return found != NULL;
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
  PinosClient *client;
  PinosClientImpl *impl;

  impl = calloc (1, sizeof (PinosClientImpl *));
  client = &impl->client;
  client->core = core;
  client->sender = strdup (sender);
  client->properties = properties;
  client->add_object = client_add_object;
  client->remove_object = client_remove_object;
  client->has_object = client_has_object;

  impl->ifaces[0].type = client->core->registry.uri.client;
  impl->ifaces[0].iface = client;

  pinos_object_init (&impl->object,
                     client_destroy,
                     1,
                     impl->ifaces);

  pinos_log_debug ("client %p: new", impl);

  impl->iface = pinos_client1_skeleton_new ();

  client_watch_name (client);
  client_register_object (client);

  pinos_registry_add_object (&client->core->registry, &impl->object);

  return client;
}

/**
 * pinos_client_destroy:
 * @client: a #PinosClient
 *
 * Trigger removal of @client
 */
void
pinos_client_destroy (PinosClient *client)
{
  PinosClientImpl *impl = SPA_CONTAINER_OF (client, PinosClientImpl, client);

  g_return_if_fail (client);

  pinos_log_debug ("client %p: destroy", client);
  pinos_object_destroy (&impl->object);
}

/**
 * pinos_client_get_object_path:
 * @client: a #PinosClient
 *
 * Get the object path of @client.
 *
 * Returns: the object path of @client
 */
const gchar *
pinos_client_get_object_path (PinosClient *client)
{
  PinosClientImpl *impl = SPA_CONTAINER_OF (client, PinosClientImpl, client);

  g_return_val_if_fail (client, NULL);

  return impl->object_path;
}
