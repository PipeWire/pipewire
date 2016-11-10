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

#define PINOS_NODE_URI                            "http://pinos.org/ns/node"
#define PINOS_NODE_PREFIX                         PINOS_NODE_URI "#"

#define PINOS_PORT_URI                            PINOS_NODE_PREFIX "Port"

typedef struct _PinosPort PinosPort;
typedef struct _PinosNode PinosNode;
typedef struct _PinosNodeListener PinosNodeListener;

#include <spa/include/spa/node.h>

#include <pinos/client/introspect.h>
#include <pinos/client/mem.h>
#include <pinos/client/transport.h>

#include <pinos/server/daemon.h>
#include <pinos/server/link.h>
#include <pinos/server/client.h>

struct _PinosPort {
  PinosObject     object;

  PinosNode      *node;
  PinosDirection  direction;
  uint32_t        port;

  gboolean        allocated;
  PinosMemblock   buffer_mem;
  SpaBuffer     **buffers;
  guint           n_buffers;

  GPtrArray      *links;
};

typedef struct {
  uint32_t  seq;
  SpaResult res;
} PinosNodeAsyncCompleteData;

typedef struct {
  PinosNodeState old;
  PinosNodeState state;
} PinosNodeStateChangeData;

/**
 * PinosNode:
 *
 * Pinos node class.
 */
struct _PinosNode {
  char *name;
  PinosProperties *properties;
  PinosNodeState state;

  SpaNode *node;

  bool live;
  SpaClock *clock;

  gboolean have_inputs;
  gboolean have_outputs;

  PinosTransport *transport;
  PinosSignal transport_changed;

  PinosSignal state_change;
  PinosSignal port_added;
  PinosSignal port_removed;
  PinosSignal async_complete;

  PinosPort * (*get_free_port)   (PinosNode        *node,
                                  PinosDirection    direction);
  PinosPort * (*enum_ports)      (PinosNode        *node,
                                  PinosDirection    direction,
                                  void            **state);
  SpaResult   (*set_state)       (PinosNode        *node,
                                  PinosNodeState    state);
};

struct _PinosNodeListener {
  void  (*async_complete)  (PinosNodeListener *listener,
                            PinosNode         *node,
                            uint32_t           seq,
                            SpaResult          res);

  void  (*state_change)    (PinosNodeListener *listener,
                            PinosNode         *node,
                            PinosNodeState     old,
                            PinosNodeState     state);

  void  (*port_added)      (PinosNodeListener *listener,
                            PinosNode         *node,
                            PinosPort         *port);

  void  (*port_removed)    (PinosNodeListener *listener,
                            PinosNode         *node,
                            PinosPort         *port);
};



PinosObject *       pinos_node_new                     (PinosCore       *core,
                                                        const char      *name,
                                                        SpaNode         *node,
                                                        SpaClock        *clock,
                                                        PinosProperties *properties);

PinosDaemon *       pinos_node_get_daemon              (PinosNode        *node);
PinosClient *       pinos_node_get_client              (PinosNode        *node);
const gchar *       pinos_node_get_object_path         (PinosNode        *node);

PinosPort *         pinos_node_get_free_port           (PinosNode        *node,
                                                        PinosDirection    direction);

GList *             pinos_node_get_ports               (PinosNode        *node,
                                                        PinosDirection    direction);

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
