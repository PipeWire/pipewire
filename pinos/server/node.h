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

#ifndef __PINOS_NODE_H__
#define __PINOS_NODE_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosNode PinosNode;
typedef struct _PinosNodeClass PinosNodeClass;
typedef struct _PinosNodePrivate PinosNodePrivate;

#include <spa/include/spa/node.h>

#include <pinos/client/introspect.h>
#include <pinos/server/daemon.h>
#include <pinos/server/port.h>

#define PINOS_TYPE_NODE                 (pinos_node_get_type ())
#define PINOS_IS_NODE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_NODE))
#define PINOS_IS_NODE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_NODE))
#define PINOS_NODE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_NODE, PinosNodeClass))
#define PINOS_NODE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_NODE, PinosNode))
#define PINOS_NODE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_NODE, PinosNodeClass))
#define PINOS_NODE_CAST(obj)            ((PinosNode*)(obj))
#define PINOS_NODE_CLASS_CAST(klass)    ((PinosNodeClass*)(klass))

/**
 * PinosNode:
 *
 * Pinos node class.
 */
struct _PinosNode {
  GObject object;

  SpaNode *node;
  SpaNodeState node_state;

  PinosNodePrivate *priv;
};

/**
 * PinosNodeClass:
 * @set_state: called to change the current state of the node
 *
 * Pinos node class.
 */
struct _PinosNodeClass {
  GObjectClass parent_class;

  gboolean    (*set_state)    (PinosNode       *node,
                               PinosNodeState   state);

  PinosPort * (*add_port)     (PinosNode       *node,
                               guint            id,
                               GError         **error);
  gboolean    (*remove_port)  (PinosNode       *node,
                               PinosPort       *port);
};

/* normal GObject stuff */
GType               pinos_node_get_type                (void);

PinosNode *         pinos_node_new                     (PinosDaemon     *daemon,
                                                        const gchar     *sender,
                                                        const gchar     *name,
                                                        PinosProperties *properties,
                                                        SpaNode         *node);
void                pinos_node_remove                  (PinosNode *node);

const gchar *       pinos_node_get_name                (PinosNode *node);
PinosProperties *   pinos_node_get_properties          (PinosNode *node);

PinosDaemon *       pinos_node_get_daemon              (PinosNode       *node);
const gchar *       pinos_node_get_sender              (PinosNode       *node);
const gchar *       pinos_node_get_object_path         (PinosNode       *node);

guint               pinos_node_get_free_port_id        (PinosNode       *node,
                                                        PinosDirection   direction);
PinosPort *         pinos_node_add_port                (PinosNode       *node,
                                                        guint            id,
                                                        GError         **error);
gboolean            pinos_node_remove_port             (PinosNode       *node,
                                                        PinosPort       *port);
PinosPort *         pinos_node_find_port_by_id         (PinosNode       *node,
                                                        guint            id);
GList *             pinos_node_get_ports               (PinosNode       *node);

PinosNodeState      pinos_node_get_state               (PinosNode *node);
gboolean            pinos_node_set_state               (PinosNode *node, PinosNodeState state);
void                pinos_node_update_state            (PinosNode *node, PinosNodeState state);

void                pinos_node_report_error            (PinosNode *node, GError *error);
void                pinos_node_report_idle             (PinosNode *node);
void                pinos_node_report_busy             (PinosNode *node);

void                pinos_node_update_node_state       (PinosNode *node, SpaNodeState state);

G_END_DECLS

#endif /* __PINOS_NODE_H__ */
