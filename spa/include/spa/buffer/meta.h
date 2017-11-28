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
#include <spa/support/type-map.h>

/** \page page_meta Metadata
 *
 * Metadata contains extra information on a buffer.
 */
#define SPA_TYPE__Meta			SPA_TYPE_POINTER_BASE "Meta"
#define SPA_TYPE_META_BASE		SPA_TYPE__Meta ":"

#define SPA_TYPE_META__Header		SPA_TYPE_META_BASE "Header"
#define SPA_TYPE_META__VideoCrop	SPA_TYPE_META_BASE "VideoCrop"

/**
 * A metadata element.
 *
 * This structure is available on the buffer structure and contains
 * the type of the metadata and a pointer/size to the actual metadata
 * itself.
 */
struct spa_meta {
	uint32_t type;		/**< metadata type */
	void *data;		/**< pointer to metadata */
	uint32_t size;		/**< size of metadata */
};

/**
 * Describes essential buffer header metadata such as flags and
 * timestamps.
 */
struct spa_meta_header {
#define SPA_META_HEADER_FLAG_DISCONT	(1 << 0)	/**< data is not continous with previous buffer */
#define SPA_META_HEADER_FLAG_CORRUPTED	(1 << 1)	/**< data might be corrupted */
#define SPA_META_HEADER_FLAG_MARKER	(1 << 2)	/**< media specific marker */
#define SPA_META_HEADER_FLAG_HEADER	(1 << 3)	/**< data contains a codec specific header */
#define SPA_META_HEADER_FLAG_GAP	(1 << 4)	/**< data contains media neutral data */
#define SPA_META_HEADER_FLAG_DELTA_UNIT	(1 << 5)	/**< cannot be decoded independently */
	uint32_t flags;				/**< flags */
	uint32_t seq;				/**< sequence number, increments with a
						  *  media specific frequency */
	int64_t pts;				/**< presentation timestamp */
	int64_t dts_offset;			/**< decoding timestamp and a difference with pts */
};

/**
 * Video cropping metadata
 * a */
struct spa_meta_video_crop {
	int32_t x, y;		/**< x and y offsets */
	int32_t width, height;	/**< width and height */
};

/**
 * Describes a control location in the buffer.
 */
struct spa_meta_control {
	uint32_t id;		/**< control id */
	uint32_t offset;	/**< offset in buffer memory */
};

struct spa_type_meta {
	uint32_t Header;
	uint32_t VideoCrop;
};

static inline void spa_type_meta_map(struct spa_type_map *map, struct spa_type_meta *type)
{
	if (type->Header == 0) {
		type->Header = spa_type_map_get_id(map, SPA_TYPE_META__Header);
		type->VideoCrop = spa_type_map_get_id(map, SPA_TYPE_META__VideoCrop);
	}
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_META_H__ */
