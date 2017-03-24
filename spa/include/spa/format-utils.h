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

#ifndef __SPA_FORMAT_UTILS_H__
#define __SPA_FORMAT_UTILS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

#include <spa/format.h>
#include <spa/pod-utils.h>
#include <spa/type-map.h>

typedef struct {
  uint32_t audio;
  uint32_t video;
  uint32_t image;
} SpaTypeMediaType;

static inline void
spa_type_media_type_map (SpaTypeMap *map, SpaTypeMediaType *type)
{
  if (type->audio == 0) {
    type->audio = spa_type_map_get_id (map, SPA_TYPE_MEDIA_TYPE__audio);
    type->video = spa_type_map_get_id (map, SPA_TYPE_MEDIA_TYPE__video);
    type->image = spa_type_map_get_id (map, SPA_TYPE_MEDIA_TYPE__image);
  }
}

typedef struct {
  uint32_t raw;
} SpaTypeMediaSubtype;

static inline void
spa_type_media_subtype_map (SpaTypeMap *map, SpaTypeMediaSubtype *type)
{
  if (type->raw == 0) {
    type->raw = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__raw);
  }
}

typedef struct {
  uint32_t h264;
  uint32_t mjpg;
  uint32_t dv;
  uint32_t mpegts;
  uint32_t h263;
  uint32_t mpeg1;
  uint32_t mpeg2;
  uint32_t mpeg4;
  uint32_t xvid;
  uint32_t vc1;
  uint32_t vp8;
  uint32_t vp9;
  uint32_t jpeg;
  uint32_t bayer;
} SpaTypeMediaSubtypeVideo;

static inline void
spa_type_media_subtype_video_map (SpaTypeMap *map, SpaTypeMediaSubtypeVideo *type)
{
  if (type->h264 == 0) {
    type->h264 = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__h264);
    type->mjpg = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__mjpg);
    type->dv = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__dv);
    type->mpegts = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__mpegts);
    type->h263 = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__h263);
    type->mpeg1 = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__mpeg1);
    type->mpeg2 = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__mpeg2);
    type->mpeg4 = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__mpeg4);
    type->xvid = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__xvid);
    type->vc1 = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__vc1);
    type->vp8 = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__vp8);
    type->vp9 = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__vp9);
    type->jpeg = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__jpeg);
    type->bayer = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__bayer);
  }
}

typedef struct {
  uint32_t mp3;
  uint32_t aac;
  uint32_t vorbis;
  uint32_t wma;
  uint32_t ra;
  uint32_t sbc;
  uint32_t adpcm;
  uint32_t g723;
  uint32_t g726;
  uint32_t g729;
  uint32_t amr;
  uint32_t gsm;
} SpaTypeMediaSubtypeAudio;

static inline void
spa_type_media_subtype_audio_map (SpaTypeMap *map, SpaTypeMediaSubtypeAudio *type)
{
  if (type->mp3 == 0) {
    type->mp3 = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__mp3);
    type->aac = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__aac);
    type->vorbis = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__vorbis);
    type->wma = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__wma);
    type->ra = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__ra);
    type->sbc = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__sbc);
    type->adpcm = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__adpcm);
    type->g723 = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__g723);
    type->g726 = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__g726);
    type->g729 = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__g729);
    type->amr = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__amr);
    type->gsm = spa_type_map_get_id (map, SPA_TYPE_MEDIA_SUBTYPE__gsm);
  }
}

#define SPA_FORMAT_INIT(size,type,media_type,media_subtype,...)         \
  { { size, SPA_POD_TYPE_OBJECT },                                      \
    { { 0, type },                                                      \
        SPA_POD_ID_INIT (media_type),                                   \
        SPA_POD_ID_INIT (media_subtype) } }

#define SPA_FORMAT_BODY_FOREACH(body, size, iter) \
  for ((iter) = SPA_MEMBER ((body), sizeof (SpaFormatBody), SpaPODProp); \
       (iter) < SPA_MEMBER ((body), (size), SpaPODProp); \
       (iter) = SPA_MEMBER ((iter), SPA_ROUND_UP_N (SPA_POD_SIZE (iter), 8), SpaPODProp))

#define SPA_FORMAT_FOREACH(format, iter) \
  SPA_FORMAT_BODY_FOREACH(&format->body, SPA_POD_BODY_SIZE(format), iter)

static inline SpaPODProp *
spa_format_find_prop (const SpaFormat *format, uint32_t key)
{
  return spa_pod_contents_find_prop (&format->pod, sizeof (SpaFormat), key);
}

static inline uint32_t
spa_format_query (const SpaFormat *format, uint32_t key, ...)
{
  uint32_t count;
  va_list args;

  va_start (args, key);
  count = spa_pod_contents_queryv (&format->pod, sizeof (SpaFormat), key, args);
  va_end (args);

  return count;
}

static inline SpaResult
spa_format_fixate (SpaFormat *format)
{
  SpaPODProp *prop;

  SPA_FORMAT_FOREACH (format, prop)
    prop->body.flags &= ~SPA_POD_PROP_FLAG_UNSET;

  return SPA_RESULT_OK;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_FORMAT_UTILS_H__ */
