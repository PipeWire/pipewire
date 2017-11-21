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

#ifndef __SPA_PARAM_IO_H__
#define __SPA_PARAM_IO_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/param/param.h>
#include <spa/support/type-map.h>

#define SPA_TYPE_PARAM__IO		SPA_TYPE_PARAM_BASE "IO"
#define SPA_TYPE_PARAM_IO_BASE		SPA_TYPE_PARAM__IO ":"

#define SPA_TYPE_PARAM_IO__id		SPA_TYPE_PARAM_IO_BASE "id"
#define SPA_TYPE_PARAM_IO__size		SPA_TYPE_PARAM_IO_BASE "size"
#define SPA_TYPE_PARAM_IO__propId	SPA_TYPE_PARAM_IO_BASE "propId"

struct spa_type_param_io {
	uint32_t IO;
	uint32_t id;
	uint32_t size;
	uint32_t propId;
};

static inline void
spa_type_param_io_map(struct spa_type_map *map,
		      struct spa_type_param_io *type)
{
	if (type->IO == 0) {
		type->IO = spa_type_map_get_id(map, SPA_TYPE_PARAM__IO);
		type->id = spa_type_map_get_id(map, SPA_TYPE_PARAM_IO__id);
		type->size = spa_type_map_get_id(map, SPA_TYPE_PARAM_IO__size);
		type->propId = spa_type_map_get_id(map, SPA_TYPE_PARAM_IO__propId);
	}
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_IO_H__ */
