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
#include "pinos/server/source.h"
#include "pinos/server/daemon.h"

#include "pinos/dbus/org-pinos.h"


#define PINOS_NODE_GET_PRIVATE(node)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((node), PINOS_TYPE_NODE, PinosNodePrivate))

struct _PinosNodePrivate
{
  PinosDaemon *daemon;
  PinosObjectSkeleton *skeleton;
  gchar *object_path;
  PinosSource *source;
  PinosSink *sink;
};

G_DEFINE_TYPE (PinosNode, pinos_node, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_SKELETON,
  PROP_OBJECT_PATH,
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

    case PROP_SKELETON:
      g_value_set_object (value, priv->skeleton);
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

  priv->skeleton = pinos_object_skeleton_new (PINOS_DBUS_OBJECT_NODE);

  g_free (priv->object_path);
  priv->object_path = pinos_daemon_export_uniquely (daemon, G_DBUS_OBJECT_SKELETON (priv->skeleton));

  pinos_daemon_add_node (daemon, node);

  return;
}

static void
node_unregister_object (PinosNode *node)
{
  PinosNodePrivate *priv = node->priv;

  pinos_daemon_unexport (priv->daemon, priv->object_path);
  pinos_daemon_remove_node (priv->daemon, node);
  g_clear_object (&priv->skeleton);
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
 * pinos_node_set_source:
 * @node: a #PinosNode
 * @source: a #PinosSource
 *
 * Set the #PinosSource of @node
 */
void
pinos_node_set_source (PinosNode *node, PinosSource *source, GObject *iface)
{
  PinosNodePrivate *priv;

  g_return_if_fail (PINOS_IS_NODE (node));
  g_return_if_fail (source == NULL || PINOS_IS_SOURCE (source));
  g_return_if_fail (iface == NULL || PINOS_IS_SOURCE1 (iface));
  priv = node->priv;

  if (source) {
    pinos_object_skeleton_set_source1 (priv->skeleton, PINOS_SOURCE1 (iface));
    priv->source = source;
  } else {
    pinos_object_skeleton_set_source1 (priv->skeleton, NULL);
    priv->source = NULL;
  }
}

/**
 * pinos_node_get_source:
 * @node: a #PinosNode
 *
 * Get the #PinosSource of @node
 *
 * Returns: the #PinosSource of @node or %NULL
 */
PinosSource *
pinos_node_get_source (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  return priv->source;
}


/**
 * pinos_node_set_sink:
 * @node: a #PinosNode
 * @sink: a #PinosSink
 *
 * Set the #PinosSink of @node
 */
void
pinos_node_set_sink (PinosNode *node, PinosSink *sink, GObject *iface)
{
  PinosNodePrivate *priv;

  g_return_if_fail (PINOS_IS_NODE (node));
  g_return_if_fail (sink == NULL || PINOS_IS_SINK (sink));
  g_return_if_fail (iface == NULL || PINOS_IS_SINK1 (iface));
  priv = node->priv;

  if (sink) {
    pinos_object_skeleton_set_sink1 (priv->skeleton, PINOS_SINK1 (iface));
    priv->sink = sink;
  } else {
    pinos_object_skeleton_set_sink1 (priv->skeleton, NULL);
    priv->sink = NULL;
  }
}

/**
 * pinos_node_get_sink:
 * @node: a #PinosNode
 *
 * Get the #PinosSink of @node
 *
 * Returns: the #PinosSink of @node or %NULL
 */
PinosSink *
pinos_node_get_sink (PinosNode *node)
{
  PinosNodePrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE (node), NULL);
  priv = node->priv;

  return priv->sink;
}

PinosNode *
pinos_node_new (PinosDaemon *daemon)
{
  return g_object_new (PINOS_TYPE_NODE,
                       "daemon", daemon,
                       NULL);
}
