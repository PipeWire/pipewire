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

#ifndef __SPA_EVENT_NODE_H__
#define __SPA_EVENT_NODE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/pod/event.h>
#include <spa/support/type-map.h>
#include <spa/node/node.h>

#define SPA_TYPE_EVENT__Node		SPA_TYPE_EVENT_BASE "Node"
#define SPA_TYPE_EVENT_NODE_BASE	SPA_TYPE_EVENT__Node ":"

#define SPA_TYPE_EVENT_NODE__Error		SPA_TYPE_EVENT_NODE_BASE "Error"
#define SPA_TYPE_EVENT_NODE__Buffering		SPA_TYPE_EVENT_NODE_BASE "Buffering"
#define SPA_TYPE_EVENT_NODE__RequestRefresh	SPA_TYPE_EVENT_NODE_BASE "RequestRefresh"
#define SPA_TYPE_EVENT_NODE__RequestClockUpdate	SPA_TYPE_EVENT_NODE_BASE "RequestClockUpdate"

struct spa_type_event_node {
	uint32_t Error;
	uint32_t Buffering;
	uint32_t RequestRefresh;
	uint32_t RequestClockUpdate;
};

static inline void
spa_type_event_node_map(struct spa_type_map *map, struct spa_type_event_node *type)
{
	if (type->Error == 0) {
		type->Error = spa_type_map_get_id(map, SPA_TYPE_EVENT_NODE__Error);
		type->Buffering = spa_type_map_get_id(map, SPA_TYPE_EVENT_NODE__Buffering);
		type->RequestRefresh = spa_type_map_get_id(map, SPA_TYPE_EVENT_NODE__RequestRefresh);
		type->RequestClockUpdate = spa_type_map_get_id(map, SPA_TYPE_EVENT_NODE__RequestClockUpdate);
	}
}

struct spa_event_node_request_clock_update_body {
	struct spa_pod_object_body body;
#define SPA_EVENT_NODE_REQUEST_CLOCK_UPDATE_TIME	(1 << 0)
#define SPA_EVENT_NODE_REQUEST_CLOCK_UPDATE_SCALE	(1 << 1)
#define SPA_EVENT_NODE_REQUEST_CLOCK_UPDATE_STATE	(1 << 2)
	struct spa_pod_int update_mask		SPA_ALIGNED(8);
	struct spa_pod_long timestamp		SPA_ALIGNED(8);
	struct spa_pod_long offset		SPA_ALIGNED(8);
};

struct spa_event_node_request_clock_update {
	struct spa_pod pod;
	struct spa_event_node_request_clock_update_body body;
};

#define SPA_EVENT_NODE_REQUEST_CLOCK_UPDATE_INIT(type,update_mask,timestamp,offset)	\
	SPA_EVENT_INIT_FULL(struct spa_event_node_request_clock_update,		\
		sizeof(struct spa_event_node_request_clock_update_body), type,		\
		SPA_POD_INT_INIT(update_mask),						\
		SPA_POD_LONG_INIT(timestamp),						\
		SPA_POD_LONG_INIT(offset))

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_EVENT_NODE_H__ */
