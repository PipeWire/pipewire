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

#ifndef __SPA_PORT_H__
#define __SPA_PORT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaAllocParam SpaAllocParam;

#include <spa/defs.h>
#include <spa/buffer.h>
#include <spa/dict.h>
#include <spa/pod-utils.h>

#define SPA_ALLOC_PARAM_URI             "http://spaplug.in/ns/alloc-param"
#define SPA_ALLOC_PARAM_URI_PREFIX      SPA_ALLOC_PARAM_URI "-"

/**
 * SpaAllocParamType:
 * @SPA_ALLOC_PARAM_TYPE_INVALID: invalid type, should be ignored
 * @SPA_ALLOC_PARAM_TYPE_BUFFER: buffer requirements
 * @SPA_ALLOC_PARAM_TYPE_META_ENABLE: enable a certain metadata on buffers
 * @SPA_ALLOC_PARAM_TYPE_VIDEO_PADDING: do specialized video padding
 */
typedef enum {
  SPA_ALLOC_PARAM_TYPE_INVALID,
  SPA_ALLOC_PARAM_TYPE_BUFFERS,
  SPA_ALLOC_PARAM_TYPE_META_ENABLE,
  SPA_ALLOC_PARAM_TYPE_VIDEO_PADDING,
} SpaAllocParamType;

typedef struct {
  SpaPODObjectBody body;
  /* SpaPODProp follow */
} SpaAllocParamBody;

struct _SpaAllocParam {
  SpaPOD            pod;
  SpaAllocParamBody body;
};

static inline uint32_t
spa_alloc_param_query (const SpaAllocParam *param, uint32_t key, ...)
{
  uint32_t count;
  va_list args;

  va_start (args, key);
  count = spa_pod_contents_queryv (&param->pod, sizeof (SpaAllocParam), key, args);
  va_end (args);

  return count;
}

#define SPA_ALLOC_PARAM_BUFFERS          SPA_ALLOC_PARAM_URI_PREFIX "buffers"
#define SPA_ALLOC_PARAM_BUFFERS_PREFIX   SPA_ALLOC_PARAM_BUFFERS "#"

#define SPA_ALLOC_PARAM_BUFFERS__size      SPA_ALLOC_PARAM_BUFFERS_PREFIX "size"
#define SPA_ALLOC_PARAM_BUFFERS__stride    SPA_ALLOC_PARAM_BUFFERS_PREFIX "stride"
#define SPA_ALLOC_PARAM_BUFFERS__buffers   SPA_ALLOC_PARAM_BUFFERS_PREFIX "buffers"
#define SPA_ALLOC_PARAM_BUFFERS__align     SPA_ALLOC_PARAM_BUFFERS_PREFIX "align"

typedef enum {
  SPA_ALLOC_PARAM_BUFFERS_SIZE = 1,
  SPA_ALLOC_PARAM_BUFFERS_STRIDE,
  SPA_ALLOC_PARAM_BUFFERS_BUFFERS,
  SPA_ALLOC_PARAM_BUFFERS_ALIGN,
} SpaAllocParamBuffersKey;

#define SPA_ALLOC_PARAM_META_ENABLE         SPA_ALLOC_PARAM_URI_PREFIX "meta-enable"
#define SPA_ALLOC_PARAM_META_ENABLE_PREFIX  SPA_ALLOC_PARAM_META_ENABLE "#"
#define SPA_ALLOC_PARAM_META_ENABLE__type   SPA_ALLOC_PARAM_META_ENABLE_PREFIX "type"

#define SPA_ALLOC_PARAM_META_ENABLE__ringbufferSize   SPA_ALLOC_PARAM_META_ENABLE_PREFIX "ringbufferSize"
#define SPA_ALLOC_PARAM_META_ENABLE__ringbufferStride SPA_ALLOC_PARAM_META_ENABLE_PREFIX "ringbufferStride"
#define SPA_ALLOC_PARAM_META_ENABLE__ringbufferBlocks SPA_ALLOC_PARAM_META_ENABLE_PREFIX "ringbufferBlocks"
#define SPA_ALLOC_PARAM_META_ENABLE__ringbufferAlign  SPA_ALLOC_PARAM_META_ENABLE_PREFIX "ringbufferAlign"

typedef enum {
  SPA_ALLOC_PARAM_META_ENABLE_TYPE = 1,
  SPA_ALLOC_PARAM_META_ENABLE_RB_SIZE,
  SPA_ALLOC_PARAM_META_ENABLE_RB_STRIDE,
  SPA_ALLOC_PARAM_META_ENABLE_RB_BLOCKS,
  SPA_ALLOC_PARAM_META_ENABLE_RB_ALIGN,
} SpaAllocParamMetaEnableKey;

