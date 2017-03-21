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

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <spa/video/raw.h>
#include <spa/video/format.h>
#include <spa/format-builder.h>
#include <lib/props.h>
#include <lib/mapper.h>

SpaResult
spa_format_video_parse (const SpaFormat       *format,
                        SpaVideoInfo          *info)
{
  static SpaMediaTypes media_types = { 0, };
  static SpaMediaSubtypes media_subtypes = { 0, };
  static SpaMediaSubtypesVideo media_subtypes_video = { 0, };
  static SpaPropVideo prop_video = { 0, };

  spa_media_types_fill (&media_types, spa_id_map_get_default ());
  spa_media_subtypes_map (spa_id_map_get_default (), &media_subtypes);
  spa_media_subtypes_video_map (spa_id_map_get_default (), &media_subtypes_video);
  spa_prop_video_map (spa_id_map_get_default (), &prop_video);

  if (format->body.media_type.value != media_types.video)
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  info->media_type = format->body.media_type.value;
  info->media_subtype = format->body.media_subtype.value;

  if (info->media_subtype == media_subtypes.raw)
    spa_format_query (format,
        prop_video.format,             SPA_POD_TYPE_INT,       &info->info.raw.format,
        prop_video.size,               SPA_POD_TYPE_RECTANGLE, &info->info.raw.size,
        prop_video.framerate,          SPA_POD_TYPE_FRACTION,  &info->info.raw.framerate,
        prop_video.max_framerate,      SPA_POD_TYPE_FRACTION,  &info->info.raw.max_framerate,
        prop_video.views,              SPA_POD_TYPE_INT,       &info->info.raw.views,
        prop_video.interlace_mode,     SPA_POD_TYPE_INT,       &info->info.raw.interlace_mode,
        prop_video.pixel_aspect_ratio, SPA_POD_TYPE_FRACTION,  &info->info.raw.pixel_aspect_ratio,
        prop_video.multiview_mode,     SPA_POD_TYPE_INT,       &info->info.raw.multiview_mode,
        prop_video.multiview_flags,    SPA_POD_TYPE_INT,       &info->info.raw.multiview_flags,
        prop_video.chroma_site,        SPA_POD_TYPE_INT,       &info->info.raw.chroma_site,
        prop_video.color_range,        SPA_POD_TYPE_INT,       &info->info.raw.color_range,
        prop_video.color_matrix,       SPA_POD_TYPE_INT,       &info->info.raw.color_matrix,
        prop_video.transfer_function,  SPA_POD_TYPE_INT,       &info->info.raw.transfer_function,
        prop_video.color_primaries,    SPA_POD_TYPE_INT,       &info->info.raw.color_primaries,
        0);
  else if (info->media_subtype == media_subtypes_video.h264)
    spa_format_query (format,
        prop_video.size,               SPA_POD_TYPE_RECTANGLE, &info->info.h264.size,
        prop_video.framerate,          SPA_POD_TYPE_FRACTION,  &info->info.h264.framerate,
        prop_video.max_framerate,      SPA_POD_TYPE_FRACTION,  &info->info.h264.max_framerate,
        0);
  else if (info->media_subtype == media_subtypes_video.mjpg)
    spa_format_query (format,
        prop_video.size,               SPA_POD_TYPE_RECTANGLE, &info->info.mjpg.size,
        prop_video.framerate,          SPA_POD_TYPE_FRACTION,  &info->info.mjpg.framerate,
        prop_video.max_framerate,      SPA_POD_TYPE_FRACTION,  &info->info.mjpg.max_framerate,
        0);
  else
    return SPA_RESULT_NOT_IMPLEMENTED;

  return SPA_RESULT_OK;
}

SpaResult
spa_format_filter (const SpaFormat  *format,
                   const SpaFormat  *filter,
                   SpaPODBuilder    *result)
{
  SpaPODFrame f;
  SpaResult res;

  if (format == NULL || result == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  if (filter == NULL) {
    spa_pod_builder_raw_padded (result, format, SPA_POD_SIZE (format));
    return SPA_RESULT_OK;
  }

  if (filter->body.media_type.value != format->body.media_type.value ||
      filter->body.media_subtype.value != format->body.media_subtype.value)
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  spa_pod_builder_push_format (result, &f,
                               filter->body.media_type.value,
                               filter->body.media_subtype.value);
  res = spa_props_filter (result,
                          SPA_POD_CONTENTS (SpaFormat, format),
                          SPA_POD_CONTENTS_SIZE (SpaFormat, format),
                          SPA_POD_CONTENTS (SpaFormat, filter),
                          SPA_POD_CONTENTS_SIZE (SpaFormat, filter));
  spa_pod_builder_pop (result, &f);

  return res;
}
