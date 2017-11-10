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

#ifndef __SPA_TYPE_H__
#define __SPA_TYPE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>

#define SPA_TYPE_BASE				"Spa:"

#define SPA_TYPE__Enum				SPA_TYPE_BASE "Enum"
#define SPA_TYPE_ENUM_BASE			SPA_TYPE__Enum ":"

#define SPA_TYPE__Pointer			SPA_TYPE_BASE "Pointer"
#define SPA_TYPE_POINTER_BASE			SPA_TYPE__Pointer ":"

#define SPA_TYPE__Interface			SPA_TYPE_BASE "Interface"
#define SPA_TYPE_INTERFACE_BASE			SPA_TYPE__Interface ":"

#define SPA_TYPE__Object			SPA_TYPE_BASE "Object"
#define SPA_TYPE_OBJECT_BASE			SPA_TYPE__Object ":"

static inline bool spa_type_is_a(const char *type, const char *parent)
{
	return type != NULL && parent != NULL && strncmp(type, parent, strlen(parent)) == 0;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_TYPE_H__ */
