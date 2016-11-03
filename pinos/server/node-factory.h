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

#ifndef __PINOS_NODE_FACTORY_H__
#define __PINOS_NODE_FACTORY_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosNodeFactory PinosNodeFactory;
typedef struct _PinosNodeFactoryClass PinosNodeFactoryClass;
typedef struct _PinosNodeFactoryPrivate PinosNodeFactoryPrivate;

#include <pinos/server/daemon.h>
#include <pinos/server/client.h>

#define PINOS_TYPE_NODE_FACTORY                 (pinos_node_factory_get_type ())
#define PINOS_IS_NODE_FACTORY(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_NODE_FACTORY))
#define PINOS_IS_NODE_FACTORY_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_NODE_FACTORY))
#define PINOS_NODE_FACTORY_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_NODE_FACTORY, PinosNodeFactoryClass))
#define PINOS_NODE_FACTORY(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_NODE_FACTORY, PinosNodeFactory))
#define PINOS_NODE_FACTORY_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_NODE_FACTORY, PinosNodeFactoryClass))
#define PINOS_NODE_FACTORY_CAST(obj)            ((PinosNodeFactory*)(obj))
#define PINOS_NODE_FACTORY_CLASS_CAST(klass)    ((PinosNodeFactoryClass*)(klass))

/**
 * PinosNodeFactory:
 *
 * Pinos node factory class.
 */
struct _PinosNodeFactory {
  GObject object;

  uint32_t id;

  PinosNodeFactoryPrivate *priv;
};

/**
 * PinosNodeFactoryClass:
 * @create_node: make a new node
 *
 * Pinos node factory class.
 */
struct _PinosNodeFactoryClass {
  GObjectClass parent_class;

  PinosNode *      (*create_node) (PinosNodeFactory *factory,
                                   PinosDaemon *daemon,
                                   PinosClient *client,
                                   const gchar *name,
                                   PinosProperties *properties);
};

/* normal GObject stuff */
GType               pinos_node_factory_get_type         (void);

PinosNode *         pinos_node_factory_create_node      (PinosNodeFactory *factory,
                                                         PinosClient *client,
                                                         const gchar *name,
                                                         PinosProperties *props);

const gchar *       pinos_node_factory_get_name         (PinosNodeFactory *node_factory);

G_END_DECLS

#endif /* __PINOS_NODE_FACTORY_H__ */
