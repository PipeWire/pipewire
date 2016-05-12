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

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"

#include "pinos/server/server-node.h"
#include "pinos/server/daemon.h"

#include "pinos/dbus/org-pinos.h"


#define PINOS_SERVER_NODE_GET_PRIVATE(node)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((node), PINOS_TYPE_SERVER_NODE, PinosServerNodePrivate))

struct _PinosServerNodePrivate
{
  PinosDaemon *daemon;
  PinosNode1 *iface;

  gchar *sender;
  gchar *object_path;
};

G_DEFINE_TYPE (PinosServerNode, pinos_server_node, PINOS_TYPE_NODE);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_SENDER,
  PROP_OBJECT_PATH,
};

static gboolean
server_node_set_state (PinosNode       *node,
                       PinosNodeState   state)
{
  return FALSE;
}

static void
server_node_create_port (PinosNode       *node,
                         PinosDirection   direction,
                         const gchar     *name,
                         GBytes          *possible_formats,
                         PinosProperties *props,
                         GTask           *task)
{
  PinosServerNodePrivate *priv = PINOS_SERVER_NODE (node)->priv;
  PinosPort *port;

  port = g_object_new (PINOS_TYPE_SERVER_PORT,
                       "daemon", priv->daemon,
                       "node", node,
                       "direction", direction,
                       "name", name,
                       "possible-formats", possible_formats,
                       "properties", props,
                       NULL);

  g_task_return_pointer (task, port, (GDestroyNotify) g_object_unref);
}

static void
server_node_remove_port (PinosNode       *node,
                         PinosPort       *port)
{
}

static void
on_port_created (GObject      *source_object,
                 GAsyncResult *res,
                 gpointer      user_data)
{
  PinosNode *node = PINOS_NODE (source_object);
  GDBusMethodInvocation *invocation = user_data;
  PinosPort *port;
  const gchar *object_path;
  GError *error = NULL;

  port = pinos_node_create_port_finish (node, res, &error);
  if (port == NULL)
    goto no_port;

  object_path = pinos_server_port_get_object_path (PINOS_SERVER_PORT (port));
  g_debug ("node %p: add port %p, %s", node, port, object_path);
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(o)", object_path));

  return;

no_port:
  {
    g_debug ("server-node %p: could create port", node);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "can't create port");
    return;
  }
}


static gboolean
handle_create_port (PinosNode1             *interface,
                    GDBusMethodInvocation  *invocation,
                    PinosDirection          arg_direction,
                    const gchar            *arg_name,
                    GVariant               *arg_properties,
                    const gchar            *arg_possible_formats,
                    gpointer                user_data)
{
  PinosNode *node = user_data;
  PinosServerNodePrivate *priv = PINOS_SERVER_NODE (node)->priv;
  const gchar *sender;
  PinosProperties *props;
  GBytes *formats;

  sender = g_dbus_method_invocation_get_sender (invocation);
  if (g_strcmp0 (priv->sender, sender) != 0)
    goto not_allowed;

  formats = g_bytes_new (arg_possible_formats, strlen (arg_possible_formats) + 1);
  props = pinos_properties_from_variant (arg_properties);

  pinos_node_create_port (node,
                          arg_direction,
                          arg_name,
                          formats,
                          props,
                          NULL,
                          on_port_created,
                          invocation);

  g_bytes_unref (formats);
  pinos_properties_free (props);

  return TRUE;

  /* ERRORS */
not_allowed:
  {
    g_debug ("sender %s is not owner of node with sender %s", sender, priv->sender);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "not node owner");
    return TRUE;
  }
}

static gboolean
handle_remove (PinosNode1             *interface,
               GDBusMethodInvocation  *invocation,
               gpointer                user_data)
{
  PinosNode *node = user_data;

  g_debug ("server-node %p: remove", node);
  pinos_node_remove (node);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("()"));
  return TRUE;
}

static void
pinos_server_node_get_property (GObject    *_object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  PinosServerNode *node = PINOS_SERVER_NODE (_object);
  PinosServerNodePrivate *priv = node->priv;

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

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (node, prop_id, pspec);
      break;
  }
}

static void
pinos_server_node_set_property (GObject      *_object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  PinosServerNode *node = PINOS_SERVER_NODE (_object);
  PinosServerNodePrivate *priv = node->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    case PROP_SENDER:
      priv->sender = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (node, prop_id, pspec);
      break;
  }
}

static void
node_register_object (PinosServerNode *node)
{
  PinosServerNodePrivate *priv = node->priv;
  PinosDaemon *daemon = priv->daemon;
  PinosObjectSkeleton *skel;

  skel = pinos_object_skeleton_new (PINOS_DBUS_OBJECT_NODE);

  pinos_object_skeleton_set_node1 (skel, priv->iface);

  g_free (priv->object_path);
  priv->object_path = pinos_daemon_export_uniquely (daemon, G_DBUS_OBJECT_SKELETON (skel));
  g_object_unref (skel);

  g_debug ("server-node %p: register object %s", node, priv->object_path);
  pinos_daemon_add_node (daemon, node);

  return;
}

