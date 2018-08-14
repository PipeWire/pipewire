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

#ifndef __SPA_DEBUG_FORMAT_H__
#define __SPA_DEBUG_FORMAT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/support/type-map.h>
#include <spa/debug/mem.h>
#include <spa/debug/pod.h>

#ifndef spa_debug
#define spa_debug(...)	({ fprintf(stderr, __VA_ARGS__);fputc('\n', stderr); })
#endif

static inline int spa_debug_format(int indent,
		struct spa_type_map *map, const struct spa_pod *pod)
{
	return spa_debug_pod_value(indent, map,
			SPA_POD_TYPE(pod),
			SPA_POD_BODY(pod),
			SPA_POD_BODY_SIZE(pod));
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_DEBUG_FORMAT_H__ */
