/* Pulsevideo
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

#include <gio/gio.h>

#include "config.h"

#include "server/pv-daemon.h"
#include "server/pv-client.h"
#include "server/pv-source-provider.h"
#include "modules/v4l2/pv-v4l2-source.h"
#include "dbus/org-pulsevideo.h"

#define PV_DAEMON_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_DAEMON, PvDaemonPrivate))

struct _PvDaemonPrivate
{
  guint id;
  GDBusConnection *connection;
  GDBusObjectManagerServer *server_manager;
  PvSubscribe *subscribe;

  GHashTable *senders;
  GList *sources;
};

typedef struct {
  PvDaemon *daemon;
  gchar *sender;
  guint id;

  GHashTable *clients;
  PvSubscribe *subscribe;

  GHashTable *sources;
} SenderData;

static void
on_server_subscription_event (PvSubscribe         *subscribe,
                              PvSubscriptionEvent  event,
                              PvSubscriptionFlags  flags,
                              GDBusObjectProxy    *object,
                              gpointer             user_data)
{
  PvDaemon *daemon = user_data;
  PvDaemonPrivate *priv = daemon->priv;
  const gchar *object_path;
  PvSource1 *source1;
  gchar *service;

  if (flags != PV_SUBSCRIPTION_FLAGS_SOURCE)
    return;

  object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));

  g_object_get (subscribe, "service", &service, NULL);
  g_print ("got event %d %d %s.%s\n", event, flags, service, object_path);

  source1 = pv_object_peek_source1 (PV_OBJECT (object));

  switch (event) {
    case PV_SUBSCRIPTION_EVENT_NEW:
    {
      g_object_set_data (G_OBJECT (source1), "org.pulsevideo.name", service);
      priv->sources = g_list_prepend (priv->sources, source1);
      break;
    }

    case PV_SUBSCRIPTION_EVENT_CHANGE:
      break;

    case PV_SUBSCRIPTION_EVENT_REMOVE:
    {
      priv->sources = g_list_remove (priv->sources, source1);
      break;
    }
  }
}

static void
on_sender_subscription_event (PvSubscribe         *subscribe,
                              PvSubscriptionEvent  event,
                              PvSubscriptionFlags  flags,
                              GDBusObjectProxy    *object,
                              gpointer             user_data)
{
  SenderData *data = user_data;
  PvDaemon *daemon = data->daemon;
  const gchar *object_path;

  on_server_subscription_event (subscribe, event, flags, object, daemon);

  object_path = g_dbus_object_get_object_path (G_DBUS_OBJECT (object));

  switch (event) {
    case PV_SUBSCRIPTION_EVENT_NEW:
    {
      PvSourceProvider *provider;

      provider = pv_source_provider_new (daemon, PV_DBUS_OBJECT_PREFIX, data->sender,
          object_path);
      g_hash_table_insert (data->sources, g_strdup (object_path), provider);
      break;
    }

    case PV_SUBSCRIPTION_EVENT_CHANGE:
      break;

    case PV_SUBSCRIPTION_EVENT_REMOVE:
    {
      g_hash_table_remove (data->sources, object_path);
      break;
    }
  }
}


static void
client_name_appeared_handler (GDBusConnection *connection,
                              const gchar *name,
                              const gchar *name_owner,
                              gpointer user_data)
{
  SenderData *data = user_data;

  /* subscribe to Source events. We want to be notified when this new
   * sender add/change/remove sources */
  data->subscribe = pv_subscribe_new ();
  g_object_set (data->subscribe, "service", data->sender,
                                 "subscription-mask", PV_SUBSCRIPTION_FLAGS_SOURCE,
                                 "connection", connection,
                                 NULL);

  g_signal_connect (data->subscribe,
                    "subscription-event",
                    (GCallback) on_sender_subscription_event,
                    data);
}

static void
client_name_vanished_handler (GDBusConnection *connection,
                              const gchar *name,
                              gpointer user_data)
{
  SenderData *data = user_data;
  PvDaemonPrivate *priv = data->daemon->priv;

  g_print ("vanished client %s\n", name);

  g_hash_table_remove (priv->senders, data->sender);

  g_hash_table_unref (data->clients);
  g_hash_table_unref (data->sources);
  g_object_unref (data->subscribe);
  g_free (data->sender);
  g_bus_unwatch_name (data->id);
  g_free (data);
}

