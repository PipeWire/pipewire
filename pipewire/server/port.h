/* PipeWire
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

#ifndef __PIPEWIRE_PORT_H__
#define __PIPEWIRE_PORT_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PIPEWIRE_TYPE__Port                          "PipeWire:Object:Port"
#define PIPEWIRE_TYPE_PORT_BASE                      PIPEWIRE_TYPE__Port ":"

#include <spa/node.h>

#include <pipewire/client/introspect.h>
#include <pipewire/client/mem.h>

#include <pipewire/server/core.h>
#include <pipewire/server/link.h>

enum pw_port_state {
  PW_PORT_STATE_ERROR         = -1,
  PW_PORT_STATE_INIT          =  0,
  PW_PORT_STATE_CONFIGURE     =  1,
  PW_PORT_STATE_READY         =  2,
  PW_PORT_STATE_PAUSED        =  3,
  PW_PORT_STATE_STREAMING     =  4,
};

struct pw_port {
  SpaList        link;

  PW_SIGNAL   (destroy_signal, (struct pw_listener *listener, struct pw_port *));

  struct pw_node     *node;
  enum pw_direction   direction;
  uint32_t            port_id;
  enum pw_port_state  state;
  SpaPortIO           io;

  bool                 allocated;
  struct pw_memblock   buffer_mem;
  SpaBuffer          **buffers;
  uint32_t             n_buffers;

  SpaList           links;

  struct {
    SpaList         links;
  } rt;
};

struct pw_port *    pw_port_new                     (struct pw_node        *node,
                                                     enum pw_direction      direction,
                                                     uint32_t               port_id);
void                pw_port_destroy                 (struct pw_port        *port);


struct pw_link *    pw_port_link                    (struct pw_port        *output_port,
                                                     struct pw_port        *input_port,
                                                     SpaFormat            **format_filter,
                                                     struct pw_properties  *properties,
                                                     char                 **error);
SpaResult           pw_port_unlink                  (struct pw_port        *port,
                                                     struct pw_link        *link);

SpaResult           pw_port_pause_rt                (struct pw_port        *port);
SpaResult           pw_port_clear_buffers           (struct pw_port        *port);


#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_PORT_H__ */
