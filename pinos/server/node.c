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
#include <stdlib.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"

#include "pinos/server/node.h"
#include "pinos/server/daemon.h"

#include "pinos/dbus/org-pinos.h"

#define PINOS_NODE_GET_PRIVATE(node)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((node), PINOS_TYPE_NODE, PinosNodePrivate))

struct _PinosNodePrivate
{
  PinosDaemon *daemon;
  PinosNode1 *iface;

  gchar *sender;
  gchar *object_path;
  gchar *name;

  PinosNodeState state;
  GError *error;
  guint idle_timeout;

  PinosProperties *properties;

  GList *ports;
};

G_DEFINE_ABSTRACT_TYPE (PinosNode, pinos_node, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_SENDER,
  PROP_OBJECT_PATH,
  PROP_NAME,
  PROP_STATE,
  PROP_PROPERTIES,
};

enum
{
  SIGNAL_REMOVE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

static gboolean
node_set_state (PinosNode       *node,
                PinosNodeState   state)
{
  return FALSE;
}

static PinosPort *
node_add_port (PinosNode       *node,
               PinosDirection   direction,
               GError         **error)
{
  PinosNodePrivate *priv = node->priv;
  PinosPort *port;

  port = g_object_new (PINOS_TYPE_PORT,
                       "daemon", priv->daemon,
                       "node", node,
                       "direction", direction,
                       NULL);
  return port;
}

static void
node_remove_port (PinosNode       *node,
                  PinosPort       *port)
{
}

static gboolean
handle_add_port (PinosNode1             *interface,
                 GDBusMethodInvocation  *invocation,
                 PinosDirection          arg_direction,
                 gpointer                user_data)
{
  PinosNode *node = user_data;
  PinosNodePrivate *priv = node->priv;
  const gchar *sender;
  PinosPort *port;
  GError *error = NULL;

  sender = g_dbus_method_invocation_get_sender (invocation);
  if (g_strcmp0 (priv->sender, sender) != 0)
    goto not_allowed;

  port = pinos_node_add_port (node, arg_direction, &error);
  if (port == NULL)
    goto no_port;

  g_debug ("node %p: add port %p", node, port);
  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("(u)", 1));

  return TRUE;

  /* ERRORS */
not_allowed:
  {
    g_debug ("sender %s is not owner of node with sender %s", sender, priv->sender);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "not node owner");
    return TRUE;
  }
no_port:
  {
    g_debug ("node %p: could create port", node);
    g_dbus_method_invocation_return_dbus_error (invocation,
                 "org.pinos.Error", "can't create port");
    return TRUE;
  }
}

static gboolean
handle_remove (PinosNode1             *interface,
               GDBusMethodInvocation  *invocation,
               gpointer                user_data)
{
  PinosNode *node = user_data;

  g_debug ("node %p: remove", node);
  pinos_node_remove (node);

  g_dbus_method_invocation_return_value (invocation,
                                         g_variant_new ("()"));
  return TRUE;
}

static void
pinos_node_get_property (GObject    *_object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  PinosNode *node = PINOS_NODE (_object);
  PinosNodePrivate *priv = node->priv;

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

    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    case PROP_STATE:
      g_value_set_enum (value, priv->state);
      break;

    case PROP_PROPERTIES:
      g_value_set_boxed (value, priv->properties);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (node, prop_id, pspec);
      break;
  }
}

static void
pinos_node_set_property (GObject      *_object,
                         guint         prop_id,
                         const GValue *value,
                         GParamSpec   *pspec)
{
  PinosNode *node = PINOS_NODE (_object);
  PinosNodePrivate *priv = node->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    case PROP_SENDER:
      priv->sender = g_value_dup_string (value);
      break;

    case PROP_NAME:
      priv->name = g_value_dup_string (value);
      break;

    case PROP_PROPERTIES:
      if (priv->properties)
        pinos_properties_free (priv->properties);
      priv->properties = g_value_dup_boxed (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (node, prop_id, pspec);
      break;
  }
}

static void
node_register_object (PinosNode *node)
{
  PinosNodePrivate *priv = node->priv;
  PinosDaemon *daemon = priv->daemon;
  PinosObjectSkeleton *skel;

  skel = pinos_object_skeleton_new (PINOS_DBUS_OBJECT_NODE);

  pinos_object_skeleton_set_node1 (skel, priv->iface);

  g_free (priv->object_path);
  priv->object_path = pinos_daemon_export_uniquely (daemon, G_DBUS_OBJECT_SKELETON (skel));
  g_object_unref (skel);

  g_debug ("node %p: register object %s", node, priv->object_path);
  pinos_daemon_add_node (daemon, node);

  return;
}

