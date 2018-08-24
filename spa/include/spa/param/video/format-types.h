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

#ifndef __SPA_PARAM_VIDEO_FORMAT_TYPES_H__
#define __SPA_PARAM_VIDEO_FORMAT_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/param/video/format.h>
#include <spa/param/video/raw-types.h>

#define SPA_TYPE__FormatVideo		SPA_TYPE_FORMAT_BASE "Video"
#define SPA_TYPE_FORMAT_VIDEO_BASE	SPA_TYPE__FormatVideo ":"

static const struct spa_type_info spa_type_format_video_ids[] = {
	{ SPA_FORMAT_VIDEO_format, SPA_TYPE_FORMAT_VIDEO_BASE "format", SPA_ID_Enum,
		spa_type_video_format, },
	{ SPA_FORMAT_VIDEO_size,  SPA_TYPE_FORMAT_VIDEO_BASE "size", SPA_ID_Rectangle, },
	{ SPA_FORMAT_VIDEO_framerate, SPA_TYPE_FORMAT_VIDEO_BASE "framerate", SPA_ID_Fraction, },
	{ SPA_FORMAT_VIDEO_maxFramerate, SPA_TYPE_FORMAT_VIDEO_BASE "maxFramerate", SPA_ID_Fraction, },
	{ SPA_FORMAT_VIDEO_views, SPA_TYPE_FORMAT_VIDEO_BASE "views", SPA_ID_Int, },
	{ SPA_FORMAT_VIDEO_interlaceMode, SPA_TYPE_FORMAT_VIDEO_BASE "interlaceMode", SPA_ID_Int, },
	{ SPA_FORMAT_VIDEO_pixelAspectRatio, SPA_TYPE_FORMAT_VIDEO_BASE "pixelAspectRatio", SPA_ID_Int, },
	{ SPA_FORMAT_VIDEO_multiviewMode, SPA_TYPE_FORMAT_VIDEO_BASE "multiviewMode", SPA_ID_Int, },
	{ SPA_FORMAT_VIDEO_multiviewFlags, SPA_TYPE_FORMAT_VIDEO_BASE "multiviewFlags", SPA_ID_Int, },
	{ SPA_FORMAT_VIDEO_chromaSite, SPA_TYPE_FORMAT_VIDEO_BASE "chromaSite", SPA_ID_Int, },
	{ SPA_FORMAT_VIDEO_colorRange, SPA_TYPE_FORMAT_VIDEO_BASE "colorRange", SPA_ID_Int, },
	{ SPA_FORMAT_VIDEO_colorMatrix, SPA_TYPE_FORMAT_VIDEO_BASE "colorMatrix", SPA_ID_Int, },
	{ SPA_FORMAT_VIDEO_transferFunction, SPA_TYPE_FORMAT_VIDEO_BASE "transferFunction", SPA_ID_Int, },
	{ SPA_FORMAT_VIDEO_colorPrimaries, SPA_TYPE_FORMAT_VIDEO_BASE "colorPrimaries", SPA_ID_Int, },
	{ SPA_FORMAT_VIDEO_profile, SPA_TYPE_FORMAT_VIDEO_BASE "profile", SPA_ID_Int, },
	{ SPA_FORMAT_VIDEO_level, SPA_TYPE_FORMAT_VIDEO_BASE "level", SPA_ID_Int, },
	{ SPA_FORMAT_VIDEO_streamFormat, SPA_TYPE_FORMAT_VIDEO_BASE "streamFormat", SPA_ID_Int, },
	{ SPA_FORMAT_VIDEO_alignment, SPA_TYPE_FORMAT_VIDEO_BASE "alignment", SPA_ID_Int, },
        { 0, NULL, },
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_VIDEO_FORMAT_TYPES_H */
