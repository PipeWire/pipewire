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
#include "modules/v4l2/pv-v4l2-source.h"
#include "dbus/org-pulsevideo.h"

#define PV_DAEMON_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PV_TYPE_DAEMON, PvDaemonPrivate))

struct _PvDaemonPrivate
{
  guint id;
  GDBusConnection *connection;
  GDBusObjectManagerServer *server_manager;

  GHashTable *senders;
  PvSource *source;
};

typedef struct {
  PvDaemon *daemon;
  gchar *sender;
  guint id;

  GHashTable *clients;
} SenderData;

static void
client_name_appeared_handler (GDBusConnection *connection,
                              const gchar *name,
                              const gchar *name_owner,
                              gpointer user_data)
{
}

static void
client_name_vanished_handler (GDBusConnection *connection,
                              const gchar *name,
                              gpointer user_data)
{
  SenderData *data = user_data;
  PvDaemonPrivate *priv = data->daemon->priv;

  g_print ("vanished client %s\n", name);

  g_hash_table_unref (data->clients);
  g_hash_table_remove (priv->senders, data->sender);
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

static gboolean
handle_disconnect_client (PvDaemon1              *interface,
                          GDBusMethodInvocation  *invocation,
                          const gchar            *arg_client,
                          gpointer                user_data)
{
  PvDaemon *daemon = user_data;
  PvDaemonPrivate *priv = daemon->priv;
  const gchar *sender;
  SenderData *data;

  sender = g_dbus_method_invocation_get_sender (invocation);

  g_print ("disconnect client %s\n", sender);
  data = g_hash_table_lookup (priv->senders, sender);
  if (data != NULL) {
    g_hash_table_remove (data->clients, arg_client);
  }

  pv_daemon1_complete_disconnect_client (interface, invocation);

  return TRUE;
}

static gboolean
handle_add_provider (PvManager1             *interface,
                     GDBusMethodInvocation  *invocation,
                     const gchar            *arg_provider,
                     GVariant               *arg_properties,
                     gpointer                user_data)
{
  g_print ("add provider\n");
  g_dbus_method_invocation_return_dbus_error (invocation,
                "org.pulseaudio.Error.NotImplemented",
                "Operation add not yet implemented");
  return TRUE;
}

static gboolean
handle_remove_provider (PvManager1             *interface,
                        GDBusMethodInvocation  *invocation,
                        const gchar            *arg_provider,
                        gpointer                user_data)
{
  g_print ("remove provider\n");
  g_dbus_method_invocation_return_dbus_error (invocation,
                "org.pulseaudio.Error.NotImplemented",
                "Operation remove not yet implemented");
  return TRUE;
}


static void
export_server_object (PvDaemon *daemon, GDBusObjectManagerServer *manager)
{
  GDBusObjectSkeleton *skel;

  skel = g_dbus_object_skeleton_new (PV_DBUS_OBJECT_SERVER);
  {
    PvDaemon1 *iface;

    iface = pv_daemon1_skeleton_new ();
    g_signal_connect (iface, "handle-connect-client", (GCallback) handle_connect_client, daemon);
    g_signal_connect (iface, "handle-disconnect-client", (GCallback) handle_disconnect_client, daemon);
    pv_daemon1_set_user_name (iface, g_get_user_name ());
    pv_daemon1_set_host_name (iface, g_get_host_name ());
    pv_daemon1_set_version (iface, PACKAGE_VERSION);
    pv_daemon1_set_name (iface, PACKAGE_NAME);
    g_dbus_object_skeleton_add_interface (skel, G_DBUS_INTERFACE_SKELETON (iface));
    g_object_unref (iface);
  }
  {
    PvManager1 *iface;

    iface = pv_manager1_skeleton_new ();
    g_signal_connect (iface, "handle-add-provider", (GCallback) handle_add_provider, daemon);
    g_signal_connect (iface, "handle-remove-provider", (GCallback) handle_remove_provider, daemon);

    g_dbus_object_skeleton_add_interface (skel, G_DBUS_INTERFACE_SKELETON (iface));
    g_object_unref (iface);
  }
  g_dbus_object_manager_server_export (manager, skel);
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
  GDBusObjectManagerServer *manager;

  priv->server_manager = manager = g_dbus_object_manager_server_new (PV_DBUS_OBJECT_PREFIX);
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

  g_dbus_object_manager_server_unexport (manager, PV_DBUS_OBJECT_SERVER);
  g_clear_object (&priv->server_manager);
}

PvDaemon *
pv_daemon_new (void)
{
  return g_object_new (PV_TYPE_DAEMON, NULL);
}

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

void
pv_daemon_stop (PvDaemon *daemon)
{
  PvDaemonPrivate *priv = daemon->priv;

  g_return_if_fail (PV_IS_DAEMON (daemon));

  g_bus_unown_name (priv->id);
  priv->id = 0;
}

gchar *
pv_daemon_export_uniquely (PvDaemon *daemon, GDBusObjectSkeleton *skel)
{
  g_return_val_if_fail (PV_IS_DAEMON (daemon), NULL);
  g_return_val_if_fail (G_IS_DBUS_OBJECT_SKELETON (skel), NULL);

  g_dbus_object_manager_server_export_uniquely (daemon->priv->server_manager, skel);

  return g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (skel)));
}

void
pv_daemon_unexport (PvDaemon *daemon, const gchar *object_path)
{
  g_return_if_fail (PV_IS_DAEMON (daemon));
  g_return_if_fail (g_variant_is_object_path (object_path));

  g_dbus_object_manager_server_unexport (daemon->priv->server_manager, object_path);
}

PvSource *
pv_daemon_get_source (PvDaemon *daemon, const gchar *name)
{
  PvDaemonPrivate *priv;

  g_return_val_if_fail (PV_IS_DAEMON (daemon), NULL);
  priv = daemon->priv;

  if (priv->source == NULL) {
    priv->source = pv_v4l2_source_new (daemon);
  }
  return priv->source;
}

G_DEFINE_TYPE (PvDaemon, pv_daemon, G_TYPE_OBJECT);

static void
pv_daemon_finalize (GObject * object)
{
  PvDaemon *daemon = PV_DAEMON_CAST (object);
  PvDaemonPrivate *priv = daemon->priv;

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
}

