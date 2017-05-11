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
  type->spa_format = spa_type_map_get_id (type->map, SPA_TYPE__Format);
  type->spa_props = spa_type_map_get_id (type->map, SPA_TYPE__Props);

  spa_type_meta_map (type->map, &type->meta);
  spa_type_data_map (type->map, &type->data);
  spa_type_event_node_map (type->map, &type->event_node);
  spa_type_command_node_map (type->map, &type->command_node);
  spa_type_monitor_map (type->map, &type->monitor);
  spa_type_alloc_param_buffers_map (type->map, &type->alloc_param_buffers);
  spa_type_alloc_param_meta_enable_map (type->map, &type->alloc_param_meta_enable);
  spa_type_alloc_param_video_padding_map (type->map, &type->alloc_param_video_padding);

  pinos_type_event_transport_map (type->map, &type->event_transport);
}

bool
pinos_pod_remap_data (uint32_t type, void * body, uint32_t size, PinosMap *types)
{
  void *t;
  switch (type) {
    case SPA_POD_TYPE_ID:
      if ((t = pinos_map_lookup (types, *(int32_t*)body)) == NULL)
        return false;
      *(int32_t*)body = PINOS_MAP_PTR_TO_ID (t);
      break;

    case SPA_POD_TYPE_PROP:
    {
      SpaPODPropBody *b = body;

      if ((t = pinos_map_lookup (types, b->key)) == NULL)
        return false;
      b->key = PINOS_MAP_PTR_TO_ID (t);

      if (b->value.type == SPA_POD_TYPE_ID) {
        void *alt;
        if (!pinos_pod_remap_data (b->value.type, SPA_POD_BODY (&b->value), b->value.size, types))
          return false;

        SPA_POD_PROP_ALTERNATIVE_FOREACH (b, size, alt)
          if (!pinos_pod_remap_data (b->value.type, alt, b->value.size, types))
            return false;
      }
      break;
    }
    case SPA_POD_TYPE_OBJECT:
    {
      SpaPODObjectBody *b = body;
      SpaPOD *p;

      if ((t = pinos_map_lookup (types, b->type)) == NULL)
        return false;
      b->type = PINOS_MAP_PTR_TO_ID (t);

      SPA_POD_OBJECT_BODY_FOREACH (b, size, p)
        if (!pinos_pod_remap_data (p->type, SPA_POD_BODY (p), p->size, types))
          return false;
      break;
    }
    case SPA_POD_TYPE_STRUCT:
    {
      SpaPOD *b = body, *p;

      SPA_POD_FOREACH (b, size, p)
        if (!pinos_pod_remap_data (p->type, SPA_POD_BODY (p), p->size, types))
          return false;
      break;
    }
    default:
      break;
  }
  return true;
}
