/* Simple Plugin API
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef SPA_PARAM_VIDEO_FORMAT_UTILS_H
#define SPA_PARAM_VIDEO_FORMAT_UTILS_H

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
		":", SPA_FORMAT_VIDEO_interlaceMode,	"?I", &info->interlace_mode,
		":", SPA_FORMAT_VIDEO_pixelAspectRatio,	"?F", &info->pixel_aspect_ratio,
		":", SPA_FORMAT_VIDEO_multiviewMode,	"?I", &info->multiview_mode,
		":", SPA_FORMAT_VIDEO_multiviewFlags,	"?I", &info->multiview_flags,
		":", SPA_FORMAT_VIDEO_chromaSite,	"?I", &info->chroma_site,
		":", SPA_FORMAT_VIDEO_colorRange,	"?I", &info->color_range,
		":", SPA_FORMAT_VIDEO_colorMatrix,	"?I", &info->color_matrix,
		":", SPA_FORMAT_VIDEO_transferFunction,	"?I", &info->transfer_function,
		":", SPA_FORMAT_VIDEO_colorPrimaries,	"?I", &info->color_primaries, NULL);
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
		":", SPA_FORMAT_VIDEO_streamFormat,	"?I", &info->stream_format,
		":", SPA_FORMAT_VIDEO_alignment,	"?I", &info->alignment, NULL);
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

#endif /* SPA_PARAM_VIDEO_FORMAT_UTILS_H */