static void
node_unregister_object (PinosNode *node)
{
  PinosNodePrivate *priv = node->priv;

  g_debug ("node %p: unregister object %s", node, priv->object_path);
  pinos_daemon_unexport (priv->daemon, priv->object_path);
  pinos_daemon_remove_node (priv->daemon, node);
}

static void
on_property_notify (GObject    *obj,
                    GParamSpec *pspec,
                    gpointer    user_data)
{
  PinosNode *node = user_data;
  PinosNodePrivate *priv = node->priv;

  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "sender") == 0) {
    pinos_node1_set_owner (priv->iface, priv->sender);
  }
  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "name") == 0) {
    pinos_node1_set_name (priv->iface, pinos_node_get_name (node));
  }
  if (pspec == NULL || strcmp (g_param_spec_get_name (pspec), "properties") == 0) {
    PinosProperties *props = pinos_node_get_properties (node);
    pinos_node1_set_properties (priv->iface, props ? pinos_properties_to_variant (props) : NULL);
  }
}

static void
pinos_node_constructed (GObject * obj)
{
  PinosNode *node = PINOS_NODE (obj);
  PinosNodePrivate *priv = node->priv;

  g_debug ("node %p: constructed", node);

  g_signal_connect (node, "notify", (GCallback) on_property_notify, node);
  G_OBJECT_CLASS (pinos_node_parent_class)->constructed (obj);

  if (priv->sender == NULL) {
    priv->sender = g_strdup (pinos_daemon_get_sender (priv->daemon));
    pinos_node1_set_owner (priv->iface, priv->sender);
  }

  node_register_object (node);
}

static void
pinos_node_dispose (GObject * obj)
{
  PinosNode *node = PINOS_NODE (obj);
  PinosNodePrivate *priv = node->priv;

  pinos_node_set_state (node, PINOS_NODE_STATE_SUSPENDED);

  g_debug ("node %p: dispose", node);
  node_unregister_object (node);

  g_list_free_full (priv->ports, (GDestroyNotify) g_object_unref);
  priv->ports = NULL;

  G_OBJECT_CLASS (pinos_node_parent_class)->dispose (obj);
}

static void
pinos_node_finalize (GObject * obj)
{
  PinosNode *node = PINOS_NODE (obj);
  PinosNodePrivate *priv = node->priv;

  g_debug ("node %p: finalize", node);
  g_clear_object (&priv->daemon);
  g_clear_object (&priv->iface);
  g_free (priv->sender);
  g_free (priv->name);
  g_clear_error (&priv->error);
  if (priv->properties)
    pinos_properties_free (priv->properties);

  G_OBJECT_CLASS (pinos_node_parent_class)->finalize (obj);
}

static void
pinos_node_class_init (PinosNodeClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  PinosNodeClass *node_class = PINOS_NODE_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosNodePrivate));

  gobject_class->constructed = pinos_node_constructed;
  gobject_class->dispose = pinos_node_dispose;
  gobject_class->finalize = pinos_node_finalize;
  gobject_class->set_property = pinos_node_set_property;
  gobject_class->get_property = pinos_node_get_property;

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

  g_object_class_install_property (gobject_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The node name",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_STATE,
                                   g_param_spec_enum ("state",
                                                      "State",
                                                      "The state of the node",
                                                      PINOS_TYPE_NODE_STATE,
                                                      PINOS_NODE_STATE_SUSPENDED,
                                                      G_PARAM_READABLE |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_PROPERTIES,
                                   g_param_spec_boxed ("properties",
                                                       "Properties",
                                                       "The properties of the node",
                                                       PINOS_TYPE_PROPERTIES,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_CONSTRUCT |
                                                       G_PARAM_STATIC_STRINGS));

  signals[SIGNAL_REMOVE] = g_signal_new ("remove",
                                         G_TYPE_FROM_CLASS (klass),
                                         G_SIGNAL_RUN_LAST,
                                         0,
                                         NULL,
                                         NULL,
                                         g_cclosure_marshal_generic,
                                         G_TYPE_NONE,
                                         0,
                                         G_TYPE_NONE);

  node_class->set_state = node_set_state;
  node_class->add_port = node_add_port;
  node_class->remove_port = node_remove_port;
}

