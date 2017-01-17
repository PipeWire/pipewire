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

typedef struct _PinosClient PinosClient;

#include <sys/socket.h>

#include <pinos/client/properties.h>
#include <pinos/client/sig.h>

#include <pinos/server/core.h>
#include <pinos/server/resource.h>

typedef SpaResult (*PinosSendFunc) (void             *object,
                                    uint32_t          id,
                                    uint32_t          opcode,
                                    void             *message,
                                    bool              flush,
                                    void             *data);

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
  bool             ucred_valid;
  struct ucred     ucred;

  PinosResource *core_resource;

  PinosMap objects;

  SpaList resource_list;
  PINOS_SIGNAL (resource_added,   (PinosListener *listener,
                                   PinosClient   *client,
                                   PinosResource *resource));
  PINOS_SIGNAL (resource_removed, (PinosListener *listener,
                                   PinosClient   *client,
                                   PinosResource *resource));

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosClient   *client));
};

PinosClient *   pinos_client_new                  (PinosCore       *core,
                                                   struct ucred    *ucred,
                                                   PinosProperties *properties);
void            pinos_client_destroy              (PinosClient     *client);

void            pinos_client_set_send             (PinosClient     *client,
                                                   PinosSendFunc    func,
                                                   void            *data);

SpaResult       pinos_client_send_message         (PinosClient     *client,
                                                   PinosResource   *resource,
                                                   uint32_t         opcode,
                                                   void            *message,
                                                   bool             flush);

SpaResult       pinos_client_send_error           (PinosClient     *client,
                                                   PinosResource   *resource,
                                                   SpaResult        res,
                                                   const char      *message, ...);

void            pinos_client_update_properties    (PinosClient     *client,
                                                   const SpaDict   *dict);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_CLIENT_H__ */
