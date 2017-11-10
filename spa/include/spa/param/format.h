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

#ifndef __SPA_PARAM_FORMAT_H__
#define __SPA_PARAM_FORMAT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/param/param.h>

#define SPA_TYPE__Format		SPA_TYPE_PARAM_BASE "Format"
#define SPA_TYPE_FORMAT_BASE		SPA_TYPE__Format ":"

#define SPA_TYPE__MediaType		SPA_TYPE_ENUM_BASE "MediaType"
#define SPA_TYPE_MEDIA_TYPE_BASE	SPA_TYPE__MediaType ":"

#define SPA_TYPE_MEDIA_TYPE__audio	SPA_TYPE_MEDIA_TYPE_BASE "audio"
#define SPA_TYPE_MEDIA_TYPE__video	SPA_TYPE_MEDIA_TYPE_BASE "video"
#define SPA_TYPE_MEDIA_TYPE__image	SPA_TYPE_MEDIA_TYPE_BASE "image"
#define SPA_TYPE_MEDIA_TYPE__binary	SPA_TYPE_MEDIA_TYPE_BASE "binary"
#define SPA_TYPE_MEDIA_TYPE__stream	SPA_TYPE_MEDIA_TYPE_BASE "stream"

#define SPA_TYPE__MediaSubtype		SPA_TYPE_ENUM_BASE "MediaSubtype"
#define SPA_TYPE_MEDIA_SUBTYPE_BASE	SPA_TYPE__MediaSubtype ":"

/* generic subtypes */
#define SPA_TYPE_MEDIA_SUBTYPE__raw		SPA_TYPE_MEDIA_SUBTYPE_BASE "raw"

/* video subtypes */
#define SPA_TYPE_MEDIA_SUBTYPE__h264		SPA_TYPE_MEDIA_SUBTYPE_BASE "h264"
#define SPA_TYPE_MEDIA_SUBTYPE__mjpg		SPA_TYPE_MEDIA_SUBTYPE_BASE "mjpg"
#define SPA_TYPE_MEDIA_SUBTYPE__dv		SPA_TYPE_MEDIA_SUBTYPE_BASE "dv"
#define SPA_TYPE_MEDIA_SUBTYPE__mpegts		SPA_TYPE_MEDIA_SUBTYPE_BASE "mpegts"
#define SPA_TYPE_MEDIA_SUBTYPE__h263		SPA_TYPE_MEDIA_SUBTYPE_BASE "h263"
#define SPA_TYPE_MEDIA_SUBTYPE__mpeg1		SPA_TYPE_MEDIA_SUBTYPE_BASE "mpeg1"
#define SPA_TYPE_MEDIA_SUBTYPE__mpeg2		SPA_TYPE_MEDIA_SUBTYPE_BASE "mpeg2"
#define SPA_TYPE_MEDIA_SUBTYPE__mpeg4		SPA_TYPE_MEDIA_SUBTYPE_BASE "mpeg4"
#define SPA_TYPE_MEDIA_SUBTYPE__xvid		SPA_TYPE_MEDIA_SUBTYPE_BASE "xvid"
#define SPA_TYPE_MEDIA_SUBTYPE__vc1		SPA_TYPE_MEDIA_SUBTYPE_BASE "vc1"
#define SPA_TYPE_MEDIA_SUBTYPE__vp8		SPA_TYPE_MEDIA_SUBTYPE_BASE "vp8"
#define SPA_TYPE_MEDIA_SUBTYPE__vp9		SPA_TYPE_MEDIA_SUBTYPE_BASE "vp9"
#define SPA_TYPE_MEDIA_SUBTYPE__jpeg		SPA_TYPE_MEDIA_SUBTYPE_BASE "jpeg"
#define SPA_TYPE_MEDIA_SUBTYPE__bayer		SPA_TYPE_MEDIA_SUBTYPE_BASE "bayer"

/* audio subtypes */
#define SPA_TYPE_MEDIA_SUBTYPE__mp3		SPA_TYPE_MEDIA_SUBTYPE_BASE "mp3"
#define SPA_TYPE_MEDIA_SUBTYPE__aac		SPA_TYPE_MEDIA_SUBTYPE_BASE "aac"
#define SPA_TYPE_MEDIA_SUBTYPE__vorbis		SPA_TYPE_MEDIA_SUBTYPE_BASE "vorbis"
#define SPA_TYPE_MEDIA_SUBTYPE__wma		SPA_TYPE_MEDIA_SUBTYPE_BASE "wma"
#define SPA_TYPE_MEDIA_SUBTYPE__ra		SPA_TYPE_MEDIA_SUBTYPE_BASE "ra"
#define SPA_TYPE_MEDIA_SUBTYPE__sbc		SPA_TYPE_MEDIA_SUBTYPE_BASE "sbc"
#define SPA_TYPE_MEDIA_SUBTYPE__adpcm		SPA_TYPE_MEDIA_SUBTYPE_BASE "adpcm"
#define SPA_TYPE_MEDIA_SUBTYPE__g723		SPA_TYPE_MEDIA_SUBTYPE_BASE "g723"
#define SPA_TYPE_MEDIA_SUBTYPE__g726		SPA_TYPE_MEDIA_SUBTYPE_BASE "g726"
#define SPA_TYPE_MEDIA_SUBTYPE__g729		SPA_TYPE_MEDIA_SUBTYPE_BASE "g729"
#define SPA_TYPE_MEDIA_SUBTYPE__amr		SPA_TYPE_MEDIA_SUBTYPE_BASE "amr"
#define SPA_TYPE_MEDIA_SUBTYPE__gsm		SPA_TYPE_MEDIA_SUBTYPE_BASE "gsm"
#define SPA_TYPE_MEDIA_SUBTYPE__midi		SPA_TYPE_MEDIA_SUBTYPE_BASE "midi"

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_FORMAT_H__ */
