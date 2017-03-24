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

#include <spa/format-builder.h>
#include <spa/video/format-utils.h>
#include <lib/props.h>
#include <lib/mapper.h>

SpaResult
spa_format_video_parse (const SpaFormat       *format,
                        SpaVideoInfo          *info)
{
  static SpaTypeMediaType media_type = { 0, };
  static SpaTypeMediaSubtype media_subtype = { 0, };
  static SpaTypeMediaSubtypeVideo media_subtype_video = { 0, };
  static SpaTypeFormatVideo format_video = { 0, };
  SpaTypeMap *map = spa_type_map_get_default ();

  spa_type_media_type_map (map, &media_type);
  spa_type_media_subtype_map (map, &media_subtype);
  spa_type_media_subtype_video_map (map, &media_subtype_video);
  spa_type_format_video_map (map, &format_video);

  if (format->body.media_type.value != media_type.video)
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  info->media_type = format->body.media_type.value;
  info->media_subtype = format->body.media_subtype.value;

  if (info->media_subtype == media_subtype.raw)
    spa_format_query (format,
        format_video.format,             SPA_POD_TYPE_ID,        &info->info.raw.format,
        format_video.size,               SPA_POD_TYPE_RECTANGLE, &info->info.raw.size,
        format_video.framerate,          SPA_POD_TYPE_FRACTION,  &info->info.raw.framerate,
        format_video.max_framerate,      SPA_POD_TYPE_FRACTION,  &info->info.raw.max_framerate,
        format_video.views,              SPA_POD_TYPE_INT,       &info->info.raw.views,
        format_video.interlace_mode,     SPA_POD_TYPE_INT,       &info->info.raw.interlace_mode,
        format_video.pixel_aspect_ratio, SPA_POD_TYPE_FRACTION,  &info->info.raw.pixel_aspect_ratio,
        format_video.multiview_mode,     SPA_POD_TYPE_INT,       &info->info.raw.multiview_mode,
        format_video.multiview_flags,    SPA_POD_TYPE_INT,       &info->info.raw.multiview_flags,
        format_video.chroma_site,        SPA_POD_TYPE_INT,       &info->info.raw.chroma_site,
        format_video.color_range,        SPA_POD_TYPE_INT,       &info->info.raw.color_range,
        format_video.color_matrix,       SPA_POD_TYPE_INT,       &info->info.raw.color_matrix,
        format_video.transfer_function,  SPA_POD_TYPE_INT,       &info->info.raw.transfer_function,
        format_video.color_primaries,    SPA_POD_TYPE_INT,       &info->info.raw.color_primaries,
        0);
  else if (info->media_subtype == media_subtype_video.h264)
    spa_format_query (format,
        format_video.size,               SPA_POD_TYPE_RECTANGLE, &info->info.h264.size,
        format_video.framerate,          SPA_POD_TYPE_FRACTION,  &info->info.h264.framerate,
        format_video.max_framerate,      SPA_POD_TYPE_FRACTION,  &info->info.h264.max_framerate,
        0);
  else if (info->media_subtype == media_subtype_video.mjpg)
    spa_format_query (format,
        format_video.size,               SPA_POD_TYPE_RECTANGLE, &info->info.mjpg.size,
        format_video.framerate,          SPA_POD_TYPE_FRACTION,  &info->info.mjpg.framerate,
        format_video.max_framerate,      SPA_POD_TYPE_FRACTION,  &info->info.mjpg.max_framerate,
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

  spa_pod_builder_push_format (result, &f, filter->body.obj_body.type,
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
