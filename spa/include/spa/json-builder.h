/* Spa
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __SPA_JSON_BUILDER_H__
#define __SPA_JSON_BUILDER_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

#include <spa/defs.h>

struct spa_json_builder {
	char *data;
	int size;
	int offset;
	int (*printf) (void *user_data, const char *format, va_list args);
	void *user_data;
};

static inline int spa_json_builder_printf(struct spa_json_builder *builder,
					  const char *format, ...)
{
        va_list args;
	int res;

	va_start(args, format);
	if (builder->printf)
		res = builder->printf(builder->user_data, format, args);
	else {
		int size = builder->size;
		int offset = builder->offset;
		int remain = SPA_MAX(0, size - offset);

		res = vsnprintf(&builder->data[offset], remain, format, args);
		builder->offset += res;
		if (builder->offset > size)
			res = -1;
	}
	va_end(args);

	return res;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_JSON_BUILDER_H__ */
