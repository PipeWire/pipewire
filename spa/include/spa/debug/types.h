/* Simple Plugin API
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_DEBUG_TYPES_H__
#define __SPA_DEBUG_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/type-info.h>

static inline const struct spa_type_info *spa_debug_type_find(const struct spa_type_info *info, uint32_t type)
{
	const struct spa_type_info *res;

	if (info == NULL)
		info = SPA_TYPE_ROOT;

	while (info && info->name) {
		if (info->type == SPA_ID_INVALID) {
			if (info->values && (res = spa_debug_type_find(info->values, type)))
				return res;
		}
		else if (info->type == type)
			return info;
		info++;
	}
	return NULL;
}

static inline const char *spa_debug_type_find_name(const struct spa_type_info *info, uint32_t type)
{
	if ((info = spa_debug_type_find(info, type)) == NULL)
		return NULL;
	return info->name;
}

static inline uint32_t spa_debug_type_find_type(const struct spa_type_info *info, const char *name)
{
	if (info == NULL)
		info = SPA_TYPE_ROOT;

	while (info && info->name) {
		uint32_t res;
		if (strcmp(info->name, name) == 0)
			return info->type;
		if (info->values && (res = spa_debug_type_find_type(info->values, name)) != SPA_ID_INVALID)
			return res;
		info++;
	}
	return SPA_ID_INVALID;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_DEBUG_NODE_H__ */
