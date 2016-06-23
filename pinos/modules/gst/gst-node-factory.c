#/* Pinos
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>

#include "gst-node-factory.h"
#include "gst-source.h"

G_DEFINE_TYPE (PinosGstNodeFactory, pinos_gst_node_factory, PINOS_TYPE_NODE_FACTORY);

static PinosNode *
factory_create_node (PinosNodeFactory * factory,
                     PinosDaemon * daemon,
                     const gchar * sender,
                     const gchar * name,
                     PinosProperties * properties)
{
  PinosNode *node;

  node = g_object_new (PINOS_TYPE_GST_SOURCE,
                       "daemon", daemon,
                       "sender", sender,
                       "name", name,
                       "properties", properties,
                       NULL);
  return node;
}

static void
pinos_gst_node_factory_class_init (PinosGstNodeFactoryClass * klass)
{
  PinosNodeFactoryClass *factory_class = PINOS_NODE_FACTORY_CLASS (klass);

  factory_class->create_node = factory_create_node;
}

static void
pinos_gst_node_factory_init (PinosGstNodeFactory * factory)
{
}

PinosNodeFactory *
pinos_gst_node_factory_new (const gchar * name)
{
  PinosNodeFactory *factory;

  factory = g_object_new (PINOS_TYPE_GST_NODE_FACTORY,
                          "name", name,
                          NULL);
  return factory;
}
