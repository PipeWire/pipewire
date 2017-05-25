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

#ifndef __SPA_PARAM_ALLOC_H__
#define __SPA_PARAM_ALLOC_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/defs.h>
#include <spa/param.h>
#include <spa/type-map.h>

#define SPA_TYPE__ParamAlloc            SPA_TYPE_PARAM_BASE "Alloc"
#define SPA_TYPE_PARAM_ALLOC_BASE       SPA_TYPE__ParamAlloc ":"

#define SPA_TYPE_PARAM_ALLOC__Buffers          SPA_TYPE_PARAM_ALLOC_BASE "Buffers"
#define SPA_TYPE_PARAM_ALLOC_BUFFERS_BASE      SPA_TYPE_PARAM_ALLOC__Buffers ":"

#define SPA_TYPE_PARAM_ALLOC_BUFFERS__size      SPA_TYPE_PARAM_ALLOC_BUFFERS_BASE "size"
#define SPA_TYPE_PARAM_ALLOC_BUFFERS__stride    SPA_TYPE_PARAM_ALLOC_BUFFERS_BASE "stride"
#define SPA_TYPE_PARAM_ALLOC_BUFFERS__buffers   SPA_TYPE_PARAM_ALLOC_BUFFERS_BASE "buffers"
#define SPA_TYPE_PARAM_ALLOC_BUFFERS__align     SPA_TYPE_PARAM_ALLOC_BUFFERS_BASE "align"

struct spa_type_param_alloc_buffers {
  uint32_t Buffers;
  uint32_t size;
  uint32_t stride;
  uint32_t buffers;
  uint32_t align;
};

static inline void
spa_type_param_alloc_buffers_map (struct spa_type_map *map,
                                  struct spa_type_param_alloc_buffers *type)
{
  if (type->Buffers == 0) {
    type->Buffers  = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC__Buffers);
    type->size     = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_BUFFERS__size);
    type->stride   = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_BUFFERS__stride);
    type->buffers  = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_BUFFERS__buffers);
    type->align    = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_BUFFERS__align);
  }
}

#define SPA_TYPE_PARAM_ALLOC__MetaEnable         SPA_TYPE_PARAM_ALLOC_BASE "MetaEnable"
#define SPA_TYPE_PARAM_ALLOC_META_ENABLE_BASE    SPA_TYPE_PARAM_ALLOC__MetaEnable ":"
#define SPA_TYPE_PARAM_ALLOC_META_ENABLE__type   SPA_TYPE_PARAM_ALLOC_META_ENABLE_BASE "type"
#define SPA_TYPE_PARAM_ALLOC_META_ENABLE__size   SPA_TYPE_PARAM_ALLOC_META_ENABLE_BASE "size"

#define SPA_TYPE_PARAM_ALLOC_META_ENABLE__ringbufferSize   SPA_TYPE_PARAM_ALLOC_META_ENABLE_BASE "ringbufferSize"
#define SPA_TYPE_PARAM_ALLOC_META_ENABLE__ringbufferStride SPA_TYPE_PARAM_ALLOC_META_ENABLE_BASE "ringbufferStride"
#define SPA_TYPE_PARAM_ALLOC_META_ENABLE__ringbufferBlocks SPA_TYPE_PARAM_ALLOC_META_ENABLE_BASE "ringbufferBlocks"
#define SPA_TYPE_PARAM_ALLOC_META_ENABLE__ringbufferAlign  SPA_TYPE_PARAM_ALLOC_META_ENABLE_BASE "ringbufferAlign"

struct spa_type_param_alloc_meta_enable {
  uint32_t MetaEnable;
  uint32_t type;
  uint32_t size;
  uint32_t ringbufferSize;
  uint32_t ringbufferStride;
  uint32_t ringbufferBlocks;
  uint32_t ringbufferAlign;
};

static inline void
spa_type_param_alloc_meta_enable_map (struct spa_type_map *map,
                                      struct spa_type_param_alloc_meta_enable *type)
{
  if (type->MetaEnable == 0) {
    type->MetaEnable       = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC__MetaEnable);
    type->type             = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_META_ENABLE__type);
    type->size             = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_META_ENABLE__size);
    type->ringbufferSize   = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_META_ENABLE__ringbufferSize);
    type->ringbufferStride = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_META_ENABLE__ringbufferStride);
    type->ringbufferBlocks = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_META_ENABLE__ringbufferBlocks);
    type->ringbufferAlign  = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_META_ENABLE__ringbufferAlign);
  }
}

#define SPA_TYPE_PARAM_ALLOC__VideoPadding         SPA_TYPE_PARAM_ALLOC_BASE "VideoPadding"
#define SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING_BASE    SPA_TYPE_PARAM_ALLOC__VideoPadding ":"

#define SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__top            SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING_BASE "top"
#define SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__bottom         SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING_BASE "bottom"
#define SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__left           SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING_BASE "left"
#define SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__right          SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING_BASE "right"
#define SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__strideAlign0   SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING_BASE "strideAlign0"
#define SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__strideAlign1   SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING_BASE "strideAlign1"
#define SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__strideAlign2   SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING_BASE "strideAlign2"
#define SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__strideAlign3   SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING_BASE "strideAlign3"

struct spa_type_param_alloc_video_padding {
  uint32_t VideoPadding;
  uint32_t top;
  uint32_t bottom;
  uint32_t left;
  uint32_t right;
  uint32_t strideAlign[4];
};

static inline void
spa_type_param_alloc_video_padding_map (struct spa_type_map *map,
                                        struct spa_type_param_alloc_video_padding *type)
{
  if (type->VideoPadding == 0) {
    type->VideoPadding   = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC__VideoPadding);
    type->top            = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__top);
    type->bottom         = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__bottom);
    type->left           = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__left);
    type->right          = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__right);
    type->strideAlign[0] = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__strideAlign0);
    type->strideAlign[1] = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__strideAlign1);
    type->strideAlign[2] = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__strideAlign2);
    type->strideAlign[3] = spa_type_map_get_id (map, SPA_TYPE_PARAM_ALLOC_VIDEO_PADDING__strideAlign3);
  }
}

#ifdef __cplusplus
}  /* extern "C" */
#endif


#endif /* __SPA_PARAM_ALLOC_H__ */
