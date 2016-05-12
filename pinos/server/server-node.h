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

#ifndef __PINOS_SERVER_NODE_H__
#define __PINOS_SERVER_NODE_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosServerNode PinosServerNode;
typedef struct _PinosServerNodeClass PinosServerNodeClass;
typedef struct _PinosServerNodePrivate PinosServerNodePrivate;

#include <pinos/client/introspect.h>
#include <pinos/server/daemon.h>
#include <pinos/server/server-port.h>

#define PINOS_TYPE_SERVER_NODE                 (pinos_server_node_get_type ())
#define PINOS_IS_SERVER_NODE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_SERVER_NODE))
#define PINOS_IS_SERVER_NODE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_SERVER_NODE))
#define PINOS_SERVER_NODE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_SERVER_NODE, PinosServerNodeClass))
#define PINOS_SERVER_NODE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_SERVER_NODE, PinosServerNode))
#define PINOS_SERVER_NODE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_SERVER_NODE, PinosServerNodeClass))
#define PINOS_SERVER_NODE_CAST(obj)            ((PinosServerNode*)(obj))
#define PINOS_SERVER_NODE_CLASS_CAST(klass)    ((PinosServerNodeClass*)(klass))

/**
 * PinosServerServerNode:
 *
 * Pinos node class.
 */
struct _PinosServerNode {
  PinosNode object;

  PinosServerNodePrivate *priv;
};

/**
 * PinosServerNodeClass:
 * @set_state: called to change the current state of the node
 *
 * Pinos node class.
 */
struct _PinosServerNodeClass {
  PinosNodeClass parent_class;
};

/* normal GObject stuff */
GType               pinos_server_node_get_type                (void);

PinosNode *         pinos_server_node_new                     (PinosDaemon     *daemon,
                                                               const gchar     *sender,
                                                               const gchar     *name,
                                                               PinosProperties *properties);

PinosDaemon *       pinos_server_node_get_daemon              (PinosServerNode *node);
const gchar *       pinos_server_node_get_sender              (PinosServerNode *node);
const gchar *       pinos_server_node_get_object_path         (PinosServerNode *node);

G_END_DECLS

#endif /* __PINOS_SERVER_NODE_H__ */
