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

#define SPA_ALLOC_PARAM_URI             "http://spaplug.in/ns/alloc-param"
#define SPA_ALLOC_PARAM_PREFIX          SPA_ALLOC_PARAM_URI "#"

#define SPA_ALLOC_PARAM__Buffers        SPA_ALLOC_PARAM_PREFIX "Buffers"
#define SPA_ALLOC_PARAM__MetaEnable     SPA_ALLOC_PARAM_PREFIX "MetaEnable"
#define SPA_ALLOC_PARAM__VideoPadding   SPA_ALLOC_PARAM_PREFIX "VideoPadding"

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

struct _SpaAllocParam {
  uint32_t  type;
  size_t    size;
};

typedef struct {
  SpaAllocParam  param;
  size_t         minsize;
  size_t         stride;
  uint32_t       min_buffers;
  uint32_t       max_buffers;
  uint32_t       align;
} SpaAllocParamBuffers;

typedef struct {
  SpaAllocParam  param;
  SpaMetaType    type;
} SpaAllocParamMetaEnable;

typedef struct {
  SpaAllocParam param;
  unsigned int  padding_top;
  unsigned int  padding_bottom;
  unsigned int  padding_left;
  unsigned int  padding_right;
  unsigned int  stride_align[4];
} SpaAllocParamVideoPadding;

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
 * @features: NULL terminated array of extra port features
 *
 */
typedef struct {
  SpaPortInfoFlags    flags;
  uint64_t            maxbuffering;
  uint64_t            latency;
  SpaAllocParam     **params;
  unsigned int        n_params;
  const char        **features;
} SpaPortInfo;


size_t          spa_port_info_get_size    (const SpaPortInfo *info);
size_t          spa_port_info_serialize   (void *dest, const SpaPortInfo *info);
SpaPortInfo *   spa_port_info_deserialize (void *src, off_t offset);

SpaPortInfo *   spa_port_info_copy_into   (void *dest, const SpaPortInfo *info);

/**
 * SpaPortStatusFlags:
 * @SPA_PORT_STATUS_FLAG_NONE: no status flags
 * @SPA_PORT_STATUS_FLAG_HAVE_FORMAT: port has a format
 * @SPA_PORT_STATUS_FLAG_HAVE_BUFFERS: port has buffers
 * @SPA_PORT_STATUS_FLAG_HAVE_OUTPUT: port has output
 * @SPA_PORT_STATUS_FLAG_NEED_INPUT: port needs input
 */
typedef enum {
  SPA_PORT_STATUS_FLAG_NONE                  = 0,
  SPA_PORT_STATUS_FLAG_HAVE_FORMAT           = 1 << 0,
  SPA_PORT_STATUS_FLAG_HAVE_BUFFERS          = 1 << 1,
  SPA_PORT_STATUS_FLAG_HAVE_OUTPUT           = 1 << 2,
  SPA_PORT_STATUS_FLAG_NEED_INPUT            = 1 << 3,
} SpaPortStatusFlags;

/**
 * SpaPortStatus:
 * @flags: port status flags
 */
typedef struct {
  SpaPortStatusFlags   flags;
} SpaPortStatus;

#ifdef __cplusplus
}  /* extern "C" */
#endif


#endif /* __SPA_PORT_H__ */
