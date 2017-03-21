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

#ifndef __SPA_AUDIO_FORMAT_H__
#define __SPA_AUDIO_FORMAT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/format.h>
#include <spa/audio/raw.h>

typedef struct _SpaAudioInfo SpaAudioInfo;

#define SPA_PROP_AUDIO_URI           "http://spaplug.in/ns/prop-audio"
#define SPA_PROP_AUDIO_PREFIX        SPA_PROP_AUDIO_URI "#"

#define SPA_PROP_AUDIO__format          SPA_PROP_AUDIO_PREFIX "format"
#define SPA_PROP_AUDIO__flags           SPA_PROP_AUDIO_PREFIX "flags"
#define SPA_PROP_AUDIO__layout          SPA_PROP_AUDIO_PREFIX "layout"
#define SPA_PROP_AUDIO__rate            SPA_PROP_AUDIO_PREFIX "rate"
#define SPA_PROP_AUDIO__channels        SPA_PROP_AUDIO_PREFIX "channels"
#define SPA_PROP_AUDIO__channelMask     SPA_PROP_AUDIO_PREFIX "channel-mask"

typedef struct {
  uint32_t format;
  uint32_t flags;
  uint32_t layout;
  uint32_t rate;
  uint32_t channels;
  uint32_t channel_mask;
} SpaPropAudio;

static inline void
spa_prop_audio_map (SpaIDMap *map, SpaPropAudio *types)
{
  if (types->format == 0) {
    types->format = spa_id_map_get_id (map, SPA_PROP_AUDIO__format);
    types->flags = spa_id_map_get_id (map, SPA_PROP_AUDIO__flags);
    types->layout = spa_id_map_get_id (map, SPA_PROP_AUDIO__layout);
    types->rate = spa_id_map_get_id (map, SPA_PROP_AUDIO__rate);
    types->channels = spa_id_map_get_id (map, SPA_PROP_AUDIO__channels);
    types->channel_mask = spa_id_map_get_id (map, SPA_PROP_AUDIO__channelMask);
  }
}

struct _SpaAudioInfo {
  uint32_t media_type;
  uint32_t media_subtype;
  union {
    SpaAudioInfoRaw raw;
  } info;
};

SpaResult   spa_format_audio_parse       (const SpaFormat *format,
                                          SpaAudioInfo    *info);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_AUDIO_FORMAT */
