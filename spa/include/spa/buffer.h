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

#include <spa/defs.h>
#include <spa/meta.h>
#include <spa/type-map.h>

#define SPA_TYPE__Buffer            SPA_TYPE_POINTER_BASE "Buffer"
#define SPA_TYPE_BUFFER_BASE        SPA_TYPE__Buffer ":"

#define SPA_TYPE__Data              SPA_TYPE_ENUM_BASE "DataType"
#define SPA_TYPE_DATA_BASE          SPA_TYPE__Data ":"

#define SPA_TYPE_DATA__MemPtr                SPA_TYPE_DATA_BASE "MemPtr"
#define SPA_TYPE_DATA__MemFd                 SPA_TYPE_DATA_BASE "MemFd"
#define SPA_TYPE_DATA__DmaBuf                SPA_TYPE_DATA_BASE "DmaBuf"
#define SPA_TYPE_DATA__Id                    SPA_TYPE_DATA_BASE "Id"

typedef struct {
  uint32_t MemPtr;
  uint32_t MemFd;
  uint32_t DmaBuf;
  uint32_t Id;
} SpaTypeData;

static inline void
spa_type_data_map (SpaTypeMap *map, SpaTypeData *type)
{
  if (type->MemPtr == 0) {
    type->MemPtr    = spa_type_map_get_id (map, SPA_TYPE_DATA__MemPtr);
    type->MemFd     = spa_type_map_get_id (map, SPA_TYPE_DATA__MemFd);
    type->DmaBuf    = spa_type_map_get_id (map, SPA_TYPE_DATA__DmaBuf);
    type->Id        = spa_type_map_get_id (map, SPA_TYPE_DATA__Id);
  }
}

/**
 * SpaChunk:
 * @offset: offset of valid data
 * @size: size of valid data
 * @stride: stride of data if applicable
 */
typedef struct {
  uint32_t       offset;
  uint32_t       size;
  int32_t        stride;
} SpaChunk;

/**
 * SpaData:
 * @type: memory type
 * @flags: memory flags
 * @fd: file descriptor
 * @mapoffset: start offset when mapping @fd
 * @maxsize: maximum size of the memory
 * @data: pointer to memory
 * @chunk: pointer to chunk with valid offset
 */
typedef struct {
  uint32_t       type;
  uint32_t       flags;
  int            fd;
  uint32_t       mapoffset;
  uint32_t       maxsize;
  void          *data;
  SpaChunk      *chunk;
} SpaData;

/**
 * SpaBuffer:
 * @id: buffer id
 * @n_metas: number of metadata
 * @metas: offset of array of @n_metas metadata
 * @n_datas: number of data pointers
 * @datas: offset of array of @n_datas data pointers
 */
struct _SpaBuffer {
  uint32_t       id;
  uint32_t       n_metas;
  SpaMeta       *metas;
  uint32_t       n_datas;
  SpaData       *datas;
};

static inline void *
spa_buffer_find_meta (SpaBuffer *b, uint32_t type)
{
  uint32_t i;

  for (i = 0; i < b->n_metas; i++)
    if (b->metas[i].type == type)
      return b->metas[i].data;

  return NULL;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_BUFFER_H__ */
