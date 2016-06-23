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

#include <gio/gio.h>

#include "config.h"

#include "pinos/client/pinos.h"

#include "pinos/server/daemon.h"
#include "pinos/server/server-node.h"

#include "pinos/dbus/org-pinos.h"

#define PINOS_DAEMON_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_DAEMON, PinosDaemonPrivate))

struct _PinosDaemonPrivate
{
  PinosDaemon1 *iface;
  guint id;
  GDBusConnection *connection;
  GDBusObjectManagerServer *server_manager;

  GList *nodes;

  GHashTable *senders;

  PinosProperties *properties;

  GHashTable *node_factories;
};

enum
{
  PROP_0,
  PROP_PROPERTIES,
};

typedef struct {
  guint id;
  gchar *sender;
  PinosDaemon *daemon;
  GList *objects;
} SenderData;

static void
sender_name_appeared_handler (GDBusConnection *connection,
                              const gchar     *name,
                              const gchar     *name_owner,
                              gpointer         user_data)
{
  SenderData *data = user_data;
  PinosDaemonPrivate *priv = data->daemon->priv;

  g_debug ("daemon %p: appeared %s %s", data->daemon, name, name_owner);

  g_hash_table_insert (priv->senders, data->sender, data);
}

static void
sender_name_vanished_handler (GDBusConnection *connection,
                              const gchar     *name,
                              gpointer         user_data)
{
  SenderData *data = user_data;

  g_debug ("daemon %p: vanished %s", data->daemon, name);

  g_bus_unwatch_name (data->id);
}

static void
data_free (SenderData *data)
{
  g_debug ("daemon %p: free sender data %p for %s", data->daemon, data, data->sender);
  g_list_free_full (data->objects, g_object_unref);
  g_hash_table_remove (data->daemon->priv->senders, data->sender);
  g_free (data->sender);
  g_free (data);
}

static SenderData *
sender_data_new (PinosDaemon *daemon,
                 const gchar *sender)
{
  PinosDaemonPrivate *priv = daemon->priv;
  SenderData *data;

  data = g_new0 (SenderData, 1);
  data->daemon = daemon;
  data->sender = g_strdup (sender);

  g_debug ("daemon %p: new sender data %p for %s", daemon, data, sender);

  data->id = g_bus_watch_name_on_connection (priv->connection,
                                    sender,
                                    G_BUS_NAME_WATCHER_FLAGS_NONE,
                                    sender_name_appeared_handler,
                                    sender_name_vanished_handler,
                                    data,
                                    (GDestroyNotify) data_free);
  return data;
}

static void
handle_remove_node (PinosServerNode *node,
                    gpointer         user_data)
{
  PinosDaemon *daemon = user_data;
  PinosDaemonPrivate *priv = daemon->priv;
  const gchar *sender;
  SenderData *data;

  sender = pinos_server_node_get_sender (node);

  g_debug ("daemon %p: sender %s removed node %p", daemon, sender, node);

  data = g_hash_table_lookup (priv->senders, sender);
  if (data == NULL)
    return;

  g_debug ("daemon %p: node %p unref", daemon, node);
  data->objects = g_list_remove (data->objects, node);
  g_object_unref (node);
}

static gboolean
handle_create_node (PinosDaemon1           *interface,
                    GDBusMethodInvocation  *invocation,
                    const gchar            *arg_factory_name,
                    const gchar            *arg_name,
                    GVariant               *arg_properties,
                    gpointer                user_data)
{
  PinosDaemon *daemon = user_data;
  PinosDaemonPrivate *priv = daemon->priv;
  PinosNodeFactory *factory;
  PinosNode *node;
  SenderData *data;
  const gchar *sender, *object_path;
  PinosProperties *props;

  sender = g_dbus_method_invocation_get_sender (invocation);

  g_debug ("daemon %p: create node: %s", daemon, sender);

  props = pinos_properties_from_variant (arg_properties);

  factory = g_hash_table_lookup (priv->node_factories, arg_factory_name);
  if (factory != NULL) {
    node = pinos_node_factory_create_node (factory,
                                           daemon,
                                           sender,
                                           arg_name,
                                           props);
  } else {
    node = pinos_server_node_new (daemon,
                                  sender,
                                  arg_name,
                                  props);
  }

  pinos_properties_free (props);

  if (node == NULL)
    goto no_node;

  data = g_hash_table_lookup (priv->senders, sender);
  if (data == NULL)
    data = sender_data_new (daemon, sender);

  data->objects = g_list_prepend (data->objects, node);

  g_signal_connect (node,
                    "remove",
                    (GCallback) handle_remove_node,
                    daemon);

  object_path = pinos_server_node_get_object_path (PINOS_SERVER_NODE (node));
  g_debug ("daemon %p: added node %p with path %s", daemon, node, object_path);
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)", object_path));

  return TRUE;

  /* ERRORS */