static void
node_unregister_object (PinosServerNode *node)
{
  PinosServerNodePrivate *priv = node->priv;

  g_debug ("server-node %p: unregister object %s", node, priv->object_path);
  pinos_daemon_unexport (priv->daemon, priv->object_path);
  pinos_daemon_remove_node (priv->daemon, node);
}

static void
pinos_server_node_constructed (GObject * obj)
{
  PinosServerNode *node = PINOS_SERVER_NODE (obj);

  g_debug ("server-node %p: constructed", node);
  node_register_object (node);

  G_OBJECT_CLASS (pinos_server_node_parent_class)->constructed (obj);
}

static void
pinos_server_node_dispose (GObject * obj)
{
  PinosServerNode *node = PINOS_SERVER_NODE (obj);

  g_debug ("server-node %p: dispose", node);
  node_unregister_object (node);

  G_OBJECT_CLASS (pinos_server_node_parent_class)->dispose (obj);
}

static void
pinos_server_node_finalize (GObject * obj)
{
  PinosServerNode *node = PINOS_SERVER_NODE (obj);
  PinosServerNodePrivate *priv = node->priv;

  g_debug ("server-node %p: finalize", node);
  g_clear_object (&priv->daemon);
  g_clear_object (&priv->iface);

  G_OBJECT_CLASS (pinos_server_node_parent_class)->finalize (obj);
}

static void
pinos_server_node_class_init (PinosServerNodeClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PinosNodeClass *node_class = PINOS_NODE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosServerNodePrivate));

  gobject_class->constructed = pinos_server_node_constructed;
  gobject_class->dispose = pinos_server_node_dispose;
  gobject_class->finalize = pinos_server_node_finalize;
  gobject_class->set_property = pinos_server_node_set_property;
  gobject_class->get_property = pinos_server_node_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "The Daemon",
                                                        PINOS_TYPE_DAEMON,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_SENDER,
                                   g_param_spec_string ("sender",
                                                        "Sender",
                                                        "The Sender",
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
  node_class->set_state = server_node_set_state;
  node_class->create_port = server_node_create_port;
  node_class->remove_port = server_node_remove_port;
}

static void
pinos_server_node_init (PinosServerNode * node)
{
  PinosServerNodePrivate *priv = node->priv = PINOS_SERVER_NODE_GET_PRIVATE (node);

  g_debug ("server-node %p: new", node);
  priv->iface = pinos_node1_skeleton_new ();
  g_signal_connect (priv->iface, "handle-create-port",
                                 (GCallback) handle_create_port,
                                 node);
  g_signal_connect (priv->iface, "handle-remove",
                                 (GCallback) handle_remove,
                                 node);
  pinos_node1_set_state (priv->iface, PINOS_NODE_STATE_SUSPENDED);
}

/**
 * pinos_server_node_new:
 * @daemon: a #PinosDaemon
 * @sender: the path of the owner
 * @name: a name
 * @properties: extra properties
 *
 * Create a new #PinosServerNode.
 *
 * Returns: a new #PinosServerNode
 */
PinosNode *
pinos_server_node_new (PinosDaemon     *daemon,
                       const gchar     *sender,
                       const gchar     *name,
                       PinosProperties *properties)
{
  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);

  return g_object_new (PINOS_TYPE_SERVER_NODE,
                       "daemon", daemon,
                       "sender", sender,
                       "name", name,
                       "properties", properties,
                       NULL);
}

/**
 * pinos_server_node_get_daemon:
 * @node: a #PinosServerNode
 *
 * Get the daemon of @node.
 *
 * Returns: the daemon of @node.
 */
PinosDaemon *
pinos_server_node_get_daemon (PinosServerNode *node)
{
  PinosServerNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_SERVER_NODE (node), NULL);
  priv = node->priv;

 return priv->daemon;
}

/**
 * pinos_server_node_get_sender:
 * @node: a #PinosServerNode
 *
 * Get the owner path of @node.
 *
 * Returns: the owner path of @node.
 */
const gchar *
pinos_server_node_get_sender (PinosServerNode *node)
{
  PinosServerNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_SERVER_NODE (node), NULL);
  priv = node->priv;

  return priv->sender;
}
/**
 * pinos_server_node_get_object_path:
 * @node: a #PinosServerNode
 *
 * Get the object path of @node.
 *
 * Returns: the object path of @node.
 */
const gchar *
pinos_server_node_get_object_path (PinosServerNode *node)
{
  PinosServerNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_SERVER_NODE (node), NULL);
  priv = node->priv;

  return priv->object_path;
}
