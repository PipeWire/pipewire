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
#include "pinos/server/server-node.h"
#include "pinos/server/upload-node.h"

#include "pinos/dbus/org-pinos.h"

struct _PinosClientPrivate
{
  PinosDaemon *daemon;
  PinosClient1 *iface;

  gchar *sender;
  gchar *object_path;
  PinosProperties *properties;

  PinosFdManager *fdmanager;

  GList *nodes;
};

#define PINOS_CLIENT_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_CLIENT, PinosClientPrivate))

G_DEFINE_TYPE (PinosClient, pinos_client, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_SENDER,
  PROP_OBJECT_PATH,
  PROP_PROPERTIES,
};

enum
{
  SIGNAL_DISCONNECT,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static void
pinos_client_get_property (GObject    *_object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PinosClient *client = PINOS_CLIENT (_object);
  PinosClientPrivate *priv = client->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      g_value_set_object (value, priv->daemon);
      break;

    case PROP_SENDER:
      g_value_set_string (value, priv->sender);
      break;

    case PROP_OBJECT_PATH:
      g_value_set_string (value, priv->object_path);
      break;

    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (client, prop_id, pspec);
      break;
  }
}

static void
pinos_client_set_property (GObject      *_object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosClient *client = PINOS_CLIENT (_object);
  PinosClientPrivate *priv = client->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    case PROP_SENDER:
      priv->sender = g_value_dup_string (value);
      pinos_client1_set_sender (priv->iface, priv->sender);
      break;

    case PROP_PROPERTIES:
      if (priv->properties)
        pinos_properties_free (priv->properties);
      priv->properties = g_value_dup_boxed (value);
      pinos_client1_set_properties (priv->iface, priv->properties ?
          pinos_properties_to_variant (priv->properties) : NULL);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (client, prop_id, pspec);
      break;
  }
}

static void
handle_remove_node (PinosNode *node,
                    gpointer   user_data)
{
  PinosClient *client = user_data;
  PinosClientPrivate *priv = client->priv;

  g_debug ("client %p: remove node %p", client, node);
  priv->nodes = g_list_remove (priv->nodes, node);
  g_object_unref (node);
}

static gboolean
handle_create_node (PinosClient1           *interface,
                    GDBusMethodInvocation  *invocation,
                    const gchar            *arg_name,
                    GVariant               *arg_properties,
                    gpointer                user_data)
{
  PinosClient *client = user_data;
  PinosClientPrivate *priv = client->priv;
  PinosNode *node;
  const gchar *object_path, *sender;
  PinosProperties *props;

  sender = g_dbus_method_invocation_get_sender (invocation);
  if (g_strcmp0 (pinos_client_get_sender (client), sender) != 0)
    goto not_allowed;

  props = pinos_properties_from_variant (arg_properties);
  node = pinos_server_node_new (priv->daemon,
                                sender,
                                arg_name,
                                props);
  pinos_properties_free (props);

  if (node == NULL)
    goto no_node;

  priv->nodes = g_list_prepend (priv->nodes, node);

  g_signal_connect (node,
                    "remove",
                    (GCallback) handle_remove_node,
                    client);

  object_path = pinos_server_node_get_object_path (PINOS_SERVER_NODE (node));
  g_debug ("client %p: add node %p %d, %s", client, node, G_OBJECT (node)->ref_count, object_path);
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)", object_path));

  return TRUE;

  /* ERRORS */
not_allowed:
  {
    g_debug ("sender %s is not owner of client object %s", sender, pinos_client_get_sender (client));
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "sender not client owner");
    return TRUE;
  }
no_node:
  {
    g_debug ("client %p: could create node %s", client, arg_name);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "can't create node");
    pinos_properties_free (props);
    return TRUE;
  }
}

static gboolean
handle_disconnect (PinosClient1           *interface,
                   GDBusMethodInvocation  *invocation,
                   gpointer                user_data)
{
  PinosClient *client = user_data;

  g_debug ("client %p: disconnect", client);
  g_signal_emit (client, signals[SIGNAL_DISCONNECT], 0, NULL);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("()"));
  return TRUE;
}

static void
client_register_object (PinosClient *client)
{
  PinosClientPrivate *priv = client->priv;
  PinosDaemon *daemon = priv->daemon;
  PinosObjectSkeleton *skel;

  skel = pinos_object_skeleton_new (PINOS_DBUS_OBJECT_CLIENT);

  pinos_object_skeleton_set_client1 (skel, priv->iface);

  g_free (priv->object_path);
  priv->object_path = pinos_daemon_export_uniquely (daemon, G_DBUS_OBJECT_SKELETON (skel));
  g_object_unref (skel);

  g_debug ("client %p: register %s", client, priv->object_path);
}

