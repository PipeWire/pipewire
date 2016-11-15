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

#include <string.h>

#include "pinos/client/pinos.h"
#include "pinos/server/core.h"
#include "pinos/server/daemon.h"
#include "pinos/server/registry.h"
#include "pinos/server/node.h"
#include "pinos/server/node-factory.h"
#include "pinos/server/client.h"
#include "pinos/server/client-node.h"

#include "spa/include/spa/monitor.h"

void
pinos_registry_init (PinosRegistry *reg)
{
  reg->map = pinos_id_map_get_default();

  reg->uri.daemon = spa_id_map_get_id (reg->map, PINOS_DAEMON_URI);
  reg->uri.registry = spa_id_map_get_id (reg->map, PINOS_REGISTRY_URI);
  reg->uri.node = spa_id_map_get_id (reg->map, PINOS_NODE_URI);
  reg->uri.node_factory = spa_id_map_get_id (reg->map, PINOS_NODE_FACTORY_URI);
  reg->uri.link = spa_id_map_get_id (reg->map, PINOS_LINK_URI);
  reg->uri.client = spa_id_map_get_id (reg->map, PINOS_CLIENT_URI);
  reg->uri.client_node = spa_id_map_get_id (reg->map, PINOS_CLIENT_NODE_URI);

  reg->uri.spa_node = spa_id_map_get_id (reg->map, SPA_NODE_URI);
  reg->uri.spa_clock = spa_id_map_get_id (reg->map, SPA_CLOCK_URI);
  reg->uri.spa_monitor = spa_id_map_get_id (reg->map, SPA_MONITOR_URI);

  pinos_map_init (&reg->objects, 512);
}

PinosObject *
pinos_registry_iterate_objects (PinosRegistry *reg,
                                uint32_t       type,
                                void         **state)
{
  unsigned int idx;
  PinosObject *o;

  while (true) {
    idx = SPA_PTR_TO_INT (*state);
    *state = SPA_INT_TO_PTR (idx+1);
    o = pinos_map_lookup (&reg->objects, idx);
    if (o != NULL)
      break;
  }
  return o;
}
