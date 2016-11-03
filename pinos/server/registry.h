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

#ifndef __PINOS_REGISTRY_H__
#define __PINOS_REGISTRY_H__

#include <glib-object.h>

G_BEGIN_DECLS

#include <pinos/client/map.h>
#include <spa/include/spa/id-map.h>

typedef struct _PinosRegistry PinosRegistry;

/**
 * PinosRegistry:
 *
 * Pinos registry struct.
 */
struct _PinosRegistry {
  SpaIDMap *map;

  PinosMap objects;

  PinosMap clients;
  PinosMap node_factories;
  PinosMap nodes;
  PinosMap ports;
  PinosMap links;
  PinosMap modules;
  PinosMap monitors;
  PinosMap devices;
};


void    pinos_registry_init     (PinosRegistry *reg);

G_END_DECLS

#endif /* __PINOS_REGISTRY_H__ */
