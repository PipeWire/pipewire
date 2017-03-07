/* Simple Plugin API
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PINOS_TRANSPORT_H__
#define __PINOS_TRANSPORT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PinosTransport PinosTransport;
typedef struct _PinosTransportArea PinosTransportArea;

#include <string.h>

#include <spa/defs.h>
#include <spa/port.h>
#include <spa/node.h>

#include <pinos/client/mem.h>
#include <pinos/client/sig.h>

typedef struct {
  int      memfd;
  uint32_t offset;
  uint32_t size;
} PinosTransportInfo;

/**
 * PinosTransportArea:
 *
 * Shared structure between client and server
 */
struct _PinosTransportArea {
  uint32_t   max_inputs;
  uint32_t   n_inputs;
  uint32_t   max_outputs;
  uint32_t   n_outputs;
};

struct _PinosTransport {
  PINOS_SIGNAL (destroy_signal, (PinosListener  *listener,
                                 PinosTransport *trans));

  PinosTransportArea *area;
  SpaPortInput       *inputs;
  SpaPortOutput      *outputs;
  void               *input_data;
  SpaRingbuffer      *input_buffer;
  void               *output_data;
  SpaRingbuffer      *output_buffer;
};

PinosTransport * pinos_transport_new            (uint32_t max_inputs,
                                                 uint32_t max_outputs);
PinosTransport * pinos_transport_new_from_info  (PinosTransportInfo *info);

void             pinos_transport_destroy        (PinosTransport     *trans);

SpaResult        pinos_transport_get_info       (PinosTransport     *trans,
                                                 PinosTransportInfo *info);

SpaResult        pinos_transport_add_event      (PinosTransport *trans,
                                                 SpaNodeEvent   *event);

SpaResult        pinos_transport_next_event     (PinosTransport *trans,
                                                 SpaNodeEvent   *event);
SpaResult        pinos_transport_parse_event    (PinosTransport *trans,
                                                 void           *event);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __PINOS_TRANSPORT_H__ */
