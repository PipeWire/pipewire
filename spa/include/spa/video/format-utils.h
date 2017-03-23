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
} SpaPropVideo;

static inline void
spa_prop_video_map (SpaIDMap *map, SpaPropVideo *types)
{
  if (types->format == 0) {
    types->format = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__format);
    types->size = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__size);
    types->framerate = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__framerate);
    types->max_framerate = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__maxFramerate);
    types->views = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__views);
    types->interlace_mode = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__interlaceMode);
    types->pixel_aspect_ratio = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__pixelAspectRatio);
    types->multiview_mode = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__multiviewMode);
    types->multiview_flags = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__multiviewFlags);
    types->chroma_site = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__chromaSite);
    types->color_range = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__colorRange);
    types->color_matrix = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__colorMatrix);
    types->transfer_function = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__transferFunction);
    types->color_primaries = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__colorPrimaries);
    types->profile = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__profile);
    types->level = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__level);
    types->stream_format = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__streamFormat);
    types->alignment = spa_id_map_get_id (map, SPA_TYPE_PROP_VIDEO__alignment);
  }
}

SpaResult   spa_format_video_parse      (const SpaFormat  *format,
                                         SpaVideoInfo     *info);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_VIDEO_FORMAT_UTILS */
