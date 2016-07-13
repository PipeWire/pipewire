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
 * @data: pointer to metadata
 * @size: size of metadata
 */
typedef struct {
  SpaMetaType  type;
  void        *data;
  size_t       size;
} SpaMeta;

/**
 * SpaDataType:
 * @SPA_DATA_TYPE_INVALID: invalid data type, is ignored
 * @SPA_DATA_TYPE_MEMPTR: data and size point to memory accassible by the
 *                        CPU.
 * @SPA_DATA_TYPE_FD: data points to an int file descriptor that can be
 *                    mmapped.
 * @SPA_DATA_TYPE_POINTER: data points to some other datastructure, the
 *        type can be found in ptr_type
 */
typedef enum {
  SPA_DATA_TYPE_INVALID               = 0,
  SPA_DATA_TYPE_MEMPTR,
  SPA_DATA_TYPE_FD,
  SPA_DATA_TYPE_POINTER,
} SpaDataType;

/**
 * SpaData:
 * @id: user id
 * @type: the type of data
 * @ptr_type: more info abouut the type of @ptr
 * @ptr: pointer to data or fd
 * @offset: offset of data
 * @size: size of data
 * @stride: stride of data if applicable
 */
typedef struct {
  SpaDataType  type;
  const char  *ptr_type;
  void        *ptr;
  unsigned int offset;
  size_t       size;
  size_t       stride;
} SpaData;

/**
 * SpaBuffer:
 * @refcount: reference counter
 * @notify: called when the refcount reaches 0
 * @size: total size of the buffer data
 * @n_metas: number of metadata
 * @metas: array of @n_metas metadata
 * @n_datas: number of data pointers
 * @datas: array of @n_datas data pointers
 */
struct _SpaBuffer {
  volatile int      refcount;
  SpaNotify         notify;
  size_t            size;
  unsigned int      n_metas;
  SpaMeta          *metas;
  unsigned int      n_datas;
  SpaData          *datas;
};

/**
 * spa_buffer_ref:
 * @buffer: a #SpaBuffer
 *
 * Increase the refcount on @buffer
 *
 * Returns: @buffer
 */
static inline SpaBuffer *
spa_buffer_ref (SpaBuffer *buffer)
{
  if (buffer != NULL)
    buffer->refcount++;
  return buffer;
}

/**
 * spa_buffer_unref:
 * @buffer: a #SpaBuffer
 *
 * Decrease the refcount on buffer. when the refcount is 0, the notify,
 * if any, of the buffer will be called.
 *
 * Returns: @buffer or %NULL when the refcount is 0
 */
static inline SpaBuffer *
spa_buffer_unref (SpaBuffer *buffer)
{
  if (buffer != NULL) {
    if (--buffer->refcount == 0) {
      if (buffer->notify)
        buffer->notify (buffer);
      return NULL;
    }
  }
  return buffer;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_BUFFER_H__ */
