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

#ifndef __SPA_AUDIO_FORMAT_UTILS_H__
#define __SPA_AUDIO_FORMAT_UTILS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/format-utils.h>
#include <spa/audio/format.h>
#include <spa/audio/raw-utils.h>

struct spa_type_format_audio {
  uint32_t format;
  uint32_t flags;
  uint32_t layout;
  uint32_t rate;
  uint32_t channels;
  uint32_t channel_mask;
};

static inline void
spa_type_format_audio_map (struct spa_type_map *map, struct spa_type_format_audio *type)
{
  if (type->format == 0) {
    type->format = spa_type_map_get_id (map, SPA_TYPE_FORMAT_AUDIO__format);
    type->flags = spa_type_map_get_id (map, SPA_TYPE_FORMAT_AUDIO__flags);
    type->layout = spa_type_map_get_id (map, SPA_TYPE_FORMAT_AUDIO__layout);
    type->rate = spa_type_map_get_id (map, SPA_TYPE_FORMAT_AUDIO__rate);
    type->channels = spa_type_map_get_id (map, SPA_TYPE_FORMAT_AUDIO__channels);
    type->channel_mask = spa_type_map_get_id (map, SPA_TYPE_FORMAT_AUDIO__channelMask);
  }
}

static inline bool
spa_format_audio_raw_parse (const struct spa_format      *format,
                            struct spa_audio_info_raw    *info,
                            struct spa_type_format_audio *type)
{
  spa_format_query (format,
      type->format,       SPA_POD_TYPE_ID,  &info->format,
      type->flags,        SPA_POD_TYPE_INT, &info->flags,
      type->layout,       SPA_POD_TYPE_INT, &info->layout,
      type->rate,         SPA_POD_TYPE_INT, &info->rate,
      type->channels,     SPA_POD_TYPE_INT, &info->channels,
      type->channel_mask, SPA_POD_TYPE_INT, &info->channel_mask,
      0);
  return true;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_AUDIO_FORMAT_UTILS */