static void
client_unregister_object (PinosClient *client)
{
  PinosClientPrivate *priv = client->priv;
  PinosDaemon *daemon = priv->daemon;

  g_debug ("client %p: unregister", client);
  pinos_daemon_unexport (daemon, priv->object_path);
}

static void
pinos_client_dispose (GObject * object)
{
  PinosClient *client = PINOS_CLIENT (object);
  PinosClientPrivate *priv = client->priv;
  GList *copy;

  g_debug ("client %p: dispose", client);
  pinos_fd_manager_remove_all (priv->fdmanager, priv->object_path);

  copy = g_list_copy (priv->nodes);
  g_list_free_full (copy, (GDestroyNotify) pinos_node_remove);
  client_unregister_object (client);

  G_OBJECT_CLASS (pinos_client_parent_class)->dispose (object);
}

static void
pinos_client_finalize (GObject * object)
{
  PinosClient *client = PINOS_CLIENT (object);
  PinosClientPrivate *priv = client->priv;

  g_debug ("client %p: finalize", client);
  g_clear_object (&priv->daemon);
  g_clear_object (&priv->iface);
  g_free (priv->sender);
  g_free (priv->object_path);
  if (priv->properties)
    pinos_properties_free (priv->properties);
  g_clear_object (&priv->fdmanager);

  G_OBJECT_CLASS (pinos_client_parent_class)->finalize (object);
}

static void
pinos_client_constructed (GObject * object)
{
  PinosClient *client = PINOS_CLIENT (object);

  g_debug ("client %p: constructed", client);
  client_register_object (client);

  G_OBJECT_CLASS (pinos_client_parent_class)->constructed (object);
}

static void
pinos_client_class_init (PinosClientClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosClientPrivate));

  gobject_class->constructed = pinos_client_constructed;
  gobject_class->dispose = pinos_client_dispose;
  gobject_class->finalize = pinos_client_finalize;
  gobject_class->set_property = pinos_client_set_property;
  gobject_class->get_property = pinos_client_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The daemon",
                                                        PINOS_TYPE_DAEMON,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_SENDER,
                                   g_param_spec_string ("sender",
                                                        "Sender",
                                                        "The sender",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_OBJECT_PATH,
                                   g_param_spec_string ("object-path",
                                                        "Object Path",
                                                        "The object path",
                                                        NULL,
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

  signals[SIGNAL_DISCONNECT] = g_signal_new ("disconnect",
                                             G_TYPE_FROM_CLASS (klass),
                                             G_SIGNAL_RUN_LAST,
                                             0,
                                             NULL,
                                             NULL,
                                             g_cclosure_marshal_generic,
                                             G_TYPE_NONE,
                                             0,
                                             G_TYPE_NONE);

}

static void
pinos_client_init (PinosClient * client)
{
  PinosClientPrivate *priv = client->priv = PINOS_CLIENT_GET_PRIVATE (client);

  priv->iface = pinos_client1_skeleton_new ();
  g_signal_connect (priv->iface, "handle-create-node",
                                   (GCallback) handle_create_node,
                                   client);
  g_signal_connect (priv->iface, "handle-disconnect",
                                   (GCallback) handle_disconnect,
                                   client);

  g_debug ("client %p: new", client);
  priv->fdmanager = pinos_fd_manager_get (PINOS_FD_MANAGER_DEFAULT);
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
pinos_client_new (PinosDaemon     *daemon,
                  const gchar     *sender,
                  PinosProperties *properties)
{
  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);

  return g_object_new (PINOS_TYPE_CLIENT, "daemon", daemon,
                                          "sender", sender,
                                          "properties", properties,
                                          NULL);
}

/**
 * pinos_client_get_sender:
 * @client: a #PinosClient
 *
 * Get the sender of @client.
 *
 * Returns: the sender of @client
 */
const gchar *
pinos_client_get_sender (PinosClient *client)
{
  PinosClientPrivate *priv;

  g_return_val_if_fail (PINOS_IS_CLIENT (client), NULL);
  priv = client->priv;

  return priv->sender;
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
  PinosClientPrivate *priv;

  g_return_val_if_fail (PINOS_IS_CLIENT (client), NULL);
  priv = client->priv;

  return priv->object_path;
}
