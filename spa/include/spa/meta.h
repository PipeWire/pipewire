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

#include <spa/defs.h>
#include <spa/ringbuffer.h>
#include <spa/type-map.h>

#define SPA_TYPE__Meta			SPA_TYPE_POINTER_BASE "Meta"
#define SPA_TYPE_META_BASE		SPA_TYPE__Meta ":"

#define SPA_TYPE_META__Header		SPA_TYPE_META_BASE "Header"
#define SPA_TYPE_META__Pointer		SPA_TYPE_META_BASE "Pointer"
#define SPA_TYPE_META__VideoCrop	SPA_TYPE_META_BASE "VideoCrop"
#define SPA_TYPE_META__Ringbuffer	SPA_TYPE_META_BASE "Ringbuffer"
#define SPA_TYPE_META__Shared		SPA_TYPE_META_BASE "Shared"

struct spa_type_meta {
	uint32_t Header;
	uint32_t Pointer;
	uint32_t VideoCrop;
	uint32_t Ringbuffer;
	uint32_t Shared;
};

static inline void spa_type_meta_map(struct spa_type_map *map, struct spa_type_meta *type)
{
	if (type->Header == 0) {
		type->Header = spa_type_map_get_id(map, SPA_TYPE_META__Header);
		type->Pointer = spa_type_map_get_id(map, SPA_TYPE_META__Pointer);
		type->VideoCrop = spa_type_map_get_id(map, SPA_TYPE_META__VideoCrop);
		type->Ringbuffer = spa_type_map_get_id(map, SPA_TYPE_META__Ringbuffer);
		type->Shared = spa_type_map_get_id(map, SPA_TYPE_META__Shared);
	}
}

/**
 * spa_meta_header:
 * @flags: extra flags
 * @seq: sequence number. This monotonically increments and with the rate,
 *       it can be used to derive a media time.
 * @pts: The MONOTONIC time for @seq.
 * @dts_offset: offset relative to @pts to start decoding this buffer.
 */
struct spa_meta_header {
#define SPA_META_HEADER_FLAG_DISCONT	(1 << 0)	/* data is not continous with previous buffer */
#define SPA_META_HEADER_FLAG_CORRUPTED	(1 << 1)	/* data might be corrupted */
#define SPA_META_HEADER_FLAG_MARKER	(1 << 2)	/* media specific marker */
#define SPA_META_HEADER_FLAG_HEADER	(1 << 3)	/* data contains a codec specific header */
#define SPA_META_HEADER_FLAG_GAP	(1 << 4)	/* data contains media neutral data */
#define SPA_META_HEADER_FLAG_DELTA_UNIT	(1 << 5)	/* cannot be decoded independently */
	uint32_t flags;
	uint32_t seq;
	int64_t pts;
	int64_t dts_offset;
};

struct spa_meta_pointer {
	uint32_t type;
	void *ptr;
};

/**
 * spa_meta_video_crop:
 * @x:
 * @y:
 * @width:
 * @height
 */
struct spa_meta_video_crop {
	int32_t x, y;
	int32_t width, height;
};

/**
 * spa_meta_ringbuffer:
 * @ringbuffer:
 */
struct spa_meta_ringbuffer {
	struct spa_ringbuffer ringbuffer;
};

/**
 * spa_meta_shared:
 * @flags: flags
 * @fd: the fd of the memory
 * @offset: start offset of memory
 * @size: size of the memory
 */
struct spa_meta_shared {
	int32_t flags;
	int fd;
	int32_t offset;
	uint32_t size;
};

/**
 * spa_meta:
 * @type: metadata type
 * @data: pointer to metadata
 * @size: size of metadata
 */
struct spa_meta {
	uint32_t type;
	void *data;
	uint32_t size;
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_META_H__ */
