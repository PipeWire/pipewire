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

#ifndef __SPA_PARAM_VIDEO_FORMAT_UTILS_H__
#define __SPA_PARAM_VIDEO_FORMAT_UTILS_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/pod/parser.h>
#include <spa/pod/builder.h>
#include <spa/param/video/format.h>
#include <spa/param/format-utils.h>

static inline int
spa_format_video_raw_parse(const struct spa_pod *format,
			   struct spa_video_info_raw *info)
{
	return spa_pod_object_parse(format,
		":", SPA_FORMAT_VIDEO_format,		"I", &info->format,
		":", SPA_FORMAT_VIDEO_size,		"R", &info->size,
		":", SPA_FORMAT_VIDEO_framerate,	"F", &info->framerate,
		":", SPA_FORMAT_VIDEO_maxFramerate,	"?F", &info->max_framerate,
		":", SPA_FORMAT_VIDEO_views,		"?i", &info->views,
		":", SPA_FORMAT_VIDEO_interlaceMode,	"?i", &info->interlace_mode,
		":", SPA_FORMAT_VIDEO_pixelAspectRatio,	"?F", &info->pixel_aspect_ratio,
		":", SPA_FORMAT_VIDEO_multiviewMode,	"?i", &info->multiview_mode,
		":", SPA_FORMAT_VIDEO_multiviewFlags,	"?i", &info->multiview_flags,
		":", SPA_FORMAT_VIDEO_chromaSite,	"?i", &info->chroma_site,
		":", SPA_FORMAT_VIDEO_colorRange,	"?i", &info->color_range,
		":", SPA_FORMAT_VIDEO_colorMatrix,	"?i", &info->color_matrix,
		":", SPA_FORMAT_VIDEO_transferFunction,	"?i", &info->transfer_function,
		":", SPA_FORMAT_VIDEO_colorPrimaries,	"?i", &info->color_primaries, NULL);
}

static inline struct spa_pod *
spa_format_video_raw_build(struct spa_pod_builder *builder, uint32_t id,
			   struct spa_video_info_raw *info)
{
	const struct spa_pod_id media_type = SPA_POD_Id(SPA_MEDIA_TYPE_video);
	const struct spa_pod_id media_subtype = SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw);
	const struct spa_pod_id format = SPA_POD_Id(info->format);
	const struct spa_pod_rectangle size = SPA_POD_Rectangle(info->size);
	const struct spa_pod_fraction framerate = SPA_POD_Fraction(info->framerate);

	return (struct spa_pod *) spa_pod_builder_object(builder,
			SPA_TYPE_OBJECT_Format, id,
			SPA_FORMAT_mediaType,		&media_type,
			SPA_FORMAT_mediaSubtype,	&media_subtype,
			SPA_FORMAT_VIDEO_format,	&format,
			SPA_FORMAT_VIDEO_size,		&size,
			SPA_FORMAT_VIDEO_framerate,	&framerate,
			0);
}

static inline int
spa_format_video_h264_parse(const struct spa_pod *format,
			    struct spa_video_info_h264 *info)
{
	return spa_pod_object_parse(format,
		":", SPA_FORMAT_VIDEO_size,		"?R", &info->size,
		":", SPA_FORMAT_VIDEO_framerate,	"?F", &info->framerate,
		":", SPA_FORMAT_VIDEO_maxFramerate,	"?F", &info->max_framerate,
		":", SPA_FORMAT_VIDEO_streamFormat,	"?i", &info->stream_format,
		":", SPA_FORMAT_VIDEO_alignment,	"?i", &info->alignment, NULL);
}

static inline int
spa_format_video_mjpg_parse(const struct spa_pod *format,
			    struct spa_video_info_mjpg *info)
{
	return spa_pod_object_parse(format,
		":", SPA_FORMAT_VIDEO_size,		"?R", &info->size,
		":", SPA_FORMAT_VIDEO_framerate,	"?F", &info->framerate,
		":", SPA_FORMAT_VIDEO_maxFramerate,	"?F", &info->max_framerate, NULL);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __SPA_PARAM_VIDEO_FORMAT_UTILS */
