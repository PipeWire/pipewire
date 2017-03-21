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

#include <lib/mapper.h>
#include <spa/audio/raw-utils.h>
#include <spa/audio/format-utils.h>

SpaResult
spa_format_audio_parse (const SpaFormat *format,
                        SpaAudioInfo    *info)
{
  static SpaMediaTypes media_types = { 0, };
  static SpaMediaSubtypes media_subtypes = { 0, };
  static SpaMediaSubtypesAudio media_subtypes_audio = { 0, };
  static SpaPropAudio prop_audio = { 0, };

  spa_media_types_fill (&media_types, spa_id_map_get_default());
  spa_media_subtypes_map (spa_id_map_get_default(), &media_subtypes);
  spa_media_subtypes_audio_map (spa_id_map_get_default (), &media_subtypes_audio);
  spa_prop_audio_map (spa_id_map_get_default (), &prop_audio);

  if (format->body.media_type.value != media_types.audio)
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  info->media_type = format->body.media_type.value;
  info->media_subtype = format->body.media_subtype.value;

  if (info->media_subtype == media_subtypes.raw) {
    spa_format_query (format,
        prop_audio.format,       SPA_POD_TYPE_URI, &info->info.raw.format,
        prop_audio.flags,        SPA_POD_TYPE_INT, &info->info.raw.flags,
        prop_audio.layout,       SPA_POD_TYPE_INT, &info->info.raw.layout,
        prop_audio.rate,         SPA_POD_TYPE_INT, &info->info.raw.rate,
        prop_audio.channels,     SPA_POD_TYPE_INT, &info->info.raw.channels,
        prop_audio.channel_mask, SPA_POD_TYPE_INT, &info->info.raw.channel_mask,
        0);
  }
  else
    return SPA_RESULT_NOT_IMPLEMENTED;

  return SPA_RESULT_OK;
}