no_node:
  {
    g_debug ("daemon %p: could create node named %s from factory %s", daemon, arg_name, arg_factory_name);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "can't create node");
    return TRUE;
  }
}

static void
export_server_object (PinosDaemon              *daemon,
                      GDBusObjectManagerServer *manager)
{
  PinosDaemonPrivate *priv = daemon->priv;
  PinosObjectSkeleton *skel;

  skel = pinos_object_skeleton_new (PINOS_DBUS_OBJECT_SERVER);

  pinos_object_skeleton_set_daemon1 (skel, priv->iface);

  g_dbus_object_manager_server_export (manager, G_DBUS_OBJECT_SKELETON (skel));
  g_object_unref (skel);
}

static void
bus_acquired_handler (GDBusConnection *connection,
                      const gchar     *name,
                      gpointer         user_data)
{
  PinosDaemon *daemon = user_data;
  PinosDaemonPrivate *priv = daemon->priv;
  GDBusObjectManagerServer *manager = priv->server_manager;

  priv->connection = connection;

  export_server_object (daemon, manager);

  g_dbus_object_manager_server_set_connection (manager, connection);
}

static void
name_acquired_handler (GDBusConnection *connection,
                       const gchar     *name,
                       gpointer         user_data)
{
}

static void
name_lost_handler (GDBusConnection *connection,
                   const gchar     *name,
                   gpointer         user_data)
{
  PinosDaemon *daemon = user_data;
  PinosDaemonPrivate *priv = daemon->priv;
  GDBusObjectManagerServer *manager = priv->server_manager;

  g_dbus_object_manager_server_unexport (manager, PINOS_DBUS_OBJECT_SERVER);
  g_dbus_object_manager_server_set_connection (manager, connection);
  priv->connection = connection;
}

/**
 * pinos_daemon_new:
 * @properties: #PinosProperties
 *
 * Make a new #PinosDaemon object with given @properties
 *
 * Returns: a new #PinosDaemon
 */
PinosDaemon *
pinos_daemon_new (PinosProperties *properties)
{
  return g_object_new (PINOS_TYPE_DAEMON, "properties", properties, NULL);
}

const gchar *
pinos_daemon_get_sender (PinosDaemon *daemon)
{
  PinosDaemonPrivate *priv;
  const gchar *res;

  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);
  priv = daemon->priv;

  if (priv->connection)
    res = g_dbus_connection_get_unique_name (priv->connection);
  else
    res = NULL;

  return res;
}

/**
 * pinos_daemon_start:
 * @daemon: a #PinosDaemon
 *
 * Start the @daemon.
 */
void
pinos_daemon_start (PinosDaemon *daemon)
{
  PinosDaemonPrivate *priv;

  g_return_if_fail (PINOS_IS_DAEMON (daemon));

  priv = daemon->priv;
  g_return_if_fail (priv->id == 0);

  g_debug ("daemon %p: start", daemon);

  priv->id = g_bus_own_name (G_BUS_TYPE_SESSION,
                             PINOS_DBUS_SERVICE,
                             G_BUS_NAME_OWNER_FLAGS_REPLACE,
                             bus_acquired_handler,
                             name_acquired_handler,
                             name_lost_handler,
                             daemon,
                             NULL);
}

/**
 * pinos_daemon_stop:
 * @daemon: a #PinosDaemon
 *
 * Stop the @daemon.
 */
void
pinos_daemon_stop (PinosDaemon *daemon)
{
  PinosDaemonPrivate *priv = daemon->priv;

  g_return_if_fail (PINOS_IS_DAEMON (daemon));

  g_debug ("daemon %p: stop", daemon);

  if (priv->id != 0) {
    g_bus_unown_name (priv->id);
    priv->id = 0;
  }
}

/**
 * pinos_daemon_export_uniquely:
 * @daemon: a #PinosDaemon
 * @skel: a #GDBusObjectSkeleton
 *
 * Export @skel with @daemon with a unique name
 *
 * Returns: the unique named used to export @skel.
 */
gchar *
pinos_daemon_export_uniquely (PinosDaemon         *daemon,
                              GDBusObjectSkeleton *skel)
{
  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);
  g_return_val_if_fail (G_IS_DBUS_OBJECT_SKELETON (skel), NULL);

  g_dbus_object_manager_server_export_uniquely (daemon->priv->server_manager, skel);

  return g_strdup (g_dbus_object_get_object_path (G_DBUS_OBJECT (skel)));
}

