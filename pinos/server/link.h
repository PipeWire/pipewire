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

#ifndef __PINOS_LINK_H__
#define __PINOS_LINK_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PinosLink PinosLink;

#include <spa/include/spa/ringbuffer.h>

#include <pinos/client/mem.h>

#include <pinos/server/core.h>
#include <pinos/server/port.h>
#include <pinos/server/main-loop.h>

/**
 * PinosLink:
 *
 * Pinos link interface.
 */
struct _PinosLink {
  PinosCore   *core;
  SpaList      link;
  PinosGlobal *global;

  PinosProperties *properties;

  PinosLinkState state;
  char *error;
  PINOS_SIGNAL (state_changed,  (PinosListener  *listener,
                                 PinosLink      *link,
                                 PinosLinkState  old,
                                 PinosLinkState  state));

  PINOS_SIGNAL (destroy_signal, (PinosListener *,
                                 PinosLink *));
  PINOS_SIGNAL (free_signal,    (PinosListener *,
                                 PinosLink *));

  SpaList resource_list;

  PinosPort    *output;
  SpaList       output_link;
  PinosPort    *input;
  SpaList       input_link;
  PINOS_SIGNAL (port_unlinked, (PinosListener *listener,
                                PinosLink     *link,
                                PinosPort     *port));

  struct {
    uint32_t       in_ready;
    PinosPort     *input;
    PinosPort     *output;
    SpaList        input_link;
    SpaList        output_link;
  } rt;
};


PinosLink *     pinos_link_new          (PinosCore       *core,
                                         PinosPort       *output,
                                         PinosPort       *input,
                                         SpaFormat      **format_filter,
                                         PinosProperties *properties);
void            pinos_link_destroy      (PinosLink       *link);

bool            pinos_link_activate     (PinosLink *link);
bool            pinos_link_deactivate   (PinosLink *link);

#ifdef __cplusplus
}
#endif


#endif /* __PINOS_LINK_H__ */
