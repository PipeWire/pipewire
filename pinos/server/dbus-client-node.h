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

#ifndef __PINOS_DBUS_CLIENT_NODE_H__
#define __PINOS_DBUS_CLIENT_NODE_H__

#include <glib-object.h>

#include <pinos/server/node.h>

G_BEGIN_DECLS

#define PINOS_TYPE_DBUS_CLIENT_NODE                 (pinos_dbus_client_node_get_type ())
#define PINOS_IS_DBUS_CLIENT_NODE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_DBUS_CLIENT_NODE))
#define PINOS_IS_DBUS_CLIENT_NODE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_DBUS_CLIENT_NODE))
#define PINOS_DBUS_CLIENT_NODE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_DBUS_CLIENT_NODE, PinosDBusClientNodeClass))
#define PINOS_DBUS_CLIENT_NODE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_DBUS_CLIENT_NODE, PinosDBusClientNode))
#define PINOS_DBUS_CLIENT_NODE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_DBUS_CLIENT_NODE, PinosDBusClientNodeClass))
#define PINOS_DBUS_CLIENT_NODE_CAST(obj)            ((PinosDBusClientNode*)(obj))
#define PINOS_DBUS_CLIENT_NODE_CLASS_CAST(klass)    ((PinosDBusClientNodeClass*)(klass))

typedef struct _PinosDBusClientNode PinosDBusClientNode;
typedef struct _PinosDBusClientNodeClass PinosDBusClientNodeClass;
typedef struct _PinosDBusClientNodePrivate PinosDBusClientNodePrivate;

/**
 * PinosDBusClientNode:
 *
 * Pinos client node object class.
 */
struct _PinosDBusClientNode {
  PinosNode object;

  PinosDBusClientNodePrivate *priv;
};

/**
 * PinosDBusClientNodeClass:
 *
 * Pinos client node object class.
 */
struct _PinosDBusClientNodeClass {
  PinosNodeClass parent_class;
};

/* normal GObject stuff */
GType              pinos_dbus_client_node_get_type        (void);

PinosNode *        pinos_dbus_client_node_new             (PinosDaemon     *daemon,
                                                           PinosClient     *client,
                                                           const gchar     *path,
                                                           PinosProperties *properties);

GSocket *          pinos_dbus_client_node_get_socket_pair (PinosDBusClientNode  *node,
                                                           GError              **error);

G_END_DECLS

#endif /* __PINOS_DBUS_CLIENT_NODE_H__ */
