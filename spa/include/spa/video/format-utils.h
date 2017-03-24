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

#ifndef __SPA_VIDEO_FORMAT_UTILS_H__
#define __SPA_VIDEO_FORMAT_UTILS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/format-utils.h>
#include <spa/video/format.h>
#include <spa/video/raw-utils.h>

typedef struct {
  uint32_t format;
  uint32_t size;
  uint32_t framerate;
  uint32_t max_framerate;
  uint32_t views;
  uint32_t interlace_mode;
  uint32_t pixel_aspect_ratio;
  uint32_t multiview_mode;
  uint32_t multiview_flags;
  uint32_t chroma_site;
  uint32_t color_range;
  uint32_t color_matrix;
  uint32_t transfer_function;
  uint32_t color_primaries;
  uint32_t profile;
  uint32_t level;
  uint32_t stream_format;
  uint32_t alignment;
} SpaTypePropVideo;

static inline void
spa_type_prop_video_map (SpaTypeMap *map, SpaTypePropVideo *type)
{
  if (type->format == 0) {
    type->format               = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__format);
    type->size                 = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__size);
    type->framerate            = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__framerate);
    type->max_framerate        = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__maxFramerate);
    type->views                = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__views);
    type->interlace_mode       = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__interlaceMode);
    type->pixel_aspect_ratio   = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__pixelAspectRatio);
    type->multiview_mode       = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__multiviewMode);
    type->multiview_flags      = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__multiviewFlags);
    type->chroma_site          = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__chromaSite);
    type->color_range          = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__colorRange);
    type->color_matrix         = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__colorMatrix);
    type->transfer_function    = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__transferFunction);
    type->color_primaries      = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__colorPrimaries);
    type->profile              = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__profile);
    type->level                = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__level);
    type->stream_format        = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__streamFormat);
    type->alignment            = spa_type_map_get_id (map, SPA_TYPE_PROP_VIDEO__alignment);
  }
}

SpaResult   spa_format_video_parse      (const SpaFormat  *format,
                                         SpaVideoInfo     *info);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_VIDEO_FORMAT_UTILS */