static SenderData *
sender_data_new (PvDaemon *daemon, const gchar *sender)
{
  PvDaemonPrivate *priv = daemon->priv;
  SenderData *data;

  data = g_new0 (SenderData, 1);
  data->daemon = daemon;
  data->sender = g_strdup (sender);
  data->clients = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);
  data->sources = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_object_unref);

  data->id = g_bus_watch_name_on_connection (priv->connection,
                                    sender,
                                    G_BUS_NAME_WATCHER_FLAGS_NONE,
                                    client_name_appeared_handler,
                                    client_name_vanished_handler,
                                    data,
                                    NULL);

  g_hash_table_insert (priv->senders, data->sender, data);

  return data;
}

static gboolean
handle_connect_client (PvDaemon1              *interface,
                       GDBusMethodInvocation  *invocation,
                       GVariant               *arg_properties,
                       gpointer                user_data)
{
  PvDaemon *daemon = user_data;
  PvDaemonPrivate *priv = daemon->priv;
  PvClient *client;
  const gchar *sender, *object_path;
  SenderData *data;

  sender = g_dbus_method_invocation_get_sender (invocation);

  g_print ("connect client %s\n", sender);
  data = g_hash_table_lookup (priv->senders, sender);
  if (data == NULL) {
    data = sender_data_new (daemon, sender);
  }

  client = pv_client_new (daemon, PV_DBUS_OBJECT_PREFIX);
  object_path = pv_client_get_object_path (client);

  g_hash_table_insert (data->clients, g_strdup (object_path), client);

  pv_daemon1_complete_connect_client (interface, invocation, object_path);

  return TRUE;
}

static void
export_server_object (PvDaemon *daemon, GDBusObjectManagerServer *manager)
{
  PvObjectSkeleton *skel;

  skel = pv_object_skeleton_new (PV_DBUS_OBJECT_SERVER);
  {
    PvDaemon1 *iface;

    iface = pv_daemon1_skeleton_new ();
    g_signal_connect (iface, "handle-connect-client", (GCallback) handle_connect_client, daemon);
    pv_daemon1_set_user_name (iface, g_get_user_name ());
    pv_daemon1_set_host_name (iface, g_get_host_name ());
    pv_daemon1_set_version (iface, PACKAGE_VERSION);
    pv_daemon1_set_name (iface, PACKAGE_NAME);
    pv_object_skeleton_set_daemon1 (skel, iface);
    g_object_unref (iface);
  }
  g_dbus_object_manager_server_export (manager, G_DBUS_OBJECT_SKELETON (skel));
  g_object_unref (skel);
}

static void
bus_acquired_handler (GDBusConnection *connection,
                      const gchar *name,
                      gpointer user_data)
{
  PvDaemon *daemon = user_data;
  PvDaemonPrivate *priv = daemon->priv;

  priv->connection = connection;
}

static void
name_acquired_handler (GDBusConnection *connection,
                       const gchar *name,
                       gpointer user_data)
{
  PvDaemon *daemon = user_data;
  PvDaemonPrivate *priv = daemon->priv;
  GDBusObjectManagerServer *manager = priv->server_manager;

  g_object_set (priv->subscribe, "service", PV_DBUS_SERVICE,
                                 "subscription-mask", PV_SUBSCRIPTION_FLAGS_SOURCE,
                                 "connection", connection,
                                 NULL);


  export_server_object (daemon, manager);

  g_dbus_object_manager_server_set_connection (manager, connection);
}

static void
name_lost_handler (GDBusConnection *connection,
                   const gchar *name,
                   gpointer user_data)
{
  PvDaemon *daemon = user_data;
  PvDaemonPrivate *priv = daemon->priv;
  GDBusObjectManagerServer *manager = priv->server_manager;

  g_object_set (priv->subscribe, "connection", connection, NULL);

  g_dbus_object_manager_server_unexport (manager, PV_DBUS_OBJECT_SERVER);
  g_dbus_object_manager_server_set_connection (manager, connection);
}

/**
 * pv_daemon_new:
 *
 * Make a new #PvDaemon object
 *
 * Returns: a new #PvDaemon
 */
PvDaemon *
pv_daemon_new (void)
{
  return g_object_new (PV_TYPE_DAEMON, NULL);
}

/**
 * pv_daemon_start:
 * @daemon: a #PvDaemon
 *
 * Start the @daemon.
 */
void
pv_daemon_start (PvDaemon *daemon)
{
  PvDaemonPrivate *priv;

  g_return_if_fail (PV_IS_DAEMON (daemon));

  priv = daemon->priv;
  g_return_if_fail (priv->id == 0);

  priv->id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             PV_DBUS_SERVICE,
                             G_BUS_NAME_OWNER_FLAGS_REPLACE,
                             bus_acquired_handler,
                             name_acquired_handler,
                             name_lost_handler,
                             daemon,
                             NULL);
}

