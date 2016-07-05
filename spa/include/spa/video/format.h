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

#ifndef __SPA_VIDEO_FORMAT_H__
#define __SPA_VIDEO_FORMAT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/format.h>
#include <spa/video/raw.h>

typedef struct _SpaVideoRawFormat SpaVideoRawFormat;

typedef enum {
  SPA_PROP_ID_VIDEO_FORMAT = SPA_PROP_ID_MEDIA_CUSTOM_START,
  SPA_PROP_ID_VIDEO_WIDTH,
  SPA_PROP_ID_VIDEO_HEIGHT,
  SPA_PROP_ID_VIDEO_FRAMERATE,
  SPA_PROP_ID_VIDEO_MAX_FRAMERATE,
  SPA_PROP_ID_VIDEO_VIEWS,
  SPA_PROP_ID_VIDEO_INTERLACE_MODE,
  SPA_PROP_ID_VIDEO_PIXEL_ASPECT_RATIO,
  SPA_PROP_ID_VIDEO_MULTIVIEW_MODE,
  SPA_PROP_ID_VIDEO_MULTIVIEW_FLAGS,
  SPA_PROP_ID_VIDEO_CHROMA_SITE,
  SPA_PROP_ID_VIDEO_COLOR_RANGE,
  SPA_PROP_ID_VIDEO_COLOR_MATRIX,
  SPA_PROP_ID_VIDEO_TRANSFER_FUNCTION,
  SPA_PROP_ID_VIDEO_COLOR_PRIMARIES,
  SPA_PROP_ID_VIDEO_RAW_INFO,
} SpaPropIdVideo;

struct _SpaVideoRawFormat {
  SpaFormat format;
  uint32_t unset_mask;
  SpaVideoRawInfo info;
};

SpaResult   spa_video_raw_format_init    (SpaVideoRawFormat *format);
SpaResult   spa_video_raw_format_parse   (const SpaFormat *format,
                                          SpaVideoRawFormat *rawformat);


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_VIDEO_FORMAT */
