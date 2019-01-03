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

#ifndef __SPA_META_H__
#define __SPA_META_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>

/** \page page_meta Metadata
 *
 * Metadata contains extra information on a buffer.
 */
enum spa_meta_type {
	SPA_META_Invalid,
	SPA_META_Header,
	SPA_META_VideoCrop,
	SPA_META_VideoDamage,
	SPA_META_Bitmap,
	SPA_META_Cursor,
};

/**
 * A metadata element.
 *
 * This structure is available on the buffer structure and contains
 * the type of the metadata and a pointer/size to the actual metadata
 * itself.
 */
struct spa_meta {
	uint32_t type;		/**< metadata type, one of enum spa_meta_type */
	void *data;		/**< pointer to metadata */
	uint32_t size;		/**< size of metadata */
};

#define spa_meta_first(m)	((m)->data)
#define spa_meta_end(m)		((m)->data + (m)->size)
#define spa_meta_check(p,m)	((void*)(p) + sizeof(*p) <= spa_meta_end(m))

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

/** metadata structure for Region or an array of these for RegionArray */
struct spa_meta_region {
	struct spa_region region;
};

#define spa_meta_region_is_valid(m)	((m)->region.size.width != 0 && (m)->region.size.height != 0)

#define spa_meta_region_for_each(pos,meta)				\
	for (pos = spa_meta_first(meta);				\
	    spa_meta_check(pos, meta);					\
            (pos)++)

#define spa_meta_bitmap_is_valid(m)	((m)->format != 0)

/**
 * Bitmap information
 *
 * This metadata contains a bitmap image in the given format and size.
 * It is typically used for cursor images or other small images that are
 * better transfered inline.
 */
struct spa_meta_bitmap {
	uint32_t format;		/**< bitmap video format, one of enum spa_video_format. 0 is
					  *  and invalid format and should be handled as if there is
					  *  no new bitmap information. */
	struct spa_rectangle size;	/**< width and height of bitmap */
	int32_t stride;			/**< stride of bitmap data */
	uint32_t offset;		/**< offset of bitmap data in this structure. An offset of
					  *  0 means no image data (invisible), an offset >=
					  *  sizeof(struct spa_meta_bitmap) contains valid bitmap
					  *  info. */
};

#define spa_meta_cursor_is_valid(m)	((m)->id != 0)

/**
 * Cursor information
 *
 * Metadata to describe the position and appearance of a pointing device.
 */
struct spa_meta_cursor {
	uint32_t id;			/**< cursor id. an id of 0 is an invalid id and means that
					  *  there is no new cursor data */
	uint32_t flags;			/**< extra flags */
	struct spa_point position;	/**< position on screen */
	struct spa_point hotspot;	/**< offsets for hotspot in bitmap */
	uint32_t bitmap_offset;		/**< offset of bitmap meta in this structure. When the offset
					  *  is 0, there is no new bitmap information. When the offset is
					  *  >= sizeof(struct spa_meta_cursor) there is a
					  *  struct spa_meta_bitmap at the offset. */
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_META_H__ */
