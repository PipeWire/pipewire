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

static const uint32_t format_values[] = {
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
  SPA_AUDIO_FORMAT_F64BE
};

static const SpaPropRangeInfo format_format_range[] = {
  { "S8", { sizeof (uint32_t), &format_values[0] } },
  { "U8", { sizeof (uint32_t), &format_values[1] } },
  { "S16LE", { sizeof (uint32_t), &format_values[2] } },
  { "S16BE", { sizeof (uint32_t), &format_values[3] } },
  { "U16LE", { sizeof (uint32_t), &format_values[4] } },
  { "U16BE", { sizeof (uint32_t), &format_values[5] } },
  { "S24_32LE", { sizeof (uint32_t), &format_values[6] } },
  { "S24_32BE", { sizeof (uint32_t), &format_values[7] } },
  { "U24_32LE", { sizeof (uint32_t), &format_values[8] } },
  { "U24_32BE", { sizeof (uint32_t), &format_values[9] } },
  { "S32LE", { sizeof (uint32_t), &format_values[10] } },
  { "S32BE", { sizeof (uint32_t), &format_values[11] } },
  { "U32LE", { sizeof (uint32_t), &format_values[12] } },
  { "U32BE", { sizeof (uint32_t), &format_values[13] } },
  { "S24LE", { sizeof (uint32_t), &format_values[14] } },
  { "S24BE", { sizeof (uint32_t), &format_values[15] } },
  { "U24LE", { sizeof (uint32_t), &format_values[16] } },
  { "U24BE", { sizeof (uint32_t), &format_values[17] } },
  { "S20LE", { sizeof (uint32_t), &format_values[18] } },
  { "S20BE", { sizeof (uint32_t), &format_values[19] } },
  { "U20LE", { sizeof (uint32_t), &format_values[20] } },
  { "U20BE", { sizeof (uint32_t), &format_values[21] } },
  { "S18LE", { sizeof (uint32_t), &format_values[22] } },
  { "S18BE", { sizeof (uint32_t), &format_values[23] } },
  { "U18LE", { sizeof (uint32_t), &format_values[24] } },
  { "U18BE", { sizeof (uint32_t), &format_values[25] } },
  { "F32LE", { sizeof (uint32_t), &format_values[26] } },
  { "F32BE", { sizeof (uint32_t), &format_values[27] } },
  { "F64LE", { sizeof (uint32_t), &format_values[28] } },
  { "F64BE", { sizeof (uint32_t), &format_values[29] } },
};

static const uint32_t format_layouts[] = {
  SPA_AUDIO_LAYOUT_INTERLEAVED,
  SPA_AUDIO_LAYOUT_NON_INTERLEAVED,
};

static const SpaPropRangeInfo layouts_range[] = {
  { "interleaved", { sizeof (uint32_t), &format_layouts[0] } },
  { "non-interleaved", { sizeof (uint32_t), &format_layouts[1] } },
};

static const uint32_t format_flags[] = {
  SPA_AUDIO_FLAG_NONE,
  SPA_AUDIO_FLAG_UNPOSITIONED,
};

static const SpaPropRangeInfo flags_range[] = {
  { "none", { sizeof (uint32_t), &format_flags[0] } },
  { "unpositioned", { sizeof (uint32_t), &format_flags[1] } },
};

static const uint32_t min_uint32 = 1;
static const uint32_t max_uint32 = UINT32_MAX;

static const SpaPropRangeInfo uint32_range[] = {
  { "min", { sizeof (uint32_t), &min_uint32 } },
  { "max", { sizeof (uint32_t), &max_uint32 } },
};

static const SpaPropInfo format_prop_info[] =
{
  { SPA_PROP_ID_AUDIO_INFO,         0,
                                    "info",
                                    SPA_PROP_FLAG_READWRITE | SPA_PROP_FLAG_OPTIONAL | SPA_PROP_FLAG_INFO,
                                    SPA_PROP_TYPE_POINTER, sizeof (SpaAudioInfoRaw),
                                    SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                    NULL },
  { SPA_PROP_ID_AUDIO_FORMAT,       0,
                                    "format",
                                    SPA_PROP_FLAG_READWRITE,
                                    SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                    SPA_PROP_RANGE_TYPE_ENUM, SPA_N_ELEMENTS (format_format_range), format_format_range,
                                    NULL },
  { SPA_PROP_ID_AUDIO_FLAGS,        0,
                                    "flags",
                                    SPA_PROP_FLAG_READWRITE,
                                    SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                    SPA_PROP_RANGE_TYPE_FLAGS, SPA_N_ELEMENTS (flags_range), flags_range,
                                    NULL },
  { SPA_PROP_ID_AUDIO_LAYOUT,       0,
                                    "layout",
                                    SPA_PROP_FLAG_READWRITE,
                                    SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                    SPA_PROP_RANGE_TYPE_ENUM, SPA_N_ELEMENTS (layouts_range), layouts_range,
                                    NULL },
  { SPA_PROP_ID_AUDIO_RATE,         0,
                                    "rate",
                                    SPA_PROP_FLAG_READWRITE,
                                    SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                    SPA_PROP_RANGE_TYPE_MIN_MAX, 2, uint32_range,
                                    NULL },
  { SPA_PROP_ID_AUDIO_CHANNELS,     0,
                                    "channels",
                                    SPA_PROP_FLAG_READWRITE,
                                    SPA_PROP_TYPE_UINT32, sizeof (uint32_t),
                                    SPA_PROP_RANGE_TYPE_MIN_MAX, 2, uint32_range,
                                    NULL },
  { SPA_PROP_ID_AUDIO_CHANNEL_MASK, 0,
                                    "channel-mask",
                                    SPA_PROP_FLAG_READWRITE,
                                    SPA_PROP_TYPE_BITMASK, sizeof (uint32_t),
                                    SPA_PROP_RANGE_TYPE_NONE, 0, NULL,
                                    NULL },
};

