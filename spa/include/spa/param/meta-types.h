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

#ifndef __SPA_PARAM_META_H__
#define __SPA_PARAM_META_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <fcntl.h> /* for off_t */

#include <spa/utils/defs.h>
#include <spa/param/param.h>
#include <spa/support/type-map.h>

#define SPA_TYPE_PARAM__Meta			SPA_TYPE_PARAM_BASE "Meta"
#define SPA_TYPE_PARAM_META_BASE		SPA_TYPE_PARAM__Meta ":"
#define SPA_TYPE_PARAM_META__type		SPA_TYPE_PARAM_META_BASE "type"
#define SPA_TYPE_PARAM_META__size		SPA_TYPE_PARAM_META_BASE "size"

struct spa_type_param_meta {
	uint32_t Meta;
	uint32_t type;
	uint32_t size;
};

static inline void
spa_type_param_meta_map(struct spa_type_map *map,
			struct spa_type_param_meta *type)
{
	if (type->Meta == 0) {
		size_t i;
#define OFF(n) offsetof(struct spa_type_param_meta, n)
		static struct { off_t offset; const char *type; } tab[] = {
			{ OFF(Meta), SPA_TYPE_PARAM__Meta },
			{ OFF(type), SPA_TYPE_PARAM_META__type },
			{ OFF(size), SPA_TYPE_PARAM_META__size },
		};
#undef OFF
		for (i = 0; i < SPA_N_ELEMENTS(tab); i++)
			*SPA_MEMBER(type, tab[i].offset, uint32_t) = spa_type_map_get_id(map, tab[i].type);
	}
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_META_H__ */
