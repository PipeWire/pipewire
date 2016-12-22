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

#ifndef __PINOS_RESOURCE_H__
#define __PINOS_RESOURCE_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PINOS_RESOURCE_URI                            "http://pinos.org/ns/resource"
#define PINOS_RESOURCE_PREFIX                         PINOS_RESOURCE_URI "#"

typedef struct _PinosResource PinosResource;

#include <pinos/server/core.h>

typedef void  (*PinosDestroy)  (void *object);

typedef SpaResult (*PinosSendFunc) (void             *object,
                                    uint32_t          id,
                                    uint32_t          opcode,
                                    void             *message,
                                    bool              flush,
                                    void             *data);

typedef SpaResult (*PinosDispatchFunc) (void             *object,
                                        uint32_t          opcode,
                                        void             *message,
                                        void             *data);

struct _PinosResource {
  PinosCore    *core;
  SpaList       link;

  PinosClient  *client;

  uint32_t      id;
  uint32_t      type;
  void         *object;
  PinosDestroy  destroy;

  PinosSendFunc      send_func;
  void              *send_data;
  PinosDispatchFunc  dispatch_func;
  void              *dispatch_data;

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosResource *resource));
};

PinosResource * pinos_resource_new                (PinosClient  *client,
                                                   uint32_t      id,
                                                   uint32_t      type,
                                                   void         *object,
                                                   PinosDestroy  destroy);
SpaResult       pinos_resource_destroy            (PinosResource *resource);

SpaResult       pinos_resource_send_message       (PinosResource     *resource,
                                                   uint32_t           opcode,
                                                   void              *message,
                                                   bool               flush);
SpaResult       pinos_resource_send_error         (PinosResource     *resource,
                                                   SpaResult          res,
                                                   const char        *message, ...);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_RESOURCE_H__ */