SpaResult
spa_prop_info_fill_audio (SpaPropInfo    *info,
                          SpaPropIdAudio  id,
                          size_t          offset)
{
  int i;

  if (info == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;

  i = id - SPA_PROP_ID_MEDIA_CUSTOM_START;

  if (i < 0 || i >= SPA_N_ELEMENTS (format_prop_info))
    return SPA_RESULT_INVALID_PROPERTY_INDEX;

  memcpy (info, &format_prop_info[i], sizeof (SpaPropInfo));
  info->offset = offset - sizeof (SpaFormat) + sizeof (SpaProps);

  return SPA_RESULT_OK;
}


SpaResult
spa_format_audio_init (SpaMediaType     type,
                       SpaMediaSubType  subtype,
                       SpaFormatAudio  *format)
{
  SpaPropInfo *prop_info = NULL;
  unsigned int n_prop_info = 0;
  int i;

  if (type != SPA_MEDIA_TYPE_AUDIO)
    return SPA_RESULT_INVALID_ARGUMENTS;

  switch (subtype) {
    case SPA_MEDIA_SUBTYPE_RAW:
    {
      static SpaPropInfo raw_format_prop_info[] =
      {
        { SPA_PROP_ID_AUDIO_INFO,         offsetof (SpaFormatAudio, info), },
        { SPA_PROP_ID_AUDIO_FORMAT,       offsetof (SpaFormatAudio, info.raw.format), },
        { SPA_PROP_ID_AUDIO_FLAGS,        offsetof (SpaFormatAudio, info.raw.flags), },
        { SPA_PROP_ID_AUDIO_LAYOUT,       offsetof (SpaFormatAudio, info.raw.layout), },
        { SPA_PROP_ID_AUDIO_RATE,         offsetof (SpaFormatAudio, info.raw.rate), },
        { SPA_PROP_ID_AUDIO_CHANNELS,     offsetof (SpaFormatAudio, info.raw.channels), },
        { SPA_PROP_ID_AUDIO_CHANNEL_MASK, offsetof (SpaFormatAudio, info.raw.channel_mask), },
      };
      static const SpaAudioInfoRaw default_raw_info = {
        SPA_AUDIO_FORMAT_S16,
        SPA_AUDIO_FLAG_NONE,
        SPA_AUDIO_LAYOUT_INTERLEAVED,
        44100,
        2,
        0
      };
      prop_info = raw_format_prop_info;
      n_prop_info = SPA_N_ELEMENTS (raw_format_prop_info);
      format->format.props.unset_mask = (1 << 1) | (1 << 3) | (1 << 4) | (1 << 5);
      format->info.raw = default_raw_info;
      break;
    }
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
      break;

    default:
      return SPA_RESULT_INVALID_ARGUMENTS;
  }

  if (prop_info && prop_info[0].name == NULL) {
    for (i = 0; i < n_prop_info; i++)
      spa_prop_info_fill_audio (&prop_info[i],
                                prop_info[i].id,
                                prop_info[i].offset);
  }

  format->format.media_type = type;
  format->format.media_subtype = subtype;
  format->format.props.n_prop_info = n_prop_info;
  format->format.props.prop_info = prop_info;

  return SPA_RESULT_OK;
}

SpaResult
spa_format_audio_parse (const SpaFormat *format,
                        SpaFormatAudio  *aformat)
{
  SpaPropValue value;
  const SpaProps *props;
  SpaResult res;
  unsigned int idx;

  if ((void *)format == (void *)aformat)
    return SPA_RESULT_OK;

  if (format->media_type != SPA_MEDIA_TYPE_AUDIO)
    return SPA_RESULT_INVALID_MEDIA_TYPE;

  if ((res = spa_format_audio_init (format->media_type,
                                    format->media_subtype,
                                    aformat)) < 0)
    return res;

  props = &format->props;
  idx = spa_props_index_for_id (props, SPA_PROP_ID_AUDIO_INFO);
  if ((res = spa_props_get_value (props, idx, &value)) < 0)
    goto fallback;

  if (props->prop_info[idx].type != SPA_PROP_TYPE_POINTER)
    goto fallback;

  memcpy (&aformat->info, value.value, SPA_MIN (value.size, sizeof (aformat->info)));
  aformat->format.props.unset_mask = props->unset_mask;

  return SPA_RESULT_OK;

fallback:
  res = spa_props_copy_values (props, &aformat->format.props);

  return res;
}
