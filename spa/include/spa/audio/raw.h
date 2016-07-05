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

#ifndef __SPA_AUDIO_RAW_H__
#define __SPA_AUDIO_RAW_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _SpaAudioRawInfo SpaAudioRawInfo;

#include <endian.h>

#if __BYTE_ORDER == __BIG_ENDIAN
#define _SPA_AUDIO_FORMAT_NE(fmt) SPA_AUDIO_FORMAT_ ## fmt ## BE
#define _SPA_AUDIO_FORMAT_OE(fmt) SPA_AUDIO_FORMAT_ ## fmt ## LE
#elif __BYTE_ORDER == __LITTLE_ENDIAN
#define _SPA_AUDIO_FORMAT_NE(fmt) SPA_AUDIO_FORMAT_ ## fmt ## LE
#define _SPA_AUDIO_FORMAT_OE(fmt) SPA_AUDIO_FORMAT_ ## fmt ## BE
#endif

typedef enum {
  SPA_AUDIO_FORMAT_UNKNOWN,
  SPA_AUDIO_FORMAT_ENCODED,
  /* 8 bit */
  SPA_AUDIO_FORMAT_S8,
  SPA_AUDIO_FORMAT_U8,
  /* 16 bit */
  SPA_AUDIO_FORMAT_S16LE,
  SPA_AUDIO_FORMAT_S16BE,
  SPA_AUDIO_FORMAT_U16LE,
  SPA_AUDIO_FORMAT_U16BE,
  /* 24 bit in low 3 bytes of 32 bits*/
  SPA_AUDIO_FORMAT_S24_32LE,
  SPA_AUDIO_FORMAT_S24_32BE,
  SPA_AUDIO_FORMAT_U24_32LE,
  SPA_AUDIO_FORMAT_U24_32BE,
  /* 32 bit */
  SPA_AUDIO_FORMAT_S32LE,
  SPA_AUDIO_FORMAT_S32BE,
  SPA_AUDIO_FORMAT_U32LE,
  SPA_AUDIO_FORMAT_U32BE,
  /* 24 bit in 3 bytes*/
  SPA_AUDIO_FORMAT_S24LE,
  SPA_AUDIO_FORMAT_S24BE,
  SPA_AUDIO_FORMAT_U24LE,
  SPA_AUDIO_FORMAT_U24BE,
  /* 20 bit in 3 bytes*/
  SPA_AUDIO_FORMAT_S20LE,
  SPA_AUDIO_FORMAT_S20BE,
  SPA_AUDIO_FORMAT_U20LE,
  SPA_AUDIO_FORMAT_U20BE,
  /* 18 bit in 3 bytes*/
  SPA_AUDIO_FORMAT_S18LE,
  SPA_AUDIO_FORMAT_S18BE,
  SPA_AUDIO_FORMAT_U18LE,
  SPA_AUDIO_FORMAT_U18BE,
  /* float */
  SPA_AUDIO_FORMAT_F32LE,
  SPA_AUDIO_FORMAT_F32BE,
  SPA_AUDIO_FORMAT_F64LE,
  SPA_AUDIO_FORMAT_F64BE,
  /* native endianness equivalents */
  SPA_AUDIO_FORMAT_S16 = _SPA_AUDIO_FORMAT_NE(S16),
  SPA_AUDIO_FORMAT_U16 = _SPA_AUDIO_FORMAT_NE(U16),
  SPA_AUDIO_FORMAT_S24_32 = _SPA_AUDIO_FORMAT_NE(S24_32),
  SPA_AUDIO_FORMAT_U24_32 = _SPA_AUDIO_FORMAT_NE(U24_32),
  SPA_AUDIO_FORMAT_S32 = _SPA_AUDIO_FORMAT_NE(S32),
  SPA_AUDIO_FORMAT_U32 = _SPA_AUDIO_FORMAT_NE(U32),
  SPA_AUDIO_FORMAT_S24 = _SPA_AUDIO_FORMAT_NE(S24),
  SPA_AUDIO_FORMAT_U24 = _SPA_AUDIO_FORMAT_NE(U24),
  SPA_AUDIO_FORMAT_S20 = _SPA_AUDIO_FORMAT_NE(S20),
  SPA_AUDIO_FORMAT_U20 = _SPA_AUDIO_FORMAT_NE(U20),
  SPA_AUDIO_FORMAT_S18 = _SPA_AUDIO_FORMAT_NE(S18),
  SPA_AUDIO_FORMAT_U18 = _SPA_AUDIO_FORMAT_NE(U18),
  SPA_AUDIO_FORMAT_F32 = _SPA_AUDIO_FORMAT_NE(F32),
  SPA_AUDIO_FORMAT_F64 = _SPA_AUDIO_FORMAT_NE(F64)
} SpaAudioFormat;


/**
 * SpaAudioFlags:
 * @SPA_AUDIO_FLAG_NONE: no valid flag
 * @SPA_AUDIO_FLAG_UNPOSITIONED: the position array explicitly
 *     contains unpositioned channels.
 *
 * Extra audio flags
 */
typedef enum {
  SPA_AUDIO_FLAG_NONE              = 0,
  SPA_AUDIO_FLAG_UNPOSITIONED      = (1 << 0)
} SpaAudioFlags;

/**
 * SpaAudioLayout:
 * @SPA_AUDIO_LAYOUT_INTERLEAVED: interleaved audio
 * @SPA_AUDIO_LAYOUT_NON_INTERLEAVED: non-interleaved audio
 *
 * Layout of the audio samples for the different channels.
 */
typedef enum {
  SPA_AUDIO_LAYOUT_INTERLEAVED = 0,
  SPA_AUDIO_LAYOUT_NON_INTERLEAVED
} SpaAudioLayout;

/**
 * SpaAudioRawInfo:
 * @format: the format
 * @flags: extra flags
 * @layout: the sample layout
 * @rate: the sample rate
 * @channels: the number of channels
 * @channel_mask: the channel mask
 */
struct _SpaAudioRawInfo {
  SpaAudioFormat format;
  SpaAudioFlags  flags;
  SpaAudioLayout layout;
  uint32_t       rate;
  uint32_t       channels;
  uint32_t       channel_mask;
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_AUDIO_RAW_H__ */
