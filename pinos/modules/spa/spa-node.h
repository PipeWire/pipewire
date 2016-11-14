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

#ifndef __PINOS_SPA_NODE_H__
#define __PINOS_SPA_NODE_H__

#include <server/core.h>
#include <server/node.h>

#ifdef __cplusplus
extern "C" {
#endif
typedef struct _PinosSpaNode PinosSpaNode;

struct _PinosSpaNode {
  PinosNode *node;

  char *lib;
  char *factory_name;
  SpaHandle  *handle;
  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosSpaNode  *node));
};

PinosSpaNode *    pinos_spa_node_load      (PinosCore       *core,
                                            const char      *lib,
                                            const char      *factory_name,
                                            const char      *name,
                                            PinosProperties *properties,
                                            const char      *args);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_SPA_NODE_H__ */
