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

#ifndef __SPA_FORMAT_H__
#define __SPA_FORMAT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaFormat SpaFormat;

#include <spa/defs.h>
#include <spa/props.h>
#include <spa/memory.h>

typedef enum {
  SPA_MEDIA_TYPE_INVALID         = 0,
  SPA_MEDIA_TYPE_AUDIO           = 1,
  SPA_MEDIA_TYPE_VIDEO           = 2,
  SPA_MEDIA_TYPE_IMAGE           = 3,
} SpaMediaType;

typedef enum {
  SPA_MEDIA_SUBTYPE_INVALID         = 0,
  SPA_MEDIA_SUBTYPE_RAW             = 1,
  SPA_MEDIA_SUBTYPE_H264            = 2,
  SPA_MEDIA_SUBTYPE_MJPG            = 3,
  SPA_MEDIA_SUBTYPE_DV              = 4,
  SPA_MEDIA_SUBTYPE_MPEGTS          = 5,
  SPA_MEDIA_SUBTYPE_H263            = 6,
  SPA_MEDIA_SUBTYPE_MPEG1           = 7,
  SPA_MEDIA_SUBTYPE_MPEG2           = 8,
  SPA_MEDIA_SUBTYPE_MPEG4           = 9,
  SPA_MEDIA_SUBTYPE_XVID            = 10,
  SPA_MEDIA_SUBTYPE_VC1             = 11,
  SPA_MEDIA_SUBTYPE_VP8             = 12,
  SPA_MEDIA_SUBTYPE_VP9             = 13,
  SPA_MEDIA_SUBTYPE_JPEG            = 14,
  SPA_MEDIA_SUBTYPE_BAYER           = 15,
} SpaMediaSubType;

/**
 * SpaFormat:
 * @props: properties
 * @media_type: media type
 * @media_subtype: subtype
 * @mem: memory reference
 */
struct _SpaFormat {
  SpaProps         props;
  SpaMediaType     media_type;
  SpaMediaSubType  media_subtype;
  SpaMemoryRef     mem;
};

#define spa_format_ref(f)    spa_memory_ref(&(f)->mem)
#define spa_format_unref(f)  spa_memory_unref(&(f)->mem)

typedef enum {
  SPA_PROP_ID_INVALID            = 0,
  SPA_PROP_ID_MEDIA_CUSTOM_START = 200,
} SpaFormatProps;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_FORMAT_H__ */