/**
 * pinos_daemon_unexport:
 * @daemon: a #PinosDaemon
 * @object_path: an object path
 *
 * Unexport the object on @object_path
 */
void
pinos_daemon_unexport (PinosDaemon *daemon,
                       const gchar *object_path)
{
  g_return_if_fail (PINOS_IS_DAEMON (daemon));
  g_return_if_fail (g_variant_is_object_path (object_path));

  g_dbus_object_manager_server_unexport (daemon->priv->server_manager, object_path);
}

/**
 * pinos_daemon_add_node:
 * @daemon: a #PinosDaemon
 * @node: a #PinosServerNode
 *
 * Add @node to @daemon.
 */
void
pinos_daemon_add_node (PinosDaemon     *daemon,
                       PinosServerNode *node)
{
  PinosDaemonPrivate *priv;

  g_return_if_fail (PINOS_IS_DAEMON (daemon));
  g_return_if_fail (PINOS_IS_SERVER_NODE (node));
  priv = daemon->priv;

  priv->nodes = g_list_prepend (priv->nodes, node);
}

/**
 * pinos_daemon_remove_node:
 * @daemon: a #PinosDaemon
 * @node: a #PinosServerNode
 *
 * Remove @node from @daemon.
 */
void
pinos_daemon_remove_node (PinosDaemon     *daemon,
                          PinosServerNode *node)
{
  PinosDaemonPrivate *priv;

  g_return_if_fail (PINOS_IS_DAEMON (daemon));
  g_return_if_fail (PINOS_IS_SERVER_NODE (node));
  priv = daemon->priv;

  priv->nodes = g_list_remove (priv->nodes, node);
}

/**
 * pinos_daemon_find_port:
 * @daemon: a #PinosDaemon
 * @name: a port name
 * @props: port properties
 * @format_filter: a format filter
 * @error: location for an error
 *
 * Find the best port in @daemon that matches the given parameters.
 *
 * Returns: a #PinosPort or %NULL when no port could be found.
 */
PinosPort *
pinos_daemon_find_port (PinosDaemon     *daemon,
                        PinosDirection   direction,
                        const gchar     *name,
                        PinosProperties *props,
                        GBytes          *format_filter,
                        GError         **error)
{
  PinosDaemonPrivate *priv;
  PinosServerPort *best = NULL;
  GList *nodes, *ports;
  gboolean have_name;

  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);
  priv = daemon->priv;

  have_name = name ? strlen (name) > 0 : FALSE;

  for (nodes = priv->nodes; nodes; nodes = g_list_next (nodes)) {
    PinosServerNode *n = nodes->data;
    gboolean node_found = FALSE;

    /* we found the node */
    if (have_name && g_str_has_suffix (pinos_server_node_get_object_path (n), name)) {
      g_debug ("name \"%s\" matches node %p", name, n);
      node_found = TRUE;
    }

    for (ports = pinos_node_get_ports (PINOS_NODE (n)); ports; ports = g_list_next (ports)) {
      PinosServerPort *p = ports->data;
      PinosDirection dir;
      GBytes *format;

      g_object_get (p, "direction", &dir, NULL);
      if (dir != direction)
        continue;

      if (have_name && !node_found) {
        if (!g_str_has_suffix (pinos_server_port_get_object_path (p), name))
          continue;
        g_debug ("name \"%s\" matches port %p", name, p);
        best = p;
        node_found = TRUE;
        break;
      }

      format = pinos_port_filter_formats (PINOS_PORT (p), format_filter, NULL);
      if (format != NULL) {
        g_debug ("port %p matches filter", p);
        g_bytes_unref (format);
        best = p;
        node_found = TRUE;
        break;
      }
    }
    if (node_found)
      break;
  }
  if (best == NULL) {
    if (error)
      *error = g_error_new (G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "No matching Port found");
  }
  return PINOS_PORT (best);
}


G_DEFINE_TYPE (PinosDaemon, pinos_daemon, G_TYPE_OBJECT);

static void
pinos_daemon_get_property (GObject    *_object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PinosDaemon *daemon = PINOS_DAEMON (_object);
  PinosDaemonPrivate *priv = daemon->priv;

  switch (prop_id) {
    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (daemon, prop_id, pspec);
      break;
  }
}

static void
pinos_daemon_set_property (GObject      *_object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosDaemon *daemon = PINOS_DAEMON (_object);
  PinosDaemonPrivate *priv = daemon->priv;

  switch (prop_id) {
    case PROP_PROPERTIES:
      if (priv->properties)
        pinos_properties_free (priv->properties);
      priv->properties = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (daemon, prop_id, pspec);
      break;
  }
}