static void
pinos_node_init (PinosNode * node)
{
  PinosNodePrivate *priv = node->priv = PINOS_NODE_GET_PRIVATE (node);

  g_debug ("node %p: new", node);
  priv->iface = pinos_node1_skeleton_new ();
  g_signal_connect (priv->iface, "handle-add-port",
                                 (GCallback) handle_add_port,
                                 node);
  g_signal_connect (priv->iface, "handle-remove",
                                 (GCallback) handle_remove,
                                 node);
  priv->state = PINOS_NODE_STATE_SUSPENDED;
  pinos_node1_set_state (priv->iface, PINOS_NODE_STATE_SUSPENDED);
}

/**
 * pinos_node_new:
 * @daemon: a #PinosDaemon
 * @sender: the path of the owner
 * @name: a name
 * @properties: extra properties
 *
 * Create a new #PinosNode.
 *
 * Returns: a new #PinosNode
 */
PinosNode *
pinos_node_new (PinosDaemon     *daemon,
                const gchar     *sender,
                const gchar     *name,
                PinosProperties *properties)
{
  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);

  return g_object_new (PINOS_TYPE_NODE,
                       "daemon", daemon,
                       "sender", sender,
                       "name", name,
                       "properties", properties,
                       NULL);
}

/**
 * pinos_node_get_name:
 * @node: a #PinosNode
 *
 * Get the name of @node
 *
 * Returns: the name of @node
 */
const gchar *
pinos_node_get_name (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  return priv->name;
}

/**
 * pinos_node_get_state:
 * @node: a #PinosNode
 *
 * Get the state of @node
 *
 * Returns: the state of @node
 */
PinosNodeState
pinos_node_get_state (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), PINOS_NODE_STATE_ERROR);
  priv = node->priv;

  return priv->state;
}

/**
 * pinos_node_get_properties:
 * @node: a #PinosNode
 *
 * Get the properties of @node
 *
 * Returns: the properties of @node
 */
PinosProperties *
pinos_node_get_properties (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  return priv->properties;
}

/**
 * pinos_node_get_daemon:
 * @node: a #PinosNode
 *
 * Get the daemon of @node.
 *
 * Returns: the daemon of @node.
 */
PinosDaemon *
pinos_node_get_daemon (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

 return priv->daemon;
}

/**
 * pinos_node_get_sender:
 * @node: a #PinosNode
 *
 * Get the owner path of @node.
 *
 * Returns: the owner path of @node.
 */
const gchar *
pinos_node_get_sender (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  return priv->sender;
}
/**
 * pinos_node_get_object_path:
 * @node: a #PinosNode
 *
 * Get the object path of @node.
 *
 * Returns: the object path of @node.
 */
const gchar *
pinos_node_get_object_path (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  return priv->object_path;
}

/**
 * pinos_node_remove:
 * @node: a #PinosNode
 *
 * Remove @node. This will stop the transfer on the node and
 * free the resources allocated by @node.
 */
void
pinos_node_remove (PinosNode *node)
{
  g_return_if_fail (PINOS_IS_NODE (node));

  g_debug ("node %p: remove", node);
  g_signal_emit (node, signals[SIGNAL_REMOVE], 0, NULL);
}

static void
handle_remove_port (PinosPort *port, PinosNode *node)
{
  pinos_node_remove_port (node, port);
}

/**
 * pinos_node_add_port:
 * @node: a #PinosNode
 * @direction: the direction of the port
 * @error: location of #GError
 *
 * Add the #PinosPort to @node
 *
 * Returns: a new #PinosPort or %NULL
 */
PinosPort *
pinos_node_add_port (PinosNode       *node,
                     PinosDirection   direction,
                     GError         **error)
{
  PinosNodeClass *klass;
  PinosNodePrivate *priv;
  PinosPort *port;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  klass = PINOS_NODE_GET_CLASS (node);
  if (!klass->add_port) {
    g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "add-port not implemented");
    return NULL;
  }

  g_debug ("node %p: add port", node);
  port = klass->add_port (node, direction, error);

  if (port) {
    priv->ports = g_list_append (priv->ports, port);
    g_signal_connect (port, "remove", (GCallback) handle_remove_port, node);
  }

  return port;
}

/**
 * pinos_node_remove_port:
 * @node: a #PinosNode
 * @port: (transfer full): a #PinosPort
 *
 * Remove the #PinosPort from @node
 */
