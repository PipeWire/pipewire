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

#include <spa/support/type-map.h>
#include <spa/utils/defs.h>
#include <spa/clock/clock.h>
#include <spa/param/format.h>
#include <spa/param/props.h>
#include <spa/monitor/monitor.h>

#include "pipewire/pipewire.h"
#include "pipewire/type.h"
#include "pipewire/module.h"


/** Initializes the type system
 * \param type a type structure
 * \memberof pw_type
 */
void pw_type_init(struct pw_type *type)
{
	type->map = pw_get_support_interface(SPA_TYPE__TypeMap);

	type->core = spa_type_map_get_id(type->map, PW_TYPE_INTERFACE__Core);
	type->registry = spa_type_map_get_id(type->map, PW_TYPE_INTERFACE__Registry);
	type->node = spa_type_map_get_id(type->map, PW_TYPE_INTERFACE__Node);
	type->factory = spa_type_map_get_id(type->map, PW_TYPE_INTERFACE__Factory);
	type->link = spa_type_map_get_id(type->map, PW_TYPE_INTERFACE__Link);
	type->client = spa_type_map_get_id(type->map, PW_TYPE_INTERFACE__Client);
	type->module = spa_type_map_get_id(type->map, PW_TYPE_INTERFACE__Module);

	type->spa_log = spa_type_map_get_id(type->map, SPA_TYPE__Log);
	type->spa_node = spa_type_map_get_id(type->map, SPA_TYPE__Node);
	type->spa_clock = spa_type_map_get_id(type->map, SPA_TYPE__Clock);
	type->spa_monitor = spa_type_map_get_id(type->map, SPA_TYPE__Monitor);
	type->spa_format = spa_type_map_get_id(type->map, SPA_TYPE__Format);
	type->spa_props = spa_type_map_get_id(type->map, SPA_TYPE__Props);

	spa_type_io_map(type->map, &type->io);
	spa_type_param_map(type->map, &type->param);
	spa_type_meta_map(type->map, &type->meta);
	spa_type_data_map(type->map, &type->data);
	spa_type_event_node_map(type->map, &type->event_node);
	spa_type_command_node_map(type->map, &type->command_node);
	spa_type_monitor_map(type->map, &type->monitor);
	spa_type_param_buffers_map(type->map, &type->param_buffers);
	spa_type_param_meta_map(type->map, &type->param_meta);
}