/**
 * pv_daemon_stop:
 * @daemon: a #PvDaemon
 *
 * Stop the @daemon.
 */
void
pv_daemon_stop (PvDaemon *daemon)
{
  PvDaemonPrivate *priv = daemon->priv;

  g_return_if_fail (PV_IS_DAEMON (daemon));

  if (priv->id != 0) {
    g_bus_unown_name (priv->id);
    priv->id = 0;
  }
}

/**
 * pv_daemon_export_uniquely:
 * @daemon: a #PvDaemon
 * @skel: a #GDBusObjectSkeleton
 *
 * Export @skel with @daemon with a unique name
 *
 * Returns: the unique named used to export @skel.
 */
gchar *
pv_daemon_export_uniquely (PvDaemon *daemon, GDBusObjectSkeleton *skel)
{
  g_return_val_if_fail (PV_IS_DAEMON (daemon), NULL);
  g_return_val_if_fail (G_IS_DBUS_OBJECT_SKELETON (skel), NULL);

  g_dbus_object_manager_server_export_uniquely (daemon->priv->server_manager, skel);

  return g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (skel)));
}

/**
 * pv_daemon_unexport:
 * @daemon: a #PvDaemon
 * @object_path: an object path
 *
 * Unexport the object on @object_path
 */
void
pv_daemon_unexport (PvDaemon *daemon, const gchar *object_path)
{
  g_return_if_fail (PV_IS_DAEMON (daemon));
  g_return_if_fail (g_variant_is_object_path (object_path));

  g_dbus_object_manager_server_unexport (daemon->priv->server_manager, object_path);
}

/**
 * pv_daemon_add_source:
 * @daemon: a #PvDaemon
 * @source: a #PvSource
 *
 * Register @source with @daemon so that it becomes available to clients.
 */
void
pv_daemon_add_source (PvDaemon *daemon, PvSource *source)
{
  PvDaemonPrivate *priv;

  g_return_if_fail (PV_IS_DAEMON (daemon));
  g_return_if_fail (PV_IS_SOURCE (source));
  priv = daemon->priv;

  pv_source_set_manager (source, priv->server_manager);
}

/**
 * pv_daemon_remove_source:
 * @daemon: a #PvDaemon
 * @source: a #PvSource
 *
 * Unregister @source from @daemon so that it becomes unavailable to clients.
 */
void
pv_daemon_remove_source (PvDaemon *daemon, PvSource *source)
{
  g_return_if_fail (PV_IS_DAEMON (daemon));
  g_return_if_fail (PV_IS_SOURCE (source));

  pv_source_set_manager (source, NULL);
}

/**
 * pv_daemon_get_source:
 * @daemon: a #PvDaemon
 * @name: a name
 *
 * Find a #PvSource1 for @name in @daemon
 *
 * Returns: a #PvSource1
 */
PvSource1 *
pv_daemon_get_source (PvDaemon *daemon, const gchar *name)
{
  PvDaemonPrivate *priv;
  PvSource1 *source;

  g_return_val_if_fail (PV_IS_DAEMON (daemon), NULL);
  priv = daemon->priv;

  if (priv->sources == NULL)
    return NULL;

  source = priv->sources->data;

  return source;
}

G_DEFINE_TYPE (PvDaemon, pv_daemon, G_TYPE_OBJECT);

static void
pv_daemon_finalize (GObject * object)
{
  PvDaemon *daemon = PV_DAEMON_CAST (object);
  PvDaemonPrivate *priv = daemon->priv;

  g_clear_object (&priv->server_manager);
  g_hash_table_unref (priv->senders);
  pv_daemon_stop (daemon);

  G_OBJECT_CLASS (pv_daemon_parent_class)->finalize (object);
}

static void
pv_daemon_class_init (PvDaemonClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PvDaemonPrivate));

  gobject_class->finalize = pv_daemon_finalize;
}

static void
pv_daemon_init (PvDaemon * daemon)
{
  PvDaemonPrivate *priv = daemon->priv = PV_DAEMON_GET_PRIVATE (daemon);

  priv->senders = g_hash_table_new (g_str_hash, g_str_equal);
  priv->server_manager = g_dbus_object_manager_server_new (PV_DBUS_OBJECT_PREFIX);

  priv->subscribe = pv_subscribe_new ();
  g_signal_connect (priv->subscribe,
                    "subscription-event",
                    (GCallback) on_server_subscription_event,
                    daemon);
}

