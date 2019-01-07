/* Simple Plugin API
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
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
	{ SPA_DATA_Invalid, SPA_TYPE_DATA_BASE "Invalid", SPA_TYPE_Int, NULL },
	{ SPA_DATA_MemPtr, SPA_TYPE_DATA_BASE "MemPtr", SPA_TYPE_Int, NULL },
	{ SPA_DATA_MemFd, SPA_TYPE_DATA_FD_BASE "MemFd", SPA_TYPE_Int, NULL },
	{ SPA_DATA_DmaBuf, SPA_TYPE_DATA_FD_BASE "DmaBuf", SPA_TYPE_Int, NULL },
	{ 0, NULL, 0, NULL },
};

#define SPA_TYPE__Meta			SPA_TYPE_POINTER_BASE "Meta"
#define SPA_TYPE_META_BASE		SPA_TYPE__Meta ":"

#define SPA_TYPE_META__Region		SPA_TYPE_META_BASE "Region"
#define SPA_TYPE_META_REGION_BASE	SPA_TYPE_META__Region ":"

#define SPA_TYPE_META__RegionArray	SPA_TYPE_META_BASE "RegionArray"
#define SPA_TYPE_META_REGION_ARRAY_BASE	SPA_TYPE_META__RegionArray ":"

static const struct spa_type_info spa_type_meta_type[] = {
	{ SPA_META_Invalid, SPA_TYPE_META_BASE "Invalid", SPA_TYPE_Pointer, NULL },
	{ SPA_META_Header, SPA_TYPE_META_BASE "Header", SPA_TYPE_Pointer, NULL },
	{ SPA_META_VideoCrop, SPA_TYPE_META_REGION_BASE "VideoCrop", SPA_TYPE_Pointer, NULL },
	{ SPA_META_VideoDamage, SPA_TYPE_META_REGION_ARRAY_BASE "VideoDamage", SPA_TYPE_Pointer, NULL },
	{ 0, NULL, 0, NULL },
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_BUFFER_TYPES_H__ */
