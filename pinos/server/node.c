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

#include <gio/gio.h>

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

  gchar *object_path;
  gchar *name;

  PinosNodeState state;
  GError *error;
  guint idle_timeout;

  PinosProperties *properties;

  GList *ports;
};

G_DEFINE_TYPE (PinosNode, pinos_node, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_OBJECT_PATH,
  PROP_NAME,
  PROP_STATE,
  PROP_PROPERTIES,
};

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

    case PROP_NAME:
      priv->name = g_value_dup_string (value);
      break;

    case PROP_PROPERTIES:
      if (priv->properties)
        pinos_properties_free (priv->properties);
      priv->properties = g_value_dup_boxed (value);
      if (priv->iface)
        pinos_node1_set_properties (priv->iface,
            priv->properties ?  pinos_properties_to_variant (priv->properties) : NULL);
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

  priv->iface = pinos_node1_skeleton_new ();
  pinos_node1_set_name (priv->iface, priv->name);
  if (priv->properties)
    pinos_node1_set_properties (priv->iface, pinos_properties_to_variant (priv->properties));
  pinos_node1_set_state (priv->iface, priv->state);
  pinos_object_skeleton_set_node1 (skel, priv->iface);

  g_free (priv->object_path);
  priv->object_path = pinos_daemon_export_uniquely (daemon, G_DBUS_OBJECT_SKELETON (skel));
  g_object_unref (skel);

  pinos_daemon_add_node (daemon, node);

  return;
}

static void
node_unregister_object (PinosNode *node)
{
  PinosNodePrivate *priv = node->priv;

  pinos_daemon_unexport (priv->daemon, priv->object_path);
  pinos_daemon_remove_node (priv->daemon, node);
  g_clear_object (&priv->iface);
}

static void
pinos_node_constructed (GObject * obj)
{
  PinosNode *node = PINOS_NODE (obj);

  node_register_object (node);

  G_OBJECT_CLASS (pinos_node_parent_class)->constructed (obj);
}

static void
pinos_node_dispose (GObject * obj)
{
  PinosNode *node = PINOS_NODE (obj);

  node_unregister_object (node);

  G_OBJECT_CLASS (pinos_node_parent_class)->dispose (obj);
}

static void
pinos_node_finalize (GObject * obj)
{
  PinosNode *node = PINOS_NODE (obj);
  PinosNodePrivate *priv = node->priv;

  g_free (priv->object_path);

  G_OBJECT_CLASS (pinos_node_parent_class)->finalize (obj);
}

static void
pinos_node_class_init (PinosNodeClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

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

}

static void
pinos_node_init (PinosNode * node)
{
  node->priv = PINOS_NODE_GET_PRIVATE (node);
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
 * pinos_node_add_port:
 * @node: a #PinosNode
 * @port: a #PinosPort
 *
 * Add the #PinosPort to @node
 */
void
pinos_node_add_port (PinosNode *node, PinosPort *port)
{
  PinosNodePrivate *priv;

  g_return_if_fail (PINOS_IS_NODE (node));
  g_return_if_fail (PINOS_IS_PORT (port));
  priv = node->priv;

  priv->ports = g_list_append (priv->ports, port);
}

/**
 * pinos_node_remove_port:
 * @node: a #PinosNode
 * @port: a #PinosPort
 *
 * Remove the #PinosPort from @node
 */
void
pinos_node_remove_port (PinosNode *node, PinosPort *port)
{
  PinosNodePrivate *priv;

  g_return_if_fail (PINOS_IS_NODE (node));
  g_return_if_fail (PINOS_IS_PORT (port));
  priv = node->priv;

  priv->ports = g_list_remove (priv->ports, port);
}

/**
 * pinos_node_get_ports:
 * @node: a #PinosNode
 *
 * Get the list of ports in @node.
 *
 * Returns: a #GList of nodes owned by @node.
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
    priv->state = state;
    pinos_node1_set_state (priv->iface, state);
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
  g_debug ("got error state %s", error->message);
  pinos_node1_set_state (priv->iface, priv->state);
  g_object_notify (G_OBJECT (node), "state");
}

static gboolean
idle_timeout (PinosNode *node)
{
  PinosNodePrivate *priv = node->priv;

  priv->idle_timeout = 0;
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

  pinos_node_set_state (node, PINOS_NODE_STATE_RUNNING);
}



/**
 * pinos_node_new:
 * @daemon: a #PinosDaemon
 *
 * Make a new node
 *
 * Returns: a new #PinosNode
 */
PinosNode *
pinos_node_new (PinosDaemon *daemon)
{
  return g_object_new (PINOS_TYPE_NODE,
                       "daemon", daemon,
                       NULL);
}
