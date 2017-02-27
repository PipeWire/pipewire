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

#ifndef __SPA_FORMAT_H__
#define __SPA_FORMAT_H__

#ifdef __cplusplus
extern "C" {
#endif

#define SPA_FORMAT_URI             "http://spaplug.in/ns/format"
#define SPA_FORMAT_PREFIX          SPA_FORMAT_URI "#"

typedef struct _SpaFormat SpaFormat;

#include <stdarg.h>

#include <spa/defs.h>
#include <spa/pod.h>

typedef enum {
  SPA_MEDIA_TYPE_INVALID         = 0,
  SPA_MEDIA_TYPE_AUDIO           = 1,
  SPA_MEDIA_TYPE_VIDEO           = 2,
  SPA_MEDIA_TYPE_IMAGE           = 3,
} SpaMediaType;

typedef enum {
  SPA_MEDIA_SUBTYPE_INVALID         = 0,

#define SPA_MEDIA_SUBTYPE_ANY_FIRST   1
  SPA_MEDIA_SUBTYPE_RAW             = SPA_MEDIA_SUBTYPE_ANY_FIRST,
#define SPA_MEDIA_SUBTYPE_ANY_LAST    SPA_MEDIA_SUBTYPE_RAW

  /* VIDEO */
#define SPA_MEDIA_SUBTYPE_VIDEO_FIRST    20
  SPA_MEDIA_SUBTYPE_H264            = SPA_MEDIA_SUBTYPE_VIDEO_FIRST,
  SPA_MEDIA_SUBTYPE_MJPG,
  SPA_MEDIA_SUBTYPE_DV,
  SPA_MEDIA_SUBTYPE_MPEGTS,
  SPA_MEDIA_SUBTYPE_H263,
  SPA_MEDIA_SUBTYPE_MPEG1,
  SPA_MEDIA_SUBTYPE_MPEG2,
  SPA_MEDIA_SUBTYPE_MPEG4,
  SPA_MEDIA_SUBTYPE_XVID,
  SPA_MEDIA_SUBTYPE_VC1,
  SPA_MEDIA_SUBTYPE_VP8,
  SPA_MEDIA_SUBTYPE_VP9,
  SPA_MEDIA_SUBTYPE_JPEG,
  SPA_MEDIA_SUBTYPE_BAYER,
#define SPA_MEDIA_SUBTYPE_VIDEO_LAST    SPA_MEDIA_SUBTYPE_BAYER

  /* AUDIO */
#define SPA_MEDIA_SUBTYPE_AUDIO_FIRST    200
  SPA_MEDIA_SUBTYPE_MP3             = SPA_MEDIA_SUBTYPE_AUDIO_FIRST,
  SPA_MEDIA_SUBTYPE_AAC,
  SPA_MEDIA_SUBTYPE_VORBIS,
  SPA_MEDIA_SUBTYPE_WMA,
  SPA_MEDIA_SUBTYPE_RA,
  SPA_MEDIA_SUBTYPE_SBC,
  SPA_MEDIA_SUBTYPE_ADPCM,
  SPA_MEDIA_SUBTYPE_G723,
  SPA_MEDIA_SUBTYPE_G726,
  SPA_MEDIA_SUBTYPE_G729,
  SPA_MEDIA_SUBTYPE_AMR,
  SPA_MEDIA_SUBTYPE_GSM,
#define SPA_MEDIA_SUBTYPE_AUDIO_LAST    SPA_MEDIA_SUBTYPE_GSM

} SpaMediaSubType;

typedef enum {
  SPA_PROP_ID_INVALID            = 0,
  SPA_PROP_ID_MEDIA_CUSTOM_START = 200,
} SpaFormatProps;

typedef struct {
  SpaPODInt     media_type;
  SpaPODInt     media_subtype;
  /* contents follow, series of SpaPODProp */
} SpaFormatBody;

/**
 * SpaFormat:
 * @media_type: media type
 * @media_subtype: subtype
 * @pod: POD object with properties
 */
struct _SpaFormat {
  SpaPODObject  obj;
  SpaFormatBody body;
};

#define SPA_FORMAT_BODY_FOREACH(body, size, iter) \
  for ((iter) = SPA_MEMBER ((body), sizeof (SpaFormatBody), SpaPODProp); \
       (iter) < SPA_MEMBER ((body), (size), SpaPODProp); \
       (iter) = SPA_MEMBER ((iter), SPA_ROUND_UP_N (SPA_POD_SIZE (iter), 8), SpaPODProp))

static inline SpaPODProp *
spa_format_find_prop (const SpaFormat *format, uint32_t key)
{
  SpaPODProp *res;
  SPA_FORMAT_BODY_FOREACH (&format->body, SPA_POD_BODY_SIZE (format), res) {
    if (res->pod.type == SPA_POD_TYPE_PROP && res->body.key == key)
      return res;
  }
  return NULL;
}

static inline SpaResult
spa_format_fixate (SpaFormat *format)
{
  if (format == NULL)
    return SPA_RESULT_INVALID_ARGUMENTS;
  return SPA_RESULT_OK;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_FORMAT_H__ */
