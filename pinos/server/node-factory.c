/* Pinos
 * Copyright (C) 2016 Axis Communications AB
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

#include "pinos/client/pinos.h"
#include "pinos/server/node-factory.h"

#define PINOS_NODE_FACTORY_GET_PRIVATE(factory)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((factory), PINOS_TYPE_NODE_FACTORY, PinosNodeFactoryPrivate))

struct _PinosNodeFactoryPrivate
{
  PinosDaemon *daemon;

  gchar *name;
};

G_DEFINE_ABSTRACT_TYPE (PinosNodeFactory, pinos_node_factory, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_NAME,
};

static void
pinos_node_factory_get_property (GObject    *_object,
                                 guint       prop_id,
                                 GValue     *value,
                                 GParamSpec *pspec)
{
  PinosNodeFactory *factory = PINOS_NODE_FACTORY (_object);
  PinosNodeFactoryPrivate *priv = factory->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      g_value_set_object (value, priv->daemon);
      break;

    case PROP_NAME:
      g_value_set_string (value, priv->name);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (factory, prop_id, pspec);
      break;
  }
}

static void
pinos_node_factory_set_property (GObject      *_object,
                                 guint         prop_id,
                                 const GValue *value,
                                 GParamSpec   *pspec)
{
  PinosNodeFactory *factory = PINOS_NODE_FACTORY (_object);
  PinosNodeFactoryPrivate *priv = factory->priv;

  switch (prop_id) {
    case PROP_DAEMON:
      priv->daemon = g_value_dup_object (value);
      break;

    case PROP_NAME:
      priv->name = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (factory, prop_id, pspec);
      break;
  }
}

static void
pinos_node_factory_constructed (GObject * obj)
{
  PinosNodeFactory *factory = PINOS_NODE_FACTORY (obj);
  PinosNodeFactoryPrivate *priv = factory->priv;

  g_debug ("node factory %p: constructed", factory);

  pinos_object_init (&factory->object,
                     priv->daemon->registry.uri.node_factory,
                     factory,
                     NULL);

  pinos_registry_add_object (&priv->daemon->registry, &factory->object);

  G_OBJECT_CLASS (pinos_node_factory_parent_class)->constructed (obj);
}

static void
pinos_node_factory_finalize (GObject * obj)
{
  PinosNodeFactory *factory = PINOS_NODE_FACTORY (obj);
  PinosNodeFactoryPrivate *priv = factory->priv;

  g_debug ("node factory %p: finalize", factory);
  pinos_registry_remove_object (&priv->daemon->registry, &factory->object);

  g_free (priv->name);

  G_OBJECT_CLASS (pinos_node_factory_parent_class)->finalize (obj);
}

static void
pinos_node_factory_class_init (PinosNodeFactoryClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosNodeFactoryPrivate));

  gobject_class->constructed = pinos_node_factory_constructed;
  gobject_class->finalize = pinos_node_factory_finalize;
  gobject_class->set_property = pinos_node_factory_set_property;
  gobject_class->get_property = pinos_node_factory_get_property;

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
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The node factory name",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
}

static void
pinos_node_factory_init (PinosNodeFactory * factory)
{
  factory->priv = PINOS_NODE_FACTORY_GET_PRIVATE (factory);

  g_debug ("node_factory %p: new", factory);
}

/**
 * pinos_node_factory_get_name:
 * @factory: a #PinosNodeFactory
 *
 * Get the name of @factory
 *
 * Returns: the name of @factory
 */
const gchar *
pinos_node_factory_get_name (PinosNodeFactory *factory)
{
  PinosNodeFactoryPrivate *priv;

  g_return_val_if_fail (PINOS_IS_NODE_FACTORY (factory), NULL);
  priv = factory->priv;

  return priv->name;
}

/**
 * pinos_node_factory_create_node:
 * @factory: A #PinosNodeFactory
 * @client: the owner client
 * @name: node name
 * @props: #PinosProperties for the node
 *
 * Create a #PinosNode
 *
 * Returns: (transfer full) (nullable): a new #PinosNode.
 */
PinosNode *
pinos_node_factory_create_node (PinosNodeFactory *factory,
                                PinosClient *client,
                                const gchar *name,
                                PinosProperties *props)
{
  PinosNodeFactoryClass *klass;

  g_return_val_if_fail (PINOS_IS_NODE_FACTORY (factory), NULL);

  klass = PINOS_NODE_FACTORY_GET_CLASS (factory);
  if (!klass->create_node)
    return NULL;

  g_debug ("node factory %p: create node", factory);
  return klass->create_node (factory, factory->priv->daemon, client, name, props);
}