void
pinos_node_remove_port (PinosNode *node, PinosPort *port)
{
  PinosNodePrivate *priv;
  PinosNodeClass *klass;
  GList *find;

  g_return_if_fail (PINOS_IS_NODE (node));
  g_return_if_fail (PINOS_IS_PORT (port));
  priv = node->priv;

  find = g_list_find (priv->ports, port);
  if (find) {
    klass = PINOS_NODE_GET_CLASS (node);

    if (klass->remove_port)
      klass->remove_port (node, port);

    g_debug ("node %p: removed port %p", node, port);
    priv->ports = g_list_delete_link (priv->ports, find);
    g_object_unref (port);
  }
}

/**
 * pinos_node_get_ports:
 * @node: a #PinosNode
 *
 * Get the list of ports in @node.
 *
 * Returns: a #GList of ports owned by @node.
 */
GList *
pinos_node_get_ports (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  return priv->ports;
}

static void
remove_idle_timeout (PinosNode *node)
{
  PinosNodePrivate *priv = node->priv;

  if (priv->idle_timeout) {
    g_source_remove (priv->idle_timeout);
    priv->idle_timeout = 0;
  }
}

/**
 * pinos_node_set_state:
 * @node: a #PinosNode
 * @state: a #PinosNodeState
 *
 * Set the state of @node to @state.
 *
 * Returns: %TRUE on success.
 */
gboolean
pinos_node_set_state (PinosNode      *node,
                      PinosNodeState  state)
{
  PinosNodeClass *klass;
  gboolean res;

  g_return_val_if_fail (PINOS_IS_NODE (node), FALSE);

  klass = PINOS_NODE_GET_CLASS (node);

  remove_idle_timeout (node);

  g_debug ("node %p: set state to %s", node, pinos_node_state_as_string (state));
  if (klass->set_state)
    res = klass->set_state (node, state);
  else
    res = FALSE;

  return res;
}

/**
 * pinos_node_update_state:
 * @node: a #PinosNode
 * @state: a #PinosNodeState
 *
 * Update the state of a node. This method is used from
 * inside @node itself.
 */
void
pinos_node_update_state (PinosNode      *node,
                         PinosNodeState  state)
{
  PinosNodePrivate *priv;

  g_return_if_fail (PINOS_IS_NODE (node));
  priv = node->priv;

  if (priv->state != state) {
    g_debug ("node %p: update state to %s", node, pinos_node_state_as_string (state));
    priv->state = state;
    g_object_notify (G_OBJECT (node), "state");
  }
}

/**
 * pinos_node_report_error:
 * @node: a #PinosNode
 * @error: a #GError
 *
 * Report an error from within @node.
 */
void
pinos_node_report_error (PinosNode *node,
                         GError    *error)
{
  PinosNodePrivate *priv;

  g_return_if_fail (PINOS_IS_NODE (node));
  priv = node->priv;

  g_clear_error (&priv->error);
  remove_idle_timeout (node);
  priv->error = error;
  priv->state = PINOS_NODE_STATE_ERROR;
  g_debug ("node %p: got error state %s", node, error->message);
  g_object_notify (G_OBJECT (node), "state");
}

static gboolean
idle_timeout (PinosNode *node)
{
  PinosNodePrivate *priv = node->priv;

  priv->idle_timeout = 0;
  g_debug ("node %p: idle timeout", node);
  pinos_node_set_state (node, PINOS_NODE_STATE_SUSPENDED);

  return G_SOURCE_REMOVE;
}

/**
 * pinos_node_report_idle:
 * @node: a #PinosNode
 *
 * Mark @node as being idle. This will start a timeout that will
 * set the node to SUSPENDED.
 */
void
pinos_node_report_idle (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_if_fail (PINOS_IS_NODE (node));
  priv = node->priv;

  g_debug ("node %p: report idle", node);
  pinos_node_set_state (node, PINOS_NODE_STATE_IDLE);

  priv->idle_timeout = g_timeout_add_seconds (3,
                                              (GSourceFunc) idle_timeout,
                                              node);
}

/**
 * pinos_node_report_busy:
 * @node: a #PinosNode
 *
 * Mark @node as being busy. This will set the state of the node
 * to the RUNNING state.
 */
void
pinos_node_report_busy (PinosNode *node)
{
  g_return_if_fail (PINOS_IS_NODE (node));

  g_debug ("node %p: report busy", node);
  pinos_node_set_state (node, PINOS_NODE_STATE_RUNNING);
}
