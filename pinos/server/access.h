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

#define PINOS_TYPE__Access                         "Pinos:Object:Access"
#define PINOS_TYPE_ACCESS_BASE                      PINOS_TYPE__Access ":"

#include <pinos/client/sig.h>

typedef struct _PinosAccess PinosAccess;
typedef struct _PinosAccessData PinosAccessData;

#include <pinos/server/client.h>
#include <pinos/server/resource.h>

struct _PinosAccessData {
  SpaResult      res;
  PinosResource *resource;
  void          *(*async_copy)  (PinosAccessData *data, size_t size);
  void           (*complete_cb) (PinosAccessData *data);
  void           (*free_cb)     (PinosAccessData *data);
  void          *user_data;
};


/**
 * PinosAccess:
 *
 * Pinos Access support struct.
 */
struct _PinosAccess {
  SpaResult  (*view_global)            (PinosAccess      *access,
                                        PinosClient      *client,
                                        PinosGlobal      *global);
  SpaResult  (*create_node)            (PinosAccess      *access,
                                        PinosAccessData  *data,
                                        const char       *factory_name,
                                        const char       *name,
                                        PinosProperties  *properties);
  SpaResult  (*create_client_node)     (PinosAccess      *access,
                                        PinosAccessData  *data,
                                        const char       *name,
                                        PinosProperties  *properties);
};

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_ACCESS_H__ */
