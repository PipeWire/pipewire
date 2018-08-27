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

#ifndef __SPA_BUFFER_TYPES_H__
#define __SPA_BUFFER_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/utils/type-info.h>

#define SPA_TYPE__Buffer		SPA_TYPE_POINTER_BASE "Buffer"
#define SPA_TYPE_BUFFER_BASE		SPA_TYPE__Buffer ":"

/** Buffers contain data of a certain type */
#define SPA_TYPE__Data			SPA_TYPE_ENUM_BASE "Data"
#define SPA_TYPE_DATA_BASE		SPA_TYPE__Data ":"

/** base type for fd based memory */
#define SPA_TYPE_DATA__Fd		SPA_TYPE_DATA_BASE "Fd"
#define SPA_TYPE_DATA_FD_BASE		SPA_TYPE_DATA__Fd ":"

static const struct spa_type_info spa_type_data_type[] = {
	{ SPA_DATA_MemPtr, SPA_TYPE_DATA_BASE "MemPtr", SPA_TYPE_Int, },
	{ SPA_DATA_MemFd, SPA_TYPE_DATA_FD_BASE "MemFd", SPA_TYPE_Int, },
	{ SPA_DATA_DmaBuf, SPA_TYPE_DATA_FD_BASE "DmaBuf", SPA_TYPE_Int, },
	{ 0, NULL, },
};

#define SPA_TYPE__Meta			SPA_TYPE_POINTER_BASE "Meta"
#define SPA_TYPE_META_BASE		SPA_TYPE__Meta ":"

#define SPA_TYPE_META__Region		SPA_TYPE_META_BASE "Region"
#define SPA_TYPE_META_REGION_BASE	SPA_TYPE_META__Region ":"

#define SPA_TYPE_META__RegionArray	SPA_TYPE_META_BASE "RegionArray"
#define SPA_TYPE_META_REGION_ARRAY_BASE	SPA_TYPE_META__RegionArray ":"

static const struct spa_type_info spa_type_meta_type[] = {
	{ SPA_META_Header, SPA_TYPE_META_BASE "Header", SPA_TYPE_Pointer },
	{ SPA_META_VideoCrop, SPA_TYPE_META_REGION_BASE "VideoCrop", SPA_TYPE_Pointer },
	{ SPA_META_VideoDamage, SPA_TYPE_META_REGION_ARRAY_BASE "VideoDamage", SPA_TYPE_Pointer },
	{ 0, NULL, },
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_BUFFER_TYPES_H__ */
