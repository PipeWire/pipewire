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

#include <spa/audio/raw.h>
#include <spa/audio/format.h>

typedef struct {
  uint32_t  key;
  uint32_t  type;
  off_t     offset;
} ParseInfo;

static const ParseInfo raw_parse_info[] = {
  { SPA_PROP_ID_AUDIO_INFO,               SPA_POD_TYPE_BYTES,     offsetof (SpaAudioInfo, info.raw) },
  { SPA_PROP_ID_AUDIO_FORMAT,             SPA_POD_TYPE_INT,       offsetof (SpaAudioInfo, info.raw.format) },
  { SPA_PROP_ID_AUDIO_FLAGS,              SPA_POD_TYPE_INT,       offsetof (SpaAudioInfo, info.raw.flags) },
  { SPA_PROP_ID_AUDIO_LAYOUT,             SPA_POD_TYPE_INT,       offsetof (SpaAudioInfo, info.raw.layout) },
  { SPA_PROP_ID_AUDIO_RATE,               SPA_POD_TYPE_INT,       offsetof (SpaAudioInfo, info.raw.rate) },
  { SPA_PROP_ID_AUDIO_CHANNELS,           SPA_POD_TYPE_INT,       offsetof (SpaAudioInfo, info.raw.channels) },
  { SPA_PROP_ID_AUDIO_CHANNEL_MASK,       SPA_POD_TYPE_INT,       offsetof (SpaAudioInfo, info.raw.channel_mask) },
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
spa_format_audio_parse (const SpaFormat *format,
                        SpaAudioInfo    *info)
{
  SpaPODProp *prop;
  const ParseInfo *pinfo, *find;

  if (format->body.media_type != SPA_MEDIA_TYPE_AUDIO)
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  switch (format->body.media_subtype) {
    case SPA_MEDIA_SUBTYPE_RAW:
      pinfo = raw_parse_info;
      break;
    case SPA_MEDIA_SUBTYPE_MP3:
    case SPA_MEDIA_SUBTYPE_AAC:
    case SPA_MEDIA_SUBTYPE_VORBIS:
    case SPA_MEDIA_SUBTYPE_WMA:
    case SPA_MEDIA_SUBTYPE_RA:
    case SPA_MEDIA_SUBTYPE_SBC:
    case SPA_MEDIA_SUBTYPE_ADPCM:
    case SPA_MEDIA_SUBTYPE_G723:
    case SPA_MEDIA_SUBTYPE_G726:
    case SPA_MEDIA_SUBTYPE_G729:
    case SPA_MEDIA_SUBTYPE_AMR:
    case SPA_MEDIA_SUBTYPE_GSM:
      return SPA_RESULT_NOT_IMPLEMENTED;

    default:
      return SPA_RESULT_INVALID_ARGUMENTS;
  }

  info->media_type = format->body.media_type;
  info->media_subtype = format->body.media_subtype;

  SPA_POD_FOREACH (format, prop) {
    if ((find = parse_info_find (pinfo, prop->body.key, prop->body.value.type))) {
      memcpy (SPA_MEMBER (info, find->offset, void),
              SPA_POD_BODY (&prop->body.value),
              SPA_POD_BODY_SIZE (&prop->body.value));
    }
  }
  return SPA_RESULT_OK;
}