static void
pinos_daemon_constructed (GObject * object)
{
  PinosDaemon *daemon = PINOS_DAEMON_CAST (object);
  PinosDaemonPrivate *priv = daemon->priv;

  g_debug ("daemon %p: constructed", object);
  pinos_daemon1_set_user_name (priv->iface, g_get_user_name ());
  pinos_daemon1_set_host_name (priv->iface, g_get_host_name ());
  pinos_daemon1_set_version (priv->iface, PACKAGE_VERSION);
  pinos_daemon1_set_name (priv->iface, PACKAGE_NAME);
  pinos_daemon1_set_cookie (priv->iface, g_random_int());
  pinos_daemon1_set_properties (priv->iface, pinos_properties_to_variant (priv->properties));

  G_OBJECT_CLASS (pinos_daemon_parent_class)->constructed (object);
}

static void
pinos_daemon_dispose (GObject * object)
{
  PinosDaemon *daemon = PINOS_DAEMON_CAST (object);

  g_debug ("daemon %p: dispose", object);

  pinos_daemon_stop (daemon);

  G_OBJECT_CLASS (pinos_daemon_parent_class)->dispose (object);
}

static void
pinos_daemon_finalize (GObject * object)
{
  PinosDaemon *daemon = PINOS_DAEMON_CAST (object);
  PinosDaemonPrivate *priv = daemon->priv;

  g_debug ("daemon %p: finalize", object);
  g_clear_object (&priv->server_manager);
  g_clear_object (&priv->iface);
  g_hash_table_unref (priv->senders);
  g_hash_table_unref (priv->node_factories);

  G_OBJECT_CLASS (pinos_daemon_parent_class)->finalize (object);
}

static void
pinos_daemon_class_init (PinosDaemonClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosDaemonPrivate));

  gobject_class->constructed = pinos_daemon_constructed;
  gobject_class->dispose = pinos_daemon_dispose;
  gobject_class->finalize = pinos_daemon_finalize;

  gobject_class->set_property = pinos_daemon_set_property;
  gobject_class->get_property = pinos_daemon_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_PROPERTIES,
                                   g_param_spec_boxed ("properties",
                                                       "Properties",
                                                       "Client properties",
                                                       PINOS_TYPE_PROPERTIES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));

}

static void
pinos_daemon_init (PinosDaemon * daemon)
{
  PinosDaemonPrivate *priv = daemon->priv = PINOS_DAEMON_GET_PRIVATE (daemon);

  g_debug ("daemon %p: new", daemon);
  priv->iface = pinos_daemon1_skeleton_new ();
  g_signal_connect (priv->iface, "handle-create-node", (GCallback) handle_create_node, daemon);

  priv->server_manager = g_dbus_object_manager_server_new (PINOS_DBUS_OBJECT_PREFIX);
  priv->senders = g_hash_table_new (g_str_hash, g_str_equal);
  priv->node_factories = g_hash_table_new_full (g_str_hash,
                                                g_str_equal,
                                                g_free,
                                                g_object_unref);
}

/**
 * pinos_daemon_add_node_factory:
 * @daemon: a #PinosDaemon
 * @factory: a #PinosNodeFactory
 *
 * Add a #PinosNodeFactory in the daemon that will be used for creating nodes.
 */
void
pinos_daemon_add_node_factory  (PinosDaemon *daemon,
                                PinosNodeFactory *factory)
{
  PinosDaemonPrivate *priv = daemon->priv;
  gchar *name;

  g_return_if_fail (PINOS_IS_DAEMON (daemon));
  g_return_if_fail (PINOS_IS_NODE_FACTORY (factory));

  g_object_get (factory, "name", &name, NULL);
  g_hash_table_insert (priv->node_factories, name, g_object_ref (factory));
}

/**
 * pinos_daemon_add_node_factory:
 * @daemon: a #PinosDaemon
 * @factory: a #PinosNodeFactory
 *
 * Remove a #PinosNodeFactory from the daemon.
 */
void
pinos_daemon_remove_node_factory (PinosDaemon *daemon,
                                  PinosNodeFactory *factory)
{
  PinosDaemonPrivate *priv = daemon->priv;
  gchar *name;

  g_return_if_fail (PINOS_IS_DAEMON (daemon));
  g_return_if_fail (PINOS_IS_NODE_FACTORY (factory));

  g_object_get (factory, "name", &name, NULL);
  g_hash_table_remove (priv->node_factories, name);
  g_free (name);
}
