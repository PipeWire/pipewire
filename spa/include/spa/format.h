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

#include <spa/defs.h>
#include <spa/pod.h>

#define SPA_MEDIA_TYPE_URI           "http://spaplug.in/ns/media-type"
#define SPA_MEDIA_TYPE_PREFIX        SPA_MEDIA_TYPE_URI "#"

#define SPA_MEDIA_TYPE__audio        SPA_MEDIA_TYPE_PREFIX "audio"
#define SPA_MEDIA_TYPE__video        SPA_MEDIA_TYPE_PREFIX "video"
#define SPA_MEDIA_TYPE__image        SPA_MEDIA_TYPE_PREFIX "image"

#define SPA_MEDIA_SUBTYPE_URI           "http://spaplug.in/ns/media-subtype"
#define SPA_MEDIA_SUBTYPE_PREFIX        SPA_MEDIA_TYPE_URI "#"

/* generic subtypes */
#define SPA_MEDIA_SUBTYPE__raw          SPA_MEDIA_TYPE_PREFIX "raw"

/* video subtypes */
#define SPA_MEDIA_SUBTYPE__h264         SPA_MEDIA_SUBTYPE_PREFIX "h264"
#define SPA_MEDIA_SUBTYPE__mjpg         SPA_MEDIA_SUBTYPE_PREFIX "mjpg"
#define SPA_MEDIA_SUBTYPE__dv           SPA_MEDIA_SUBTYPE_PREFIX "dv"
#define SPA_MEDIA_SUBTYPE__mpegts       SPA_MEDIA_SUBTYPE_PREFIX "mpegts"
#define SPA_MEDIA_SUBTYPE__h263         SPA_MEDIA_SUBTYPE_PREFIX "h263"
#define SPA_MEDIA_SUBTYPE__mpeg1        SPA_MEDIA_SUBTYPE_PREFIX "mpeg1"
#define SPA_MEDIA_SUBTYPE__mpeg2        SPA_MEDIA_SUBTYPE_PREFIX "mpeg2"
#define SPA_MEDIA_SUBTYPE__mpeg4        SPA_MEDIA_SUBTYPE_PREFIX "mpeg4"
#define SPA_MEDIA_SUBTYPE__xvid         SPA_MEDIA_SUBTYPE_PREFIX "xvid"
#define SPA_MEDIA_SUBTYPE__vc1          SPA_MEDIA_SUBTYPE_PREFIX "vc1"
#define SPA_MEDIA_SUBTYPE__vp8          SPA_MEDIA_SUBTYPE_PREFIX "vp8"
#define SPA_MEDIA_SUBTYPE__vp9          SPA_MEDIA_SUBTYPE_PREFIX "vp9"
#define SPA_MEDIA_SUBTYPE__jpeg         SPA_MEDIA_SUBTYPE_PREFIX "jpeg"
#define SPA_MEDIA_SUBTYPE__bayer        SPA_MEDIA_SUBTYPE_PREFIX "bayer"

/* audio subtypes */
#define SPA_MEDIA_SUBTYPE__mp3          SPA_MEDIA_SUBTYPE_PREFIX "mp3"
#define SPA_MEDIA_SUBTYPE__aac          SPA_MEDIA_SUBTYPE_PREFIX "aac"
#define SPA_MEDIA_SUBTYPE__vorbis       SPA_MEDIA_SUBTYPE_PREFIX "vorbis"
#define SPA_MEDIA_SUBTYPE__wma          SPA_MEDIA_SUBTYPE_PREFIX "wma"
#define SPA_MEDIA_SUBTYPE__ra           SPA_MEDIA_SUBTYPE_PREFIX "ra"
#define SPA_MEDIA_SUBTYPE__sbc          SPA_MEDIA_SUBTYPE_PREFIX "sbc"
#define SPA_MEDIA_SUBTYPE__adpcm        SPA_MEDIA_SUBTYPE_PREFIX "adpcm"
#define SPA_MEDIA_SUBTYPE__g723         SPA_MEDIA_SUBTYPE_PREFIX "g723"
#define SPA_MEDIA_SUBTYPE__g726         SPA_MEDIA_SUBTYPE_PREFIX "g726"
#define SPA_MEDIA_SUBTYPE__g729         SPA_MEDIA_SUBTYPE_PREFIX "g729"
#define SPA_MEDIA_SUBTYPE__amr          SPA_MEDIA_SUBTYPE_PREFIX "amr"
#define SPA_MEDIA_SUBTYPE__gsm          SPA_MEDIA_SUBTYPE_PREFIX "gsm"

typedef struct {
  SpaPODObjectBody obj_body;
  SpaPODURI        media_type           SPA_ALIGNED (8);
  SpaPODURI        media_subtype        SPA_ALIGNED (8);
  /* contents follow, series of SpaPODProp */
} SpaFormatBody;

/**
 * SpaFormat:
 * @media_type: media type
 * @media_subtype: subtype
 * @pod: POD object with properties
 */
struct _SpaFormat {
  SpaPOD        pod;
  SpaFormatBody body;
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_FORMAT_H__ */
