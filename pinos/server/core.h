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

#ifndef __PINOS_CORE_H__
#define __PINOS_CORE_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PinosCore PinosCore;
typedef struct _PinosGlobal PinosGlobal;

#define PINOS_CORE_URI                            "http://pinos.org/ns/core"
#define PINOS_CORE_PREFIX                         PINOS_CORE_URI "#"

#include <spa/include/spa/log.h>

#include <pinos/server/main-loop.h>
#include <pinos/server/data-loop.h>
#include <pinos/server/uri.h>
#include <pinos/server/node.h>
#include <pinos/server/link.h>
#include <pinos/server/node-factory.h>

struct _PinosGlobal {
  PinosCore *core;
  SpaList    link;
  uint32_t   id;
  uint32_t   type;
  void      *object;

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosGlobal   *global));
};

/**
 * PinosCore:
 *
 * Pinos core object class.
 */
struct _PinosCore {
  PinosGlobal *global;

  PinosURI uri;

  PinosMap objects;

  SpaList global_list;
  SpaList client_list;
  SpaList node_list;
  SpaList node_factory_list;
  SpaList link_list;

  PinosMainLoop *main_loop;
  PinosDataLoop *data_loop;

  SpaSupport *support;
  unsigned int n_support;

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosCore     *core));

  PINOS_SIGNAL (global_added,  (PinosListener *listener,
                                PinosCore     *core,
                                PinosGlobal   *global));
  PINOS_SIGNAL (global_removed, (PinosListener *listener,
                                 PinosCore     *core,
                                 PinosGlobal   *global));

  PINOS_SIGNAL (node_state_request, (PinosListener  *listener,
                                     PinosNode      *object,
                                     PinosNodeState  state));
  PINOS_SIGNAL (node_state_changed, (PinosListener  *listener,
                                     PinosNode      *object,
                                     PinosNodeState  old,
                                     PinosNodeState  state));
  PINOS_SIGNAL (port_added, (PinosListener *listener,
                             PinosNode     *node,
                             PinosPort     *port));
  PINOS_SIGNAL (port_removed, (PinosListener *listener,
                               PinosNode     *node,
                               PinosPort     *port));

  PINOS_SIGNAL (port_unlinked, (PinosListener *listener,
                                PinosLink     *link,
                                PinosPort     *port));
  PINOS_SIGNAL (link_state_changed,  (PinosListener *listener,
                                      PinosLink     *link));
  PINOS_SIGNAL (node_unlink,       (PinosListener *listener,
                                    PinosNode     *node));
  PINOS_SIGNAL (node_unlink_done,  (PinosListener *listener,
                                    PinosNode     *node));
};

PinosCore *     pinos_core_new           (PinosMainLoop *main_loop);
void            pinos_core_destroy       (PinosCore     *core);

PinosGlobal *   pinos_core_add_global    (PinosCore           *core,
                                          uint32_t             type,
                                          void                *object);
void            pinos_global_destroy     (PinosGlobal         *global);


PinosPort *     pinos_core_find_port     (PinosCore        *core,
                                          PinosPort        *other_port,
                                          uint32_t          id,
                                          PinosProperties  *props,
                                          SpaFormat       **format_filter,
                                          char            **error);

PinosNodeFactory * pinos_core_find_node_factory (PinosCore  *core,
                                                 const char *name);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_CORE_H__ */
