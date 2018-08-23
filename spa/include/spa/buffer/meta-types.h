/* Simple Plugin API
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

#ifndef __SPA_META_H__
#define __SPA_META_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/utils/type-info.h>

#define SPA_TYPE__Meta			SPA_TYPE_POINTER_BASE "Meta"
#define SPA_TYPE_META_BASE		SPA_TYPE__Meta ":"

#define SPA_TYPE_META__Region		SPA_TYPE_META_BASE "Region"
#define SPA_TYPE_META_REGION_BASE	SPA_TYPE_META__Region ":"

#define SPA_TYPE_META__RegionArray	SPA_TYPE_META_BASE "RegionArray"
#define SPA_TYPE_META_REGION_ARRAY_BASE	SPA_TYPE_META__RegionArray ":"

static const struct spa_type_info spa_type_meta_type[] = {
	{ SPA_META_Header, SPA_TYPE_META_BASE "Header", SPA_POD_TYPE_INT, },
	{ SPA_META_VideoCrop, SPA_TYPE_META_REGION_BASE "VideoCrop", SPA_POD_TYPE_INT, },
	{ SPA_META_VideoDamage, SPA_TYPE_META_REGION_ARRAY_BASE "VideoDamage", SPA_POD_TYPE_INT, },
	{ 0, NULL, },
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_META_H__ */
