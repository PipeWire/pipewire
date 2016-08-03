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

#ifndef __SPA_BUFFER_H__
#define __SPA_BUFFER_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaBuffer SpaBuffer;
typedef struct _SpaBufferGroup SpaBufferGroup;

#include <spa/defs.h>

/**
 * SpaMetaType:
 * @SPA_META_TYPE_INVALID: invalid metadata, should be ignored
 * @SPA_META_TYPE_HEADER: header metadata
 */
typedef enum {
  SPA_META_TYPE_INVALID               = 0,
  SPA_META_TYPE_HEADER,
  SPA_META_TYPE_POINTER,
  SPA_META_TYPE_VIDEO_CROP,
} SpaMetaType;

/**
 * SpaBufferFlags:
 * @SPA_BUFFER_FLAG_NONE: no flag
 * @SPA_BUFFER_FLAG_DISCONT: the buffer marks a data discontinuity
 * @SPA_BUFFER_FLAG_CORRUPTED: the buffer data might be corrupted
 * @SPA_BUFFER_FLAG_MARKER: the buffer contains a media specific marker
 * @SPA_BUFFER_FLAG_HEADER: the buffer contains a header
 * @SPA_BUFFER_FLAG_GAP: the buffer has been constructed to fill a gap
 *                       and contains media neutral data
 * @SPA_BUFFER_FLAG_DELTA_UNIT: the media cannot be decoded independently
 */
typedef enum {
  SPA_BUFFER_FLAG_NONE               =  0,
  SPA_BUFFER_FLAG_DISCONT            = (1 << 0),
  SPA_BUFFER_FLAG_CORRUPTED          = (1 << 1),
  SPA_BUFFER_FLAG_MARKER             = (1 << 2),
  SPA_BUFFER_FLAG_HEADER             = (1 << 3),
  SPA_BUFFER_FLAG_GAP                = (1 << 4),
  SPA_BUFFER_FLAG_DELTA_UNIT         = (1 << 5),
} SpaBufferFlags;

typedef struct {
  SpaBufferFlags flags;
  uint32_t seq;
  int64_t pts;
  int64_t dts_offset;
} SpaMetaHeader;

typedef struct {
  const char *ptr_type;
  void       *ptr;
} SpaMetaPointer;

/**
 * SpaMetaVideoCrop:
 * @x:
 * @y:
 * @width:
 * @height
 */
typedef struct {
  int   x, y;
  int   width, height;
} SpaMetaVideoCrop;

/**
 * SpaMeta:
 * @type: metadata type
 * @offset: offset of metadata
 * @size: size of metadata
 */
typedef struct {
  SpaMetaType  type;
  off_t        offset;
  size_t       size;
} SpaMeta;

/**
 * SpaData:
 * @mem_id: the memory id to use
 * @offset: offset of memory
 * @size: size of memory
 * @stride: stride of memory if applicable
 */
typedef struct {
  uint32_t     mem_id;
  off_t        offset;
  size_t       size;
  size_t       stride;
} SpaData;

/**
 * SpaBuffer:
 * @id: buffer id
 * @mem_id: memory id of the buffer
 * @offset: offset into the memory
 * @size: total size of the buffer structure
 * @n_metas: number of metadata
 * @metas: offset of array of @n_metas metadata
 * @n_datas: number of data pointers
 * @datas: offset of array of @n_datas data pointers
 */
struct _SpaBuffer {
  uint32_t          id;
  uint32_t          mem_id;
  off_t             offset;
  size_t            size;
  unsigned int      n_metas;
  off_t             metas;
  unsigned int      n_datas;
  off_t             datas;
};

#define SPA_BUFFER_METAS(b)        (SPA_MEMBER ((b), (b)->metas, SpaMeta))
#define SPA_BUFFER_DATAS(b)        (SPA_MEMBER ((b), (b)->datas, SpaData))


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_BUFFER_H__ */
