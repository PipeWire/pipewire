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

#ifndef __SPA_LIBDEBUG_H__
#define __SPA_LIBDEBUG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>
#include <spa/node.h>
#include <spa/buffer.h>
#include <spa/pod.h>
#include <spa/props.h>
#include <spa/format.h>
#include <spa/dict.h>
#include <spa/log.h>

void spa_debug_set_type_map(const struct spa_type_map *map);

int spa_debug_port_info(const struct spa_port_info *info);
int spa_debug_buffer(const struct spa_buffer *buffer);
#define SPA_DEBUG_FLAG_FORMAT	(1 << 0)
int spa_debug_pod(const struct spa_pod *pod, uint32_t flags);
int spa_debug_dump_mem(const void *data, size_t size);
int spa_debug_dict(const struct spa_dict *dict);

#ifdef __cplusplus
} /* extern "C" */
#endif
#endif /* __SPA_LIBDEBUG_H__ */
