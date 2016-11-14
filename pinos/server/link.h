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

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosLink PinosLink;

#define PINOS_LINK_URI                            "http://pinos.org/ns/link"
#define PINOS_LINK_PREFIX                         PINOS_LINK_URI "#"

#include <spa/include/spa/ringbuffer.h>

#include <pinos/client/mem.h>
#include <pinos/client/object.h>

#include <pinos/server/daemon.h>
#include <pinos/server/main-loop.h>

/**
 * PinosLink:
 *
 * Pinos link interface.
 */
struct _PinosLink {
  PinosCore   *core;
  SpaList      list;
  PinosGlobal *global;

  PinosProperties *properties;

  PinosLinkState state;
  GError *error;

  PINOS_SIGNAL (destroy_signal, (PinosListener *,
                                 PinosLink *));

  PinosPort    *output;
  PinosPort    *input;

  uint32_t      queue[64];
  SpaRingbuffer ringbuffer;
  gint          in_ready;
};


PinosLink *     pinos_link_new          (PinosCore       *core,
                                         PinosPort       *output,
                                         PinosPort       *input,
                                         GPtrArray       *format_filter,
                                         PinosProperties *properties);
void            pinos_link_destroy      (PinosLink       *link);

bool            pinos_link_activate     (PinosLink *link);
bool            pinos_link_deactivate   (PinosLink *link);



const gchar *       pinos_link_get_object_path      (PinosLink *link);

G_END_DECLS

#endif /* __PINOS_LINK_H__ */
