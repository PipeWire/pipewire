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

#ifdef __cplusplus
extern "C" {
#endif

#define PINOS_TYPE__Node                          "Pinos:Object:Node"
#define PINOS_TYPE_NODE_BASE                      PINOS_TYPE__Node ":"

typedef struct _PinosNode PinosNode;

#include <spa/clock.h>
#include <spa/node.h>

#include <pinos/client/mem.h>
#include <pinos/client/transport.h>

#include <pinos/server/core.h>
#include <pinos/server/port.h>
#include <pinos/server/link.h>
#include <pinos/server/client.h>
#include <pinos/server/data-loop.h>

/**
 * PinosNode:
 *
 * Pinos node class.
 */
struct _PinosNode {
  PinosCore   *core;
  SpaList      link;
  PinosGlobal *global;

  PinosClient *owner;
  char *name;
  PinosProperties *properties;
  PinosNodeState state;
  char *error;
  PINOS_SIGNAL (state_request, (PinosListener  *listener,
                                PinosNode      *object,
                                PinosNodeState  state));
  PINOS_SIGNAL (state_changed, (PinosListener  *listener,
                                PinosNode      *object,
                                PinosNodeState  old,
                                PinosNodeState  state));

  SpaHandle *handle;
  SpaNode *node;
  bool live;
  SpaClock *clock;

  SpaList resource_list;

  PINOS_SIGNAL (initialized, (PinosListener *listener,
                              PinosNode     *object));

  uint32_t    max_input_ports;
  uint32_t    n_input_ports;
  SpaList     input_ports;
  PinosPort **input_port_map;
  uint32_t    n_used_input_links;

  uint32_t    max_output_ports;
  uint32_t    n_output_ports;
  SpaList     output_ports;
  PinosPort **output_port_map;
  uint32_t    n_used_output_links;

  PINOS_SIGNAL (port_added, (PinosListener *listener,
                             PinosNode     *node,
                             PinosPort     *port));
  PINOS_SIGNAL (port_removed, (PinosListener *listener,
                               PinosNode     *node,
                               PinosPort     *port));

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosNode     *object));
  PINOS_SIGNAL (free_signal,    (PinosListener *listener,
                                 PinosNode     *object));

  PINOS_SIGNAL (async_complete, (PinosListener *listener,
                                 PinosNode     *node,
                                 uint32_t       seq,
                                 SpaResult      res));

  PinosDataLoop *data_loop;
  PINOS_SIGNAL (loop_changed, (PinosListener *listener,
                               PinosNode     *object));

};

PinosNode *         pinos_node_new                     (PinosCore       *core,
                                                        PinosClient     *owner,
                                                        const char      *name,
                                                        bool             async,
                                                        SpaNode         *node,
                                                        SpaClock        *clock,
                                                        PinosProperties *properties);
void                pinos_node_destroy                 (PinosNode       *node);


void                pinos_node_set_data_loop           (PinosNode        *node,
                                                        PinosDataLoop    *loop);

PinosPort *         pinos_node_get_free_port           (PinosNode        *node,
                                                        PinosDirection    direction);

SpaResult           pinos_node_set_state               (PinosNode        *node,
                                                        PinosNodeState    state);
void                pinos_node_update_state            (PinosNode        *node,
                                                        PinosNodeState    state,
                                                        char             *error);

#ifdef __cplusplus
}
#endif


#endif /* __PINOS_NODE_H__ */
