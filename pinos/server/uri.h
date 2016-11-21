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

#ifndef __PINOS_URI_H__
#define __PINOS_URI_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PINOS_URI_URI                            "http://pinos.org/ns/uri"
#define PINOS_URI_PREFIX                         PINOS_URI_URI "#"

#include <pinos/client/map.h>
#include <spa/include/spa/id-map.h>

typedef struct _PinosURI PinosURI;

/**
 * PinosURI:
 *
 * Pinos URI support struct.
 */
struct _PinosURI {
  SpaIDMap *map;

  uint32_t node;
  uint32_t node_factory;
  uint32_t link;
  uint32_t client;
  uint32_t client_node;

  uint32_t spa_node;
  uint32_t spa_clock;
  uint32_t spa_monitor;
};

void pinos_uri_init (PinosURI *uri);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_URI_H__ */
