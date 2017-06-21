/* PipeWire
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

#include "spa/defs.h"
#include "spa/clock.h"
#include "spa/type-map.h"
#include "spa/monitor.h"

#include "pipewire/client/pipewire.h"
#include "pipewire/client/type.h"

#include "pipewire/server/node-factory.h"


/** Initializes the type system
 * \param type a type structure
 * \memberof pw_type
 */
void pw_type_init(struct pw_type *type)
{
	type->map = pw_get_support_interface(SPA_TYPE__TypeMap);

	type->core = spa_type_map_get_id(type->map, PIPEWIRE_TYPE__Core);
	type->registry = spa_type_map_get_id(type->map, PIPEWIRE_TYPE__Registry);
	type->node = spa_type_map_get_id(type->map, PIPEWIRE_TYPE__Node);
	type->node_factory = spa_type_map_get_id(type->map, PIPEWIRE_TYPE__NodeFactory);
	type->link = spa_type_map_get_id(type->map, PIPEWIRE_TYPE__Link);
	type->client = spa_type_map_get_id(type->map, PIPEWIRE_TYPE__Client);
	type->module = spa_type_map_get_id(type->map, PIPEWIRE_TYPE__Module);

	type->spa_log = spa_type_map_get_id(type->map, SPA_TYPE__Log);
	type->spa_node = spa_type_map_get_id(type->map, SPA_TYPE__Node);
	type->spa_clock = spa_type_map_get_id(type->map, SPA_TYPE__Clock);
	type->spa_monitor = spa_type_map_get_id(type->map, SPA_TYPE__Monitor);
	type->spa_format = spa_type_map_get_id(type->map, SPA_TYPE__Format);
	type->spa_props = spa_type_map_get_id(type->map, SPA_TYPE__Props);

	spa_type_meta_map(type->map, &type->meta);
	spa_type_data_map(type->map, &type->data);
	spa_type_event_node_map(type->map, &type->event_node);
	spa_type_command_node_map(type->map, &type->command_node);
	spa_type_monitor_map(type->map, &type->monitor);
	spa_type_param_alloc_buffers_map(type->map, &type->param_alloc_buffers);
	spa_type_param_alloc_meta_enable_map(type->map, &type->param_alloc_meta_enable);
	spa_type_param_alloc_video_padding_map(type->map, &type->param_alloc_video_padding);

	pw_type_event_transport_map(type->map, &type->event_transport);
}

bool pw_pod_remap_data(uint32_t type, void *body, uint32_t size, struct pw_map *types)
{
	void *t;
	switch (type) {
	case SPA_POD_TYPE_ID:
		if ((t = pw_map_lookup(types, *(int32_t *) body)) == NULL)
			return false;
		*(int32_t *) body = PW_MAP_PTR_TO_ID(t);
		break;

	case SPA_POD_TYPE_PROP:
	{
		struct spa_pod_prop_body *b = body;

		if ((t = pw_map_lookup(types, b->key)) == NULL)
			return false;
		b->key = PW_MAP_PTR_TO_ID(t);

		if (b->value.type == SPA_POD_TYPE_ID) {
			void *alt;
			if (!pw_pod_remap_data
			    (b->value.type, SPA_POD_BODY(&b->value), b->value.size, types))
				return false;

			SPA_POD_PROP_ALTERNATIVE_FOREACH(b, size, alt)
				if (!pw_pod_remap_data(b->value.type, alt, b->value.size, types))
					return false;
		}
		break;
	}
	case SPA_POD_TYPE_OBJECT:
	{
		struct spa_pod_object_body *b = body;
		struct spa_pod *p;

		if ((t = pw_map_lookup(types, b->type)) == NULL)
			return false;
		b->type = PW_MAP_PTR_TO_ID(t);

		SPA_POD_OBJECT_BODY_FOREACH(b, size, p)
			if (!pw_pod_remap_data(p->type, SPA_POD_BODY(p), p->size, types))
				return false;
		break;
	}
	case SPA_POD_TYPE_STRUCT:
	{
		struct spa_pod *b = body, *p;

		SPA_POD_FOREACH(b, size, p)
			if (!pw_pod_remap_data(p->type, SPA_POD_BODY(p), p->size, types))
				return false;
		break;
	}
	default:
		break;
	}
	return true;
}
