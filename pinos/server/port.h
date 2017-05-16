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

#ifndef __PINOS_PORT_H__
#define __PINOS_PORT_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PINOS_TYPE__Port                          "Pinos:Object:Port"
#define PINOS_TYPE_PORT_BASE                      PINOS_TYPE__Port ":"

typedef struct _PinosPort PinosPort;

#include <spa/node.h>

#include <pinos/client/introspect.h>
#include <pinos/client/mem.h>

#include <pinos/server/core.h>
#include <pinos/server/link.h>

typedef enum {
  PINOS_PORT_STATE_ERROR         = -1,
  PINOS_PORT_STATE_INIT          =  0,
  PINOS_PORT_STATE_CONFIGURE     =  1,
  PINOS_PORT_STATE_READY         =  2,
  PINOS_PORT_STATE_PAUSED        =  3,
  PINOS_PORT_STATE_STREAMING     =  4,
} PinosPortState;

struct _PinosPort {
  SpaList        link;

  PINOS_SIGNAL   (destroy_signal, (PinosListener *listener, PinosPort *));

  PinosNode      *node;
  PinosDirection  direction;
  uint32_t        port_id;
  PinosPortState  state;
  SpaPortIO       io;

  bool            allocated;
  PinosMemblock   buffer_mem;
  SpaBuffer     **buffers;
  uint32_t        n_buffers;

  SpaList         links;

  struct {
    SpaList         links;
  } rt;
};

PinosPort *         pinos_port_new                     (PinosNode       *node,
                                                        PinosDirection   direction,
                                                        uint32_t         port_id);
void                pinos_port_destroy                 (PinosPort       *port);


PinosLink *         pinos_port_link                    (PinosPort        *output_port,
                                                        PinosPort        *input_port,
                                                        SpaFormat       **format_filter,
                                                        PinosProperties  *properties,
                                                        char            **error);
SpaResult           pinos_port_unlink                  (PinosPort        *port,
                                                        PinosLink        *link);

SpaResult           pinos_port_pause_rt                (PinosPort        *port);
SpaResult           pinos_port_clear_buffers           (PinosPort        *port);


#ifdef __cplusplus
}
#endif

#endif /* __PINOS_PORT_H__ */
