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

#ifndef __SPA_PARAM_VIDEO_FORMAT_H__
#define __SPA_PARAM_VIDEO_FORMAT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/param/format-utils.h>
#include <spa/param/video/raw.h>
#include <spa/param/video/encoded.h>

#define SPA_TYPE_FORMAT__Video		SPA_TYPE_FORMAT_BASE "Video"
#define SPA_TYPE_FORMAT_VIDEO_BASE	SPA_TYPE_FORMAT__Video ":"

#define SPA_TYPE_FORMAT_VIDEO__format		SPA_TYPE_FORMAT_VIDEO_BASE "format"
#define SPA_TYPE_FORMAT_VIDEO__size		SPA_TYPE_FORMAT_VIDEO_BASE "size"
#define SPA_TYPE_FORMAT_VIDEO__framerate	SPA_TYPE_FORMAT_VIDEO_BASE "framerate"
#define SPA_TYPE_FORMAT_VIDEO__maxFramerate	SPA_TYPE_FORMAT_VIDEO_BASE "max-framerate"
#define SPA_TYPE_FORMAT_VIDEO__views		SPA_TYPE_FORMAT_VIDEO_BASE "views"
#define SPA_TYPE_FORMAT_VIDEO__interlaceMode	SPA_TYPE_FORMAT_VIDEO_BASE "interlace-mode"
#define SPA_TYPE_FORMAT_VIDEO__pixelAspectRatio	SPA_TYPE_FORMAT_VIDEO_BASE "pixel-aspect-ratio"
#define SPA_TYPE_FORMAT_VIDEO__multiviewMode	SPA_TYPE_FORMAT_VIDEO_BASE "multiview-mode"
#define SPA_TYPE_FORMAT_VIDEO__multiviewFlags	SPA_TYPE_FORMAT_VIDEO_BASE "multiview-flags"
#define SPA_TYPE_FORMAT_VIDEO__chromaSite	SPA_TYPE_FORMAT_VIDEO_BASE "chroma-site"
#define SPA_TYPE_FORMAT_VIDEO__colorRange	SPA_TYPE_FORMAT_VIDEO_BASE "color-range"
#define SPA_TYPE_FORMAT_VIDEO__colorMatrix	SPA_TYPE_FORMAT_VIDEO_BASE "color-matrix"
#define SPA_TYPE_FORMAT_VIDEO__transferFunction	SPA_TYPE_FORMAT_VIDEO_BASE "transfer-function"
#define SPA_TYPE_FORMAT_VIDEO__colorPrimaries	SPA_TYPE_FORMAT_VIDEO_BASE "color-primaries"
#define SPA_TYPE_FORMAT_VIDEO__profile		SPA_TYPE_FORMAT_VIDEO_BASE "profile"
#define SPA_TYPE_FORMAT_VIDEO__level		SPA_TYPE_FORMAT_VIDEO_BASE "level"
#define SPA_TYPE_FORMAT_VIDEO__streamFormat	SPA_TYPE_FORMAT_VIDEO_BASE "stream-format"
#define SPA_TYPE_FORMAT_VIDEO__alignment	SPA_TYPE_FORMAT_VIDEO_BASE "alignment"

struct spa_video_info {
	uint32_t media_type;
	uint32_t media_subtype;
	union {
		struct spa_video_info_raw raw;
		struct spa_video_info_h264 h264;
		struct spa_video_info_mjpg mjpg;
	} info;
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __SPA_PARAM_VIDEO_FORMAT */
