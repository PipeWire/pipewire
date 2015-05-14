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

#include "dbus/org-pulsevideo.h"

#include "modules/v4l2/pv-v4l2-source.h"

#define PV_DAEMON_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_DAEMON, PvDaemonPrivate))

struct _PvDaemonPrivate
{
  guint id;
  GDBusConnection *connection;
  GDBusObjectManagerServer *server_manager;
  PvSubscribe *subscribe;

  GList *sources;

  GHashTable *senders;
};

typedef struct {
  guint id;
  gchar *sender;
  PvDaemon *daemon;
  GList *objects;
} SenderData;

static void
on_server_subscription_event (PvSubscribe         *subscribe,
                              PvSubscriptionEvent  event,
                              PvSubscriptionFlags  flags,
                              GDBusProxy          *object,
                              gpointer             user_data)
{
  const gchar *name, *object_path;

  name = g_dbus_proxy_get_name (object);
  object_path = g_dbus_proxy_get_object_path (object);

  g_print ("got event %d %d %s:%s\n", event, flags, name, object_path);

  switch (flags) {
    default:
      break;
  }
}
static void
client_name_appeared_handler (GDBusConnection *connection,
                              const gchar *name,
                              const gchar *name_owner,
                              gpointer user_data)
{
  SenderData *data = user_data;
  PvDaemonPrivate *priv = data->daemon->priv;

  g_print ("client name appeared def: %p\n", g_main_context_get_thread_default ());

  g_hash_table_insert (priv->senders, data->sender, data);

  if (!g_strcmp0 (name, g_dbus_connection_get_unique_name (connection)))
    return;

  g_print ("appeared client %s %p\n", name, data);
}

static void
client_name_vanished_handler (GDBusConnection *connection,
                              const gchar *name,
                              gpointer user_data)
{
  SenderData *data = user_data;

  g_print ("vanished client %s %p\n", name, data);

  g_bus_unwatch_name (data->id);
}

static void
data_free (SenderData *data)
{
  g_print ("free client %s %p\n", data->sender, data);

  g_list_free_full (data->objects, g_object_unref);
  g_hash_table_remove (data->daemon->priv->senders, data->sender);
  g_free (data->sender);
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

  g_print ("watch name def: %p\n", g_main_context_get_thread_default ());
  g_print ("watch name %s %p\n", sender, data);

  data->id = g_bus_watch_name_on_connection (priv->connection,
                                    sender,
                                    G_BUS_NAME_WATCHER_FLAGS_NONE,
                                    client_name_appeared_handler,
                                    client_name_vanished_handler,
                                    data,
                                    (GDestroyNotify) data_free);


  return data;
}


static gboolean
handle_connect_client (PvDaemon1              *interface,
                       GDBusMethodInvocation  *invocation,
                       GVariant               *arg_properties,
                       gpointer                user_data)
{
  PvDaemon *daemon = user_data;
  PvClient *client;
  const gchar *sender, *object_path;

  sender = g_dbus_method_invocation_get_sender (invocation);

  g_print ("connect client %s\n", sender);
  client = pv_client_new (daemon, sender, PV_DBUS_OBJECT_PREFIX, arg_properties);

  pv_daemon_track_object (daemon, sender, G_OBJECT (client));

  object_path = pv_client_get_object_path (client);
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)", object_path));

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

  export_server_object (daemon, manager);

  g_object_set (priv->subscribe, "service", PV_DBUS_SERVICE,
                                 "subscription-mask", PV_SUBSCRIPTION_FLAGS_ALL,
                                 "connection", connection,
                                 NULL);

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

void
pv_daemon_track_object (PvDaemon    *daemon,
                        const gchar *sender,
                        GObject     *object)
{
  PvDaemonPrivate *priv;
  SenderData *data;

  g_return_if_fail (PV_IS_DAEMON (daemon));
  g_return_if_fail (sender != NULL);
  g_return_if_fail (G_IS_OBJECT (object));

  priv = daemon->priv;

  data = g_hash_table_lookup (priv->senders, sender);
  if (data == NULL)
    data = sender_data_new (daemon, sender);

  data->objects = g_list_prepend (data->objects, object);
}

void
pv_daemon_add_source (PvDaemon *daemon, PvSource *source)
{
  PvDaemonPrivate *priv;

  g_return_if_fail (PV_IS_DAEMON (daemon));
  g_return_if_fail (PV_IS_SOURCE (source));
  priv = daemon->priv;

  priv->sources = g_list_prepend (priv->sources, source);
}

void
pv_daemon_remove_source (PvDaemon *daemon, PvSource *source)
{
  PvDaemonPrivate *priv;

  g_return_if_fail (PV_IS_DAEMON (daemon));
  g_return_if_fail (PV_IS_SOURCE (source));
  priv = daemon->priv;

  priv->sources = g_list_remove (priv->sources, source);
}

PvSource *
pv_daemon_find_source (PvDaemon    *daemon,
                       const gchar *name,
                       GVariant    *props,
                       GBytes      *format_filter)
{
  PvDaemonPrivate *priv;

  g_return_val_if_fail (PV_IS_DAEMON (daemon), NULL);
  priv = daemon->priv;

  if (priv->sources == NULL)
    return NULL;

  return priv->sources->data;
}

G_DEFINE_TYPE (PvDaemon, pv_daemon, G_TYPE_OBJECT);

static void
pv_daemon_finalize (GObject * object)
{
  PvDaemon *daemon = PV_DAEMON_CAST (object);
  PvDaemonPrivate *priv = daemon->priv;

  g_clear_object (&priv->server_manager);
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

  priv->server_manager = g_dbus_object_manager_server_new (PV_DBUS_OBJECT_PREFIX);
  priv->senders = g_hash_table_new (g_str_hash, g_str_equal);

  priv->subscribe = pv_subscribe_new ();
  g_signal_connect (priv->subscribe,
                    "subscription-event",
                    (GCallback) on_server_subscription_event,
                    daemon);
}

