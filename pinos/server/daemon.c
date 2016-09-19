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
#include <gio/gunixfdlist.h>

#include "config.h"

#include "pinos/client/pinos.h"

#include "pinos/server/daemon.h"
#include "pinos/server/node.h"
#include "pinos/server/client-node.h"
#include "pinos/server/client.h"
#include "pinos/server/link.h"

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

  GHashTable *clients;

  PinosProperties *properties;

  GHashTable *node_factories;
};

enum
{
  PROP_0,
  PROP_CONNECTION,
  PROP_PROPERTIES,
};

static void
handle_client_appeared (PinosClient *client, gpointer user_data)
{
  PinosDaemon *daemon = user_data;
  PinosDaemonPrivate *priv = daemon->priv;

  g_debug ("daemon %p: appeared %p", daemon, client);

  g_hash_table_insert (priv->clients, (gpointer) pinos_client_get_sender (client), client);
}

static void
handle_client_vanished (PinosClient *client, gpointer user_data)
{
  PinosDaemon *daemon = user_data;
  PinosDaemonPrivate *priv = daemon->priv;

  g_debug ("daemon %p: vanished %p", daemon, client);
  g_hash_table_remove (priv->clients, (gpointer) pinos_client_get_sender (client));
}

static PinosClient *
sender_get_client (PinosDaemon *daemon,
                   const gchar *sender)
{
  PinosDaemonPrivate *priv = daemon->priv;
  PinosClient *client;

  client = g_hash_table_lookup (priv->clients, sender);
  if (client == NULL) {
    client = pinos_client_new (daemon, sender, NULL);

    g_debug ("daemon %p: new client %p for %s", daemon, client, sender);
    g_signal_connect (client,
                      "appeared",
                      (GCallback) handle_client_appeared,
                      daemon);
    g_signal_connect (client,
                      "vanished",
                      (GCallback) handle_client_vanished,
                      daemon);
  }
  return client;
}

static void
handle_remove_node (PinosNode *node,
                    gpointer   user_data)
{
  PinosClient *client = user_data;

  g_debug ("client %p: node %p remove", daemon, node);
  pinos_client_remove_object (client, G_OBJECT (node));
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
  PinosClient *client;
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
    node = pinos_node_new (daemon,
                           sender,
                           arg_name,
                           props,
                           NULL);
  }

  pinos_properties_free (props);

  if (node == NULL)
    goto no_node;

  client = sender_get_client (daemon, sender);
  pinos_client_add_object (client, G_OBJECT (node));

  g_signal_connect (node,
                    "remove",
                    (GCallback) handle_remove_node,
                    client);

  object_path = pinos_node_get_object_path (node);
  g_debug ("daemon %p: added node %p with path %s", daemon, node, object_path);
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)", object_path));
  g_object_unref (node);

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
on_link_state_notify (GObject     *obj,
                      GParamSpec  *pspec,
                      PinosClient *client)
{
  PinosLink *link = PINOS_LINK (obj);
  GError *error = NULL;
  PinosLinkState state;

  state = pinos_link_get_state (link, &error);
  switch (state) {
    case PINOS_LINK_STATE_ERROR:
      g_warning ("link %p: state error: %s", link, error->message);

      if (link->input_node && pinos_client_has_object (client, G_OBJECT (link->input_node))) {
        pinos_node_report_error (link->input_node, g_error_copy (error));
      }
      if (link->output_node && pinos_client_has_object (client, G_OBJECT (link->output_node))) {
        pinos_node_report_error (link->output_node, g_error_copy (error));
      }
      break;
    case PINOS_LINK_STATE_UNLINKED:
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
on_port_added (PinosNode *node, PinosDirection direction, uint32_t port_id, PinosClient *client)
{
  PinosDaemon *this;
  PinosProperties *props;
  PinosNode *target;
  const gchar *path;
  GError *error = NULL;
  PinosLink *link;

  this = pinos_node_get_daemon (node);

  props = pinos_node_get_properties (node);
  path = pinos_properties_get (props, "pinos.target.node");

  if (path) {
    guint target_port;
    guint node_port;

    target = pinos_daemon_find_node (this,
                                     pinos_direction_reverse (direction),
                                     path,
                                     NULL,
                                     0,
                                     NULL,
                                     &error);
    if (target == NULL)
      goto error;

    target_port = pinos_node_get_free_port (target, pinos_direction_reverse (direction));
    if (target_port == SPA_ID_INVALID) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_NODE_PORT,
                   "can't get free port from target %s", pinos_node_get_object_path (target));
      goto error;
    }
    node_port = pinos_node_get_free_port (node, direction);
    if (node_port == SPA_ID_INVALID) {
      g_set_error (&error,
                   PINOS_ERROR,
                   PINOS_ERROR_NODE_PORT,
                   "can't get free port from node %s", pinos_node_get_object_path (node));
      goto error;
    }

    if (direction == PINOS_DIRECTION_OUTPUT)
      link = pinos_node_link (node, node_port, target, target_port, NULL, NULL, &error);
    else
      link = pinos_node_link (target, target_port, node, node_port, NULL, NULL, &error);

    if (link == NULL)
      goto error;

    pinos_client_add_object (client, G_OBJECT (link));

    g_signal_connect (link, "notify::state", (GCallback) on_link_state_notify, client);
    pinos_link_activate (link);

    g_object_unref (link);
  }
  return;

error:
  {
    pinos_node_report_error (node, error);
    return;
  }
}

