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

#ifndef __PINOS_CLIENT_H__
#define __PINOS_CLIENT_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PINOS_CLIENT_URI                            "http://pinos.org/ns/client"
#define PINOS_CLIENT_PREFIX                         PINOS_CLIENT_URI "#"

typedef struct _PinosClient PinosClient;
typedef struct _PinosResource PinosResource;

typedef void  (*PinosDestroy)  (void *object);

#include <pinos/server/core.h>

struct _PinosResource {
  PinosCore    *core;
  SpaList       link;

  PinosClient  *client;

  uint32_t      id;
  uint32_t      type;
  void         *object;
  PinosDestroy  destroy;

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosResource *resource));
};

/**
 * PinosClient:
 *
 * Pinos client object class.
 */
struct _PinosClient {
  PinosCore   *core;
  SpaList      link;
  PinosGlobal *global;

  PinosProperties *properties;

  SpaList resource_list;

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosClient *client));
};

PinosClient *   pinos_client_new                  (PinosCore       *core,
                                                   PinosProperties *properties);
SpaResult       pinos_client_destroy              (PinosClient     *client);


PinosResource * pinos_client_add_resource         (PinosClient  *client,
                                                   uint32_t      type,
                                                   void         *object,
                                                   PinosDestroy  destroy);

SpaResult       pinos_resource_destroy            (PinosResource *resource);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_CLIENT_H__ */
