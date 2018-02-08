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

#ifndef __SPA_TYPE_MAP_IMPL_H__
#define __SPA_TYPE_MAP_IMPL_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/support/type-map.h>

struct spa_type_map_impl_data {
	struct spa_type_map map;
	unsigned int n_types;
	char *types[1];
};

static inline uint32_t
spa_type_map_impl_get_id (struct spa_type_map *map, const char *type)
{
	struct spa_type_map_impl_data *impl = (struct spa_type_map_impl_data *) map;
	uint32_t i = 0;

	for (i = 1; i <= impl->n_types; i++) {
		if (strcmp(impl->types[i], type) == 0)
			return i;
	}
	impl->types[i] = (char *) type;
	impl->n_types++;
        return i;
}

static inline const char *
spa_type_map_impl_get_type (const struct spa_type_map *map, uint32_t id)
{
	struct spa_type_map_impl_data *impl = (struct spa_type_map_impl_data *) map;
        if (id <= impl->n_types)
                return impl->types[id];
        return NULL;
}

static inline size_t spa_type_map_impl_get_size (const struct spa_type_map *map)
{
	struct spa_type_map_impl_data *impl = (struct spa_type_map_impl_data *) map;
	return impl->n_types;
}

#define SPA_TYPE_MAP_IMPL_DEFINE(name,maxtypes)	\
struct  {					\
	struct spa_type_map map;		\
	unsigned int n_types;			\
	char *types[maxtypes];			\
} name

#define SPA_TYPE_MAP_IMPL_INIT			\
	{ { SPA_VERSION_TYPE_MAP,		\
	    NULL,				\
	    spa_type_map_impl_get_id,		\
	    spa_type_map_impl_get_type,		\
	    spa_type_map_impl_get_size,},	\
	  0, { NULL, } }

#define SPA_TYPE_MAP_IMPL(name,maxtypes)		\
	SPA_TYPE_MAP_IMPL_DEFINE(name,maxtypes) = SPA_TYPE_MAP_IMPL_INIT

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_TYPE_MAP_IMPL_H__ */