static void
on_port_removed (PinosNode *node, uint32_t port_id, PinosClient *client)
{
}

static gboolean
handle_create_client_node (PinosDaemon1           *interface,
                           GDBusMethodInvocation  *invocation,
                           const gchar            *arg_name,
                           GVariant               *arg_properties,
                           gpointer                user_data)
{
  PinosDaemon *daemon = user_data;
  PinosNode *node;
  PinosClient *client;
  const gchar *sender, *object_path;
  PinosProperties *props;
  GError *error = NULL;
  GUnixFDList *fdlist;
  GSocket *socket;
  gint fdidx;

  sender = g_dbus_method_invocation_get_sender (invocation);

  g_debug ("daemon %p: create client-node: %s", daemon, sender);
  props = pinos_properties_from_variant (arg_properties);

  node =  pinos_client_node_new (daemon,
                                 sender,
                                 arg_name,
                                 props);
  pinos_properties_free (props);

  socket = pinos_client_node_get_socket_pair (PINOS_CLIENT_NODE (node), &error);
  if (socket == NULL)
    goto no_socket;

  client = sender_get_client (daemon, sender);
  pinos_client_add_object (client, G_OBJECT (node));

  g_signal_connect (node, "port-added", (GCallback) on_port_added, client);
  g_signal_connect (node, "port-removed", (GCallback) on_port_removed, client);

  object_path = pinos_node_get_object_path (PINOS_NODE (node));
  g_debug ("daemon %p: add client-node %p, %s", daemon, node, object_path);
  g_object_unref (node);

  fdlist = g_unix_fd_list_new ();
  fdidx = g_unix_fd_list_append (fdlist, g_socket_get_fd (socket), &error);
  g_object_unref (socket);

  g_dbus_method_invocation_return_value_with_unix_fd_list (invocation,
           g_variant_new ("(oh)", object_path, fdidx), fdlist);
  g_object_unref (fdlist);

  return TRUE;

no_socket:
  {
    g_debug ("daemon %p: could not create socket %s", daemon, error->message);
    g_object_unref (node);
    goto exit_error;
  }
exit_error:
  {
    g_dbus_method_invocation_return_gerror (invocation, error);
    g_clear_error (&error);
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
 * @node: a #PinosNode
 *
 * Add @node to @daemon.
 */
void
pinos_daemon_add_node (PinosDaemon *daemon,
                       PinosNode   *node)
{
  PinosDaemonPrivate *priv;

  g_return_if_fail (PINOS_IS_DAEMON (daemon));
  g_return_if_fail (PINOS_IS_NODE (node));
  priv = daemon->priv;

  priv->nodes = g_list_prepend (priv->nodes, node);
}

/**
 * pinos_daemon_remove_node:
 * @daemon: a #PinosDaemon
 * @node: a #PinosNode
 *
 * Remove @node from @daemon.
 */
void
pinos_daemon_remove_node (PinosDaemon *daemon,
                          PinosNode   *node)
{
  PinosDaemonPrivate *priv;

  g_return_if_fail (PINOS_IS_DAEMON (daemon));
  g_return_if_fail (PINOS_IS_NODE (node));
  priv = daemon->priv;

  priv->nodes = g_list_remove (priv->nodes, node);
}

/**
 * pinos_daemon_find_node:
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
PinosNode *
pinos_daemon_find_node (PinosDaemon     *daemon,
                        PinosDirection   direction,
                        const gchar     *name,
                        PinosProperties *props,
                        unsigned int     n_format_filters,
                        SpaFormat      **format_filters,
                        GError         **error)
{
  PinosDaemonPrivate *priv;
  PinosNode *best = NULL;
  GList *nodes;
  gboolean have_name;

  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);
  priv = daemon->priv;

  have_name = name ? strlen (name) > 0 : FALSE;

  g_debug ("name \"%s\", %d", name, have_name);

  for (nodes = priv->nodes; nodes; nodes = g_list_next (nodes)) {
    PinosNode *n = nodes->data;

    g_debug ("node path \"%s\"", pinos_node_get_object_path (n));

    if (!g_str_has_suffix (pinos_node_get_object_path (n), name))
      continue;

    g_debug ("name \"%s\" matches node %p", name, n);
    best = n;
    break;
  }
  if (best == NULL) {
    g_set_error (error,
                 G_IO_ERROR,
                 G_IO_ERROR_NOT_FOUND,
                 "No matching Node found");
  }
  return best;
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

    case PROP_CONNECTION:
      g_value_set_object (value, priv->connection);
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
  g_hash_table_unref (priv->clients);
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
                                   PROP_CONNECTION,
                                   g_param_spec_object ("connection",
                                                        "Connection",
                                                        "The DBus connection",
                                                        G_TYPE_DBUS_CONNECTION,
                                                        G_PARAM_READABLE |
                                                        G_PARAM_STATIC_STRINGS));

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
  g_signal_connect (priv->iface, "handle-create-client-node", (GCallback) handle_create_client_node, daemon);

  priv->server_manager = g_dbus_object_manager_server_new (PINOS_DBUS_OBJECT_PREFIX);
  priv->clients = g_hash_table_new (g_str_hash, g_str_equal);
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
