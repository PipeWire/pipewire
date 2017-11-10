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

#ifndef __SPA_PARAM_BUFFERS_H__
#define __SPA_PARAM_BUFFERS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/param/param.h>
#include <spa/support/type-map.h>

#define SPA_TYPE_PARAM__Buffers			SPA_TYPE_PARAM_BASE "Buffers"
#define SPA_TYPE_PARAM_BUFFERS_BASE		SPA_TYPE_PARAM__Buffers ":"

#define SPA_TYPE_PARAM_BUFFERS__size		SPA_TYPE_PARAM_BUFFERS_BASE "size"
#define SPA_TYPE_PARAM_BUFFERS__stride		SPA_TYPE_PARAM_BUFFERS_BASE "stride"
#define SPA_TYPE_PARAM_BUFFERS__buffers		SPA_TYPE_PARAM_BUFFERS_BASE "buffers"
#define SPA_TYPE_PARAM_BUFFERS__align		SPA_TYPE_PARAM_BUFFERS_BASE "align"

struct spa_type_param_buffers {
	uint32_t Buffers;
	uint32_t size;
	uint32_t stride;
	uint32_t buffers;
	uint32_t align;
};

static inline void
spa_type_param_buffers_map(struct spa_type_map *map,
			   struct spa_type_param_buffers *type)
{
	if (type->Buffers == 0) {
		type->Buffers = spa_type_map_get_id(map, SPA_TYPE_PARAM__Buffers);
		type->size = spa_type_map_get_id(map, SPA_TYPE_PARAM_BUFFERS__size);
		type->stride = spa_type_map_get_id(map, SPA_TYPE_PARAM_BUFFERS__stride);
		type->buffers = spa_type_map_get_id(map, SPA_TYPE_PARAM_BUFFERS__buffers);
		type->align = spa_type_map_get_id(map, SPA_TYPE_PARAM_BUFFERS__align);
	}
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_BUFFERS_H__ */