#define SPA_ALLOC_PARAM_VIDEO_PADDING         SPA_ALLOC_PARAM_URI_PREFIX "video-padding"
#define SPA_ALLOC_PARAM_VIDEO_PADDING_PREFIX  SPA_ALLOC_PARAM_VIDEO_PADDING "#"

#define SPA_ALLOC_PARAM_VIDEO_PADDING__top            SPA_ALLOC_PARAM_VIDEO_PADDING_PREFIX "top"
#define SPA_ALLOC_PARAM_VIDEO_PADDING__bottom         SPA_ALLOC_PARAM_VIDEO_PADDING_PREFIX "bottom"
#define SPA_ALLOC_PARAM_VIDEO_PADDING__left           SPA_ALLOC_PARAM_VIDEO_PADDING_PREFIX "left"
#define SPA_ALLOC_PARAM_VIDEO_PADDING__right          SPA_ALLOC_PARAM_VIDEO_PADDING_PREFIX "right"
#define SPA_ALLOC_PARAM_VIDEO_PADDING__strideAlign0   SPA_ALLOC_PARAM_VIDEO_PADDING_PREFIX "strideAlign0"
#define SPA_ALLOC_PARAM_VIDEO_PADDING__strideAlign1   SPA_ALLOC_PARAM_VIDEO_PADDING_PREFIX "strideAlign1"
#define SPA_ALLOC_PARAM_VIDEO_PADDING__strideAlign2   SPA_ALLOC_PARAM_VIDEO_PADDING_PREFIX "strideAlign2"
#define SPA_ALLOC_PARAM_VIDEO_PADDING__strideAlign3   SPA_ALLOC_PARAM_VIDEO_PADDING_PREFIX "strideAlign3"

typedef enum {
  SPA_ALLOC_PARAM_VIDEO_PADDING_TOP = 1,
  SPA_ALLOC_PARAM_VIDEO_PADDING_BOTTOM,
  SPA_ALLOC_PARAM_VIDEO_PADDING_LEFT,
  SPA_ALLOC_PARAM_VIDEO_PADDING_RIGHT,
  SPA_ALLOC_PARAM_VIDEO_PADDING_STRIDE_ALIGN0,
  SPA_ALLOC_PARAM_VIDEO_PADDING_STRIDE_ALIGN1,
  SPA_ALLOC_PARAM_VIDEO_PADDING_STRIDE_ALIGN2,
  SPA_ALLOC_PARAM_VIDEO_PADDING_STRIDE_ALIGN3,
} SpaAllocParamVideoPaddingKey;

/**
 * SpaPortInfoFlags:
 * @SPA_PORT_INFO_FLAG_NONE: no flags
 * @SPA_PORT_INFO_FLAG_REMOVABLE: port can be removed
 * @SPA_PORT_INFO_FLAG_OPTIONAL: processing on port is optional
 * @SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS: the port can give a buffer
 * @SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS: the port can use a provided buffer
 * @SPA_PORT_INFO_FLAG_IN_PLACE: the port can process data in-place and will need
 *    a writable input buffer
 * @SPA_PORT_INFO_FLAG_NO_REF: the port does not keep a ref on the buffer
 * @SPA_PORT_INFO_FLAG_LIVE: output buffers from this port are timestamped against
 *                           a live clock.
 */
typedef enum {
  SPA_PORT_INFO_FLAG_NONE                  = 0,
  SPA_PORT_INFO_FLAG_REMOVABLE             = 1 << 0,
  SPA_PORT_INFO_FLAG_OPTIONAL              = 1 << 1,
  SPA_PORT_INFO_FLAG_CAN_ALLOC_BUFFERS     = 1 << 2,
  SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS       = 1 << 3,
  SPA_PORT_INFO_FLAG_IN_PLACE              = 1 << 4,
  SPA_PORT_INFO_FLAG_NO_REF                = 1 << 5,
  SPA_PORT_INFO_FLAG_LIVE                  = 1 << 6,
} SpaPortInfoFlags;

/**
 * SpaPortInfo
 * @flags: extra port flags
 * @maxbuffering: the maximum amount of bytes that the element will keep
 *                around internally
 * @latency: latency on this port in nanoseconds
 * @params: extra allocation parameters
 * @n_params: number of elements in @params;
 * @extra: a dictionary of extra port info
 *
 */
typedef struct {
  SpaPortInfoFlags    flags;
  uint64_t            maxbuffering;
  uint64_t            latency;
  SpaAllocParam     **params;
  uint32_t            n_params;
  SpaDict            *extra;
} SpaPortInfo;

#ifdef __cplusplus
}  /* extern "C" */
#endif


#endif /* __SPA_PORT_H__ */
