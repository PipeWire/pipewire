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

typedef struct _PinosPort PinosPort;
typedef struct _PinosNode PinosNode;
typedef struct _PinosNodeClass PinosNodeClass;
typedef struct _PinosNodePrivate PinosNodePrivate;


typedef enum {
  PINOS_NODE_FLAG_NONE          = 0,
  PINOS_NODE_FLAG_REMOVING      = (1 << 0),
} PinosNodeFlags;

#include <spa/include/spa/node.h>

#include <pinos/client/introspect.h>
#include <pinos/client/mem.h>

#include <pinos/server/daemon.h>
#include <pinos/server/link.h>
#include <pinos/server/client.h>

struct _PinosPort {
  uint32_t        id;
  PinosNode      *node;
  PinosDirection  direction;
  uint32_t        port;
  gboolean        allocated;
  PinosMemblock   buffer_mem;
  SpaBuffer     **buffers;
  guint           n_buffers;
  GPtrArray      *links;
};

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

  uint32_t id;

  PinosNodeFlags flags;

  SpaNode *node;

  bool live;
  SpaClock *clock;

  gboolean have_inputs;
  gboolean have_outputs;

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
};

/* normal GObject stuff */
GType               pinos_node_get_type                (void);

void                pinos_node_remove                  (PinosNode *node);

const gchar *       pinos_node_get_name                (PinosNode *node);
PinosProperties *   pinos_node_get_properties          (PinosNode *node);

PinosDaemon *       pinos_node_get_daemon              (PinosNode        *node);
PinosClient *       pinos_node_get_client              (PinosNode        *node);
const gchar *       pinos_node_get_object_path         (PinosNode        *node);

PinosPort *         pinos_node_get_free_port           (PinosNode        *node,
                                                        PinosDirection    direction);

GList *             pinos_node_get_ports               (PinosNode        *node,
                                                        PinosDirection    direction);

PinosNodeState      pinos_node_get_state               (PinosNode *node);
gboolean            pinos_node_set_state               (PinosNode *node, PinosNodeState state);
void                pinos_node_update_state            (PinosNode *node, PinosNodeState state);

void                pinos_node_report_error            (PinosNode *node, GError *error);
void                pinos_node_report_idle             (PinosNode *node);
void                pinos_node_report_busy             (PinosNode *node);

PinosLink *         pinos_port_link                    (PinosPort        *output_port,
                                                        PinosPort        *input_port,
                                                        GPtrArray        *format_filter,
                                                        PinosProperties  *properties,
                                                        GError          **error);
SpaResult           pinos_port_unlink                  (PinosPort        *port,
                                                        PinosLink        *link);

SpaResult           pinos_port_clear_buffers           (PinosPort        *port);


G_END_DECLS

#endif /* __PINOS_NODE_H__ */
