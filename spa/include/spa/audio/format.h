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

typedef struct _SpaAudioRawFormat SpaAudioRawFormat;

typedef enum {
  SPA_PROP_ID_AUDIO_FORMAT = SPA_PROP_ID_MEDIA_CUSTOM_START,
  SPA_PROP_ID_AUDIO_FLAGS,
  SPA_PROP_ID_AUDIO_LAYOUT,
  SPA_PROP_ID_AUDIO_RATE,
  SPA_PROP_ID_AUDIO_CHANNELS,
  SPA_PROP_ID_AUDIO_CHANNEL_MASK,
  SPA_PROP_ID_AUDIO_RAW_INFO,
} SpaPropIdAudio;

struct _SpaAudioRawFormat {
  SpaFormat format;
  SpaAudioRawInfo info;
};

SpaResult   spa_audio_raw_format_init    (SpaAudioRawFormat *format);
SpaResult   spa_audio_raw_format_parse   (const SpaFormat *format,
                                          SpaAudioRawFormat *rawformat);


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_AUDIO_FORMAT */
