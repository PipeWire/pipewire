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

#ifndef __SPA_VIDEO_FORMAT_H__
#define __SPA_VIDEO_FORMAT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/format-utils.h>
#include <spa/video/raw.h>
#include <spa/video/encoded.h>

typedef struct _SpaVideoInfo SpaVideoInfo;

#define SPA_PROP_VIDEO_URI           "http://spaplug.in/ns/prop-video"
#define SPA_PROP_VIDEO_PREFIX        SPA_PROP_VIDEO_URI "#"

#define SPA_PROP_VIDEO__format                  SPA_PROP_VIDEO_PREFIX "format"
#define SPA_PROP_VIDEO__size                    SPA_PROP_VIDEO_PREFIX "size"
#define SPA_PROP_VIDEO__framerate               SPA_PROP_VIDEO_PREFIX "framerate"
#define SPA_PROP_VIDEO__maxFramerate            SPA_PROP_VIDEO_PREFIX "max-framerate"
#define SPA_PROP_VIDEO__views                   SPA_PROP_VIDEO_PREFIX "views"
#define SPA_PROP_VIDEO__interlaceMode           SPA_PROP_VIDEO_PREFIX "interlace-mode"
#define SPA_PROP_VIDEO__pixelAspectRatio        SPA_PROP_VIDEO_PREFIX "pixel-aspect-ratio"
#define SPA_PROP_VIDEO__multiviewMode           SPA_PROP_VIDEO_PREFIX "multiview-mode"
#define SPA_PROP_VIDEO__multiviewFlags          SPA_PROP_VIDEO_PREFIX "multiview-flags"
#define SPA_PROP_VIDEO__chromaSite              SPA_PROP_VIDEO_PREFIX "chroma-site"
#define SPA_PROP_VIDEO__colorRange              SPA_PROP_VIDEO_PREFIX "color-range"
#define SPA_PROP_VIDEO__colorMatrix             SPA_PROP_VIDEO_PREFIX "color-matrix"
#define SPA_PROP_VIDEO__transferFunction        SPA_PROP_VIDEO_PREFIX "transfer-function"
#define SPA_PROP_VIDEO__colorPrimaries          SPA_PROP_VIDEO_PREFIX "color-primaries"
#define SPA_PROP_VIDEO__profile                 SPA_PROP_VIDEO_PREFIX "profile"
#define SPA_PROP_VIDEO__level                   SPA_PROP_VIDEO_PREFIX "level"
#define SPA_PROP_VIDEO__streamFormat            SPA_PROP_VIDEO_PREFIX "stream-format"
#define SPA_PROP_VIDEO__alignment               SPA_PROP_VIDEO_PREFIX "alignment"

struct _SpaVideoInfo {
  uint32_t media_type;
  uint32_t media_subtype;
  union {
    SpaVideoInfoRaw raw;
    SpaVideoInfoH264 h264;
    SpaVideoInfoMJPG mjpg;
  } info;
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_VIDEO_FORMAT */
