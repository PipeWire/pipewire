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
#include "pinos/client/type.h"

#include "pinos/server/core.h"
#include "pinos/server/node.h"
#include "pinos/server/node-factory.h"
#include "pinos/server/client.h"
#include "pinos/server/client-node.h"
#include "pinos/server/module.h"

#include "spa/include/spa/monitor.h"

void
pinos_type_init (PinosType *type)
{
  type->map = pinos_type_map_get_default();

  type->core = spa_type_map_get_id (type->map, PINOS_TYPE__Core);
  type->registry = spa_type_map_get_id (type->map, PINOS_TYPE__Registry);
  type->node = spa_type_map_get_id (type->map, PINOS_TYPE__Node);
  type->node_factory = spa_type_map_get_id (type->map, PINOS_TYPE__NodeFactory);
  type->link = spa_type_map_get_id (type->map, PINOS_TYPE__Link);
  type->client = spa_type_map_get_id (type->map, PINOS_TYPE__Client);
  type->client_node = spa_type_map_get_id (type->map, PINOS_TYPE__ClientNode);
  type->module = spa_type_map_get_id (type->map, PINOS_TYPE__Module);

  type->spa_node = spa_type_map_get_id (type->map, SPA_TYPE__Node);
  type->spa_clock = spa_type_map_get_id (type->map, SPA_TYPE__Clock);
  type->spa_monitor = spa_type_map_get_id (type->map, SPA_TYPE__Monitor);

  spa_type_event_node_map (type->map, &type->event_node);
  spa_type_command_node_map (type->map, &type->command_node);
  spa_type_monitor_map (type->map, &type->monitor);
  spa_type_alloc_param_buffers_map (type->map, &type->alloc_param_buffers);
  spa_type_alloc_param_meta_enable_map (type->map, &type->alloc_param_meta_enable);
  spa_type_alloc_param_video_padding_map (type->map, &type->alloc_param_video_padding);
}
