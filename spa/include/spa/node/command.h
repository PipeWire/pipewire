/* Simple Plugin API
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_COMMAND_NODE_H__
#define __SPA_COMMAND_NODE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/support/type-map.h>
#include <spa/pod/command.h>

#define SPA_TYPE_COMMAND__Node			SPA_TYPE_COMMAND_BASE "Node"
#define SPA_TYPE_COMMAND_NODE_BASE		SPA_TYPE_COMMAND__Node ":"

/** Suspend a node. This will release all resources of the node */
#define SPA_TYPE_COMMAND_NODE__Suspend		SPA_TYPE_COMMAND_NODE_BASE "Suspend"
/** Pause processing of a node */
#define SPA_TYPE_COMMAND_NODE__Pause		SPA_TYPE_COMMAND_NODE_BASE "Pause"
/** Start processing of a node */
#define SPA_TYPE_COMMAND_NODE__Start		SPA_TYPE_COMMAND_NODE_BASE "Start"
/** Enable ports of a node. When sent to a port, enables just that port. Enabled
 * ports on a Started node begin streaming immediately */
#define SPA_TYPE_COMMAND_NODE__Enable		SPA_TYPE_COMMAND_NODE_BASE "Enable"
/** Disable ports of a node. When sent to a port, disables just that port. */
#define SPA_TYPE_COMMAND_NODE__Disable		SPA_TYPE_COMMAND_NODE_BASE "Disable"
/** Flush all data from the node or port */
#define SPA_TYPE_COMMAND_NODE__Flush		SPA_TYPE_COMMAND_NODE_BASE "Flush"
/** Drain all data from the node or port */
#define SPA_TYPE_COMMAND_NODE__Drain		SPA_TYPE_COMMAND_NODE_BASE "Drain"
/** Set a marker on a node or port */
#define SPA_TYPE_COMMAND_NODE__Marker		SPA_TYPE_COMMAND_NODE_BASE "Marker"

struct spa_type_command_node {
	uint32_t Suspend;
	uint32_t Pause;
	uint32_t Start;
	uint32_t Enable;
	uint32_t Disable;
	uint32_t Flush;
	uint32_t Drain;
	uint32_t Marker;
};

static inline void
spa_type_command_node_map(struct spa_type_map *map, struct spa_type_command_node *type)
{
	if (type->Suspend == 0) {
		type->Suspend = spa_type_map_get_id(map, SPA_TYPE_COMMAND_NODE__Suspend);
		type->Pause = spa_type_map_get_id(map, SPA_TYPE_COMMAND_NODE__Pause);
		type->Start = spa_type_map_get_id(map, SPA_TYPE_COMMAND_NODE__Start);
		type->Enable = spa_type_map_get_id(map, SPA_TYPE_COMMAND_NODE__Enable);
		type->Disable = spa_type_map_get_id(map, SPA_TYPE_COMMAND_NODE__Disable);
		type->Flush = spa_type_map_get_id(map, SPA_TYPE_COMMAND_NODE__Flush);
		type->Drain = spa_type_map_get_id(map, SPA_TYPE_COMMAND_NODE__Drain);
		type->Marker = spa_type_map_get_id(map, SPA_TYPE_COMMAND_NODE__Marker);
	}
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* _SPA_COMMAND_NODE_H__ */
