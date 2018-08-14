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

#ifndef __SPA_DEBUG_MEM_H__
#define __SPA_DEBUG_MEM_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/dict.h>

#ifndef spa_debug
#define spa_debug(...)	({ fprintf(stderr, __VA_ARGS__);fputc('\n', stderr); })
#endif

static inline int spa_debug_mem(int indent, const void *data, size_t size)
{
	const uint8_t *t = data;
	char buffer[512];
	int i, pos = 0;

	for (i = 0; i < size; i++) {
		if (i % 16 == 0)
			pos = sprintf(buffer, "%p: ", &t[i]);
		pos += sprintf(buffer + pos, "%02x ", t[i]);
		if (i % 16 == 15 || i == size - 1) {
			spa_debug("%*s" "%s", indent, "", buffer);
		}
	}
	return 0;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_DEBUG_MEM_H__ */
