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

#ifndef __PINOS_GST_NODE_FACTORY_H__
#define __PINOS_GST_NODE_FACTORY_H__

#include <glib-object.h>

#include <server/node-factory.h>

G_BEGIN_DECLS

#define PINOS_TYPE_GST_NODE_FACTORY                 (pinos_gst_node_factory_get_type ())
#define PINOS_IS_GST_NODE_FACTORY(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_GST_NODE_FACTORY))
#define PINOS_IS_GST_NODE_FACTORY_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_GST_NODE_FACTORY))
#define PINOS_GST_NODE_FACTORY_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_GST_NODE_FACTORY, PinosGstNodeFactoryClass))
#define PINOS_GST_NODE_FACTORY(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_GST_NODE_FACTORY, PinosGstNodeFactory))
#define PINOS_GST_NODE_FACTORY_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_GST_NODE_FACTORY, PinosGstNodeFactoryClass))
#define PINOS_GST_NODE_FACTORY_CAST(obj)            ((PinosGstNodeFactory*)(obj))
#define PINOS_GST_NODE_FACTORY_CLASS_CAST(klass)    ((PinosGstNodeFactoryClass*)(klass))

typedef struct _PinosGstNodeFactory PinosGstNodeFactory;
typedef struct _PinosGstNodeFactoryClass PinosGstNodeFactoryClass;

struct _PinosGstNodeFactory {
  PinosNodeFactory object;
};

struct _PinosGstNodeFactoryClass {
  PinosNodeFactoryClass parent_class;
};

GType              pinos_gst_node_factory_get_type        (void);

PinosNodeFactory * pinos_gst_node_factory_new             (const gchar * name);

G_END_DECLS

#endif /* __PINOS_GST_NODE_FACTORY_H__ */
