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

#include "pinos/client/node.h"


#define PINOS_NODE_GET_PRIVATE(node)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((node), PINOS_TYPE_NODE, PinosNodePrivate))

struct _PinosNodePrivate
{
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

static void
pinos_node_get_property (GObject    *_object,
                         guint       prop_id,
                         GValue     *value,
                         GParamSpec *pspec)
{
  PinosNode *node = PINOS_NODE (_object);
  PinosNodePrivate *priv = node->priv;

  switch (prop_id) {
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
pinos_node_constructed (GObject * obj)
{
  PinosNode *node = PINOS_NODE (obj);

  g_debug ("node %p: constructed", node);

  G_OBJECT_CLASS (pinos_node_parent_class)->constructed (obj);
}

static void
pinos_node_dispose (GObject * obj)
{
  PinosNode *node = PINOS_NODE (obj);
  PinosNodePrivate *priv = node->priv;

  g_debug ("node %p: dispose", node);
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

  g_type_class_add_private (klass, sizeof (PinosNodePrivate));

  gobject_class->constructed = pinos_node_constructed;
  gobject_class->dispose = pinos_node_dispose;
  gobject_class->finalize = pinos_node_finalize;
  gobject_class->set_property = pinos_node_set_property;
  gobject_class->get_property = pinos_node_get_property;

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
}

static void
pinos_node_init (PinosNode * node)
{
  PinosNodePrivate *priv = node->priv = PINOS_NODE_GET_PRIVATE (node);

  g_debug ("node %p: new", node);
  priv->state = PINOS_NODE_STATE_SUSPENDED;
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
 * pinos_node_create_port:
 * @node: a #PinosNode
 * @direction: the direction of the port
 * @name: the name of the port
 * @possible_formats: possible media formats for the port
 * @props: extra properties for the port
 *
 * Create a new #PinosPort from @node.
 *
 * Returns: a new #PinosPort that should be freed with
 * pinos_node_remove_port().
 */
void
pinos_node_create_port (PinosNode          *node,
                        PinosDirection      direction,
                        const gchar        *name,
                        GBytes             *possible_formats,
                        PinosProperties    *props,
                        GCancellable       *cancellable,
                        GAsyncReadyCallback callback,
                        gpointer            user_data)
{
  PinosNodeClass *klass;
  GTask *task;

  g_return_if_fail (PINOS_IS_NODE (node));

  klass = PINOS_NODE_GET_CLASS (node);
  if (!klass->create_port)
    return;

  task = g_task_new  (node, cancellable, callback, user_data);
  klass->create_port (node, direction, name, possible_formats, props, task);
}

PinosPort *
pinos_node_create_port_finish (PinosNode       *node,
                               GAsyncResult    *res,
                               GError         **error)
{
  PinosPort *port;
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  port = g_task_propagate_pointer (G_TASK (res), error);
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
