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

#include <pinos/server/core.h>
#include <pinos/server/resource.h>

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

  PinosMap objects;

  PinosSendFunc   send_func;
  void           *send_data;

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosClient *client));
};

PinosClient *   pinos_client_new                  (PinosCore       *core,
                                                   PinosProperties *properties);
void            pinos_client_destroy              (PinosClient     *client);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_CLIENT_H__ */
