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

typedef enum {
  SPA_MEDIA_TYPE_INVALID         = 0,
  SPA_MEDIA_TYPE_AUDIO           = 1,
  SPA_MEDIA_TYPE_VIDEO           = 2,
} SpaMediaType;

typedef enum {
  SPA_MEDIA_SUBTYPE_INVALID         = 0,
  SPA_MEDIA_SUBTYPE_RAW             = 1,
} SpaMediaSubType;

struct _SpaFormat {
  SpaProps         props;
  SpaMediaType     media_type;
  SpaMediaSubType  media_subtype;
};

typedef enum {
  SPA_PROP_ID_INVALID            = 0,
  SPA_PROP_ID_MEDIA_CUSTOM_START = 200,
} SpaFormatProps;

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_FORMAT_H__ */
