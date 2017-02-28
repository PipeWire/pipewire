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

typedef struct {
  uint32_t  key;
  uint32_t  type;
  off_t     offset;
} ParseInfo;

static const ParseInfo raw_parse_info[] = {
  { SPA_PROP_ID_VIDEO_INFO,               SPA_POD_TYPE_BYTES,     offsetof (SpaVideoInfo, info.raw) },
  { SPA_PROP_ID_VIDEO_FORMAT,             SPA_POD_TYPE_INT,       offsetof (SpaVideoInfo, info.raw.format) },
  { SPA_PROP_ID_VIDEO_SIZE,               SPA_POD_TYPE_RECTANGLE, offsetof (SpaVideoInfo, info.raw.size) },
  { SPA_PROP_ID_VIDEO_FRAMERATE,          SPA_POD_TYPE_FRACTION,  offsetof (SpaVideoInfo, info.raw.framerate) },
  { SPA_PROP_ID_VIDEO_MAX_FRAMERATE,      SPA_POD_TYPE_FRACTION,  offsetof (SpaVideoInfo, info.raw.max_framerate) },
  { SPA_PROP_ID_VIDEO_VIEWS,              SPA_POD_TYPE_INT,       offsetof (SpaVideoInfo, info.raw.views) },
  { SPA_PROP_ID_VIDEO_INTERLACE_MODE,     SPA_POD_TYPE_INT,       offsetof (SpaVideoInfo, info.raw.interlace_mode) },
  { SPA_PROP_ID_VIDEO_PIXEL_ASPECT_RATIO, SPA_POD_TYPE_FRACTION,  offsetof (SpaVideoInfo, info.raw.pixel_aspect_ratio) },
  { SPA_PROP_ID_VIDEO_MULTIVIEW_MODE,     SPA_POD_TYPE_INT,       offsetof (SpaVideoInfo, info.raw.multiview_mode) },
  { SPA_PROP_ID_VIDEO_MULTIVIEW_FLAGS,    SPA_POD_TYPE_INT,       offsetof (SpaVideoInfo, info.raw.multiview_flags) },
  { SPA_PROP_ID_VIDEO_CHROMA_SITE,        SPA_POD_TYPE_INT,       offsetof (SpaVideoInfo, info.raw.chroma_site) },
  { SPA_PROP_ID_VIDEO_COLOR_RANGE,        SPA_POD_TYPE_INT,       offsetof (SpaVideoInfo, info.raw.color_range) },
  { SPA_PROP_ID_VIDEO_COLOR_MATRIX,       SPA_POD_TYPE_INT,       offsetof (SpaVideoInfo, info.raw.color_matrix) },
  { SPA_PROP_ID_VIDEO_TRANSFER_FUNCTION,  SPA_POD_TYPE_INT,       offsetof (SpaVideoInfo, info.raw.transfer_function) },
  { SPA_PROP_ID_VIDEO_COLOR_PRIMARIES,    SPA_POD_TYPE_INT,       offsetof (SpaVideoInfo, info.raw.color_primaries) },
  { 0, }
};

static const ParseInfo h264_parse_info[] = {
  { SPA_PROP_ID_VIDEO_INFO,               SPA_POD_TYPE_BYTES,     offsetof (SpaVideoInfo, info.h264) },
  { SPA_PROP_ID_VIDEO_SIZE,               SPA_POD_TYPE_RECTANGLE, offsetof (SpaVideoInfo, info.h264.size) },
  { SPA_PROP_ID_VIDEO_FRAMERATE,          SPA_POD_TYPE_FRACTION,  offsetof (SpaVideoInfo, info.h264.framerate) },
  { SPA_PROP_ID_VIDEO_MAX_FRAMERATE,      SPA_POD_TYPE_FRACTION,  offsetof (SpaVideoInfo, info.h264.max_framerate) },
  { 0, }
};

static const ParseInfo mjpg_parse_info[] = {
  { SPA_PROP_ID_VIDEO_INFO,               SPA_POD_TYPE_BYTES,     offsetof (SpaVideoInfo, info.h264) },
  { SPA_PROP_ID_VIDEO_SIZE,               SPA_POD_TYPE_RECTANGLE, offsetof (SpaVideoInfo, info.h264.size) },
  { SPA_PROP_ID_VIDEO_FRAMERATE,          SPA_POD_TYPE_FRACTION,  offsetof (SpaVideoInfo, info.h264.framerate) },
  { SPA_PROP_ID_VIDEO_MAX_FRAMERATE,      SPA_POD_TYPE_FRACTION,  offsetof (SpaVideoInfo, info.h264.max_framerate) },
  { 0, }
};

static const ParseInfo *
parse_info_find (const ParseInfo *info, uint32_t key, uint32_t type)
{
  while (info->key) {
    if (info->key == key && info->type == type)
      return info;
    info++;
  }
  return NULL;
}

SpaResult
spa_format_video_parse (const SpaFormat *format,
                        SpaVideoInfo    *info)
{
  SpaPODProp *prop;
  const ParseInfo *pinfo, *find;

  if (format->body.media_type.value != SPA_MEDIA_TYPE_VIDEO)
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  info->media_type = format->body.media_type.value;
  info->media_subtype = format->body.media_subtype.value;

  switch (info->media_subtype) {
    case SPA_MEDIA_SUBTYPE_RAW:
      pinfo = raw_parse_info;
      break;
    case SPA_MEDIA_SUBTYPE_H264:
      pinfo = h264_parse_info;
      break;
    case SPA_MEDIA_SUBTYPE_MJPG:
      pinfo = mjpg_parse_info;
      break;
    case SPA_MEDIA_SUBTYPE_DV:
    case SPA_MEDIA_SUBTYPE_MPEGTS:
    case SPA_MEDIA_SUBTYPE_H263:
    case SPA_MEDIA_SUBTYPE_MPEG1:
    case SPA_MEDIA_SUBTYPE_MPEG2:
    case SPA_MEDIA_SUBTYPE_MPEG4:
    case SPA_MEDIA_SUBTYPE_XVID:
    case SPA_MEDIA_SUBTYPE_VC1:
    case SPA_MEDIA_SUBTYPE_VP8:
    case SPA_MEDIA_SUBTYPE_VP9:
    case SPA_MEDIA_SUBTYPE_JPEG:
    case SPA_MEDIA_SUBTYPE_BAYER:
      return SPA_RESULT_NOT_IMPLEMENTED;

    default:
      return SPA_RESULT_INVALID_ARGUMENTS;
  }

  SPA_FORMAT_FOREACH (format, prop) {
    if ((find = parse_info_find (pinfo, prop->body.key, prop->body.value.type))) {
      memcpy (SPA_MEMBER (info, find->offset, void),
              SPA_POD_BODY (&prop->body.value),
              SPA_POD_BODY_SIZE (&prop->body.value));
    }
  }
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
    spa_pod_builder_raw (result, format, SPA_POD_SIZE (format), true);
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
