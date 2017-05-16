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

#include <spa/log.h>

#include <pinos/client/type.h>

#include <pinos/server/access.h>
#include <pinos/server/main-loop.h>
#include <pinos/server/data-loop.h>
#include <pinos/server/node.h>
#include <pinos/server/link.h>
#include <pinos/server/node-factory.h>

typedef SpaResult (*PinosBindFunc)  (PinosGlobal   *global,
                                     PinosClient   *client,
                                     uint32_t       version,
                                     uint32_t       id);

struct _PinosGlobal {
  PinosCore   *core;
  PinosClient *owner;
  SpaList      link;
  uint32_t     id;
  uint32_t     type;
  uint32_t     version;
  void        *object;

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

  PinosCoreInfo info;

  PinosProperties *properties;

  PinosType type;
  PinosAccess *access;

  PinosMap objects;

  SpaList resource_list;
  SpaList registry_resource_list;
  SpaList global_list;
  SpaList client_list;
  SpaList node_list;
  SpaList node_factory_list;
  SpaList link_list;

  PinosMainLoop *main_loop;
  PinosDataLoop *data_loop;

  SpaSupport *support;
  uint32_t    n_support;

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosCore     *core));

  PINOS_SIGNAL (global_added,  (PinosListener *listener,
                                PinosCore     *core,
                                PinosGlobal   *global));
  PINOS_SIGNAL (global_removed, (PinosListener *listener,
                                 PinosCore     *core,
                                 PinosGlobal   *global));
};

PinosCore *     pinos_core_new           (PinosMainLoop   *main_loop,
                                          PinosProperties *props);
void            pinos_core_destroy       (PinosCore       *core);

void            pinos_core_update_properties (PinosCore     *core,
                                              const SpaDict *dict);

bool            pinos_core_add_global    (PinosCore     *core,
                                          PinosClient   *owner,
                                          uint32_t       type,
                                          uint32_t       version,
                                          void          *object,
                                          PinosBindFunc  bind,
                                          PinosGlobal  **global);

SpaResult       pinos_global_bind        (PinosGlobal   *global,
                                          PinosClient   *client,
                                          uint32_t       version,
                                          uint32_t       id);
void            pinos_global_destroy     (PinosGlobal   *global);

SpaFormat *     pinos_core_find_format   (PinosCore        *core,
                                          PinosPort        *output,
                                          PinosPort        *input,
                                          PinosProperties  *props,
                                          uint32_t          n_format_filters,
                                          SpaFormat       **format_filters,
                                          char            **error);

PinosPort *     pinos_core_find_port     (PinosCore        *core,
                                          PinosPort        *other_port,
                                          uint32_t          id,
                                          PinosProperties  *props,
                                          uint32_t          n_format_filters,
                                          SpaFormat       **format_filters,
                                          char            **error);

PinosNodeFactory * pinos_core_find_node_factory (PinosCore  *core,
                                                 const char *name);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_CORE_H__ */
