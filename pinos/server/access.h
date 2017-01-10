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

#ifndef __PINOS_ACCESS_H__
#define __PINOS_ACCESS_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PINOS_ACCESS_URI                            "http://pinos.org/ns/access"
#define PINOS_ACCESS_PREFIX                         PINOS_ACCESS_URI "#"

#include <pinos/client/sig.h>

typedef struct _PinosAccess PinosAccess;

#include <pinos/server/client.h>
#include <pinos/server/resource.h>

typedef struct {
  SpaResult         res;
  PinosClient      *client;
  PinosResource    *resource;
  uint32_t          opcode;
  void             *message;
  bool              flush;
} PinosAccessData;

typedef SpaResult (*PinosAccessFunc) (PinosAccessData *data);

/**
 * PinosAccess:
 *
 * Pinos Access support struct.
 */
struct _PinosAccess {
  PINOS_SIGNAL (check_send,     (PinosListener    *listener,
                                 PinosAccessFunc   func,
                                 PinosAccessData  *data));
  PINOS_SIGNAL (check_dispatch, (PinosListener    *listener,
                                 PinosAccessFunc   func,
                                 PinosAccessData  *data));
};

void pinos_access_init (PinosAccess *access);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_ACCESS_H__ */
