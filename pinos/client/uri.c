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
#include "pinos/client/uri.h"

#include "pinos/server/core.h"
#include "pinos/server/node.h"
#include "pinos/server/node-factory.h"
#include "pinos/server/client.h"
#include "pinos/server/client-node.h"
#include "pinos/server/module.h"

#include "spa/include/spa/monitor.h"

void
pinos_uri_init (PinosURI *uri)
{
  uri->map = pinos_id_map_get_default();

  uri->core = spa_id_map_get_id (uri->map, PINOS_CORE_URI);
  uri->registry = spa_id_map_get_id (uri->map, PINOS_CORE_REGISTRY);
  uri->node = spa_id_map_get_id (uri->map, PINOS_NODE_URI);
  uri->node_factory = spa_id_map_get_id (uri->map, PINOS_NODE_FACTORY_URI);
  uri->link = spa_id_map_get_id (uri->map, PINOS_LINK_URI);
  uri->client = spa_id_map_get_id (uri->map, PINOS_CLIENT_URI);
  uri->client_node = spa_id_map_get_id (uri->map, PINOS_CLIENT_NODE_URI);
  uri->module = spa_id_map_get_id (uri->map, PINOS_MODULE_URI);

  uri->spa_node = spa_id_map_get_id (uri->map, SPA_NODE_URI);
  uri->spa_clock = spa_id_map_get_id (uri->map, SPA_CLOCK_URI);
  uri->spa_monitor = spa_id_map_get_id (uri->map, SPA_MONITOR_URI);

  spa_node_events_map (uri->map, &uri->node_events);
  spa_node_commands_map (uri->map, &uri->node_commands);
  spa_monitor_types_map (uri->map, &uri->monitor_types);
}
