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

#ifndef __SPA_VIDEO_RAW_H__
#define __SPA_VIDEO_RAW_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/param/video/chroma.h>
#include <spa/param/video/color.h>
#include <spa/param/video/multiview.h>

#define SPA_VIDEO_MAX_PLANES 4
#define SPA_VIDEO_MAX_COMPONENTS 4

#define SPA_TYPE__VideoFormat		SPA_TYPE_ENUM_BASE "VideoFormat"
#define SPA_TYPE_VIDEO_FORMAT_BASE	SPA_TYPE__VideoFormat ":"

#define SPA_TYPE_VIDEO_FORMAT__ENCODED		SPA_TYPE_VIDEO_FORMAT_BASE "encoded"
#define SPA_TYPE_VIDEO_FORMAT__I420		SPA_TYPE_VIDEO_FORMAT_BASE "I420"
#define SPA_TYPE_VIDEO_FORMAT__YV12		SPA_TYPE_VIDEO_FORMAT_BASE "YV12"
#define SPA_TYPE_VIDEO_FORMAT__YUY2		SPA_TYPE_VIDEO_FORMAT_BASE "YUY2"
#define SPA_TYPE_VIDEO_FORMAT__UYVY		SPA_TYPE_VIDEO_FORMAT_BASE "UYVY"
#define SPA_TYPE_VIDEO_FORMAT__AYUV		SPA_TYPE_VIDEO_FORMAT_BASE "AYUV"
#define SPA_TYPE_VIDEO_FORMAT__RGBx		SPA_TYPE_VIDEO_FORMAT_BASE "RGBx"
#define SPA_TYPE_VIDEO_FORMAT__BGRx		SPA_TYPE_VIDEO_FORMAT_BASE "BGRx"
#define SPA_TYPE_VIDEO_FORMAT__xRGB		SPA_TYPE_VIDEO_FORMAT_BASE "xRGB"
#define SPA_TYPE_VIDEO_FORMAT__xBGR		SPA_TYPE_VIDEO_FORMAT_BASE "xBGR"
#define SPA_TYPE_VIDEO_FORMAT__RGBA		SPA_TYPE_VIDEO_FORMAT_BASE "RGBA"
#define SPA_TYPE_VIDEO_FORMAT__BGRA		SPA_TYPE_VIDEO_FORMAT_BASE "BGRA"
#define SPA_TYPE_VIDEO_FORMAT__ARGB		SPA_TYPE_VIDEO_FORMAT_BASE "ARGB"
#define SPA_TYPE_VIDEO_FORMAT__ABGR		SPA_TYPE_VIDEO_FORMAT_BASE "ABGR"
#define SPA_TYPE_VIDEO_FORMAT__RGB		SPA_TYPE_VIDEO_FORMAT_BASE "RGB"
#define SPA_TYPE_VIDEO_FORMAT__BGR		SPA_TYPE_VIDEO_FORMAT_BASE "BGR"
#define SPA_TYPE_VIDEO_FORMAT__Y41B		SPA_TYPE_VIDEO_FORMAT_BASE "Y41B"
#define SPA_TYPE_VIDEO_FORMAT__Y42B		SPA_TYPE_VIDEO_FORMAT_BASE "Y42B"
#define SPA_TYPE_VIDEO_FORMAT__YVYU		SPA_TYPE_VIDEO_FORMAT_BASE "YVYU"
#define SPA_TYPE_VIDEO_FORMAT__Y444		SPA_TYPE_VIDEO_FORMAT_BASE "Y444"
#define SPA_TYPE_VIDEO_FORMAT__v210		SPA_TYPE_VIDEO_FORMAT_BASE "v210"
#define SPA_TYPE_VIDEO_FORMAT__v216		SPA_TYPE_VIDEO_FORMAT_BASE "v216"
#define SPA_TYPE_VIDEO_FORMAT__NV12		SPA_TYPE_VIDEO_FORMAT_BASE "NV12"
#define SPA_TYPE_VIDEO_FORMAT__NV21		SPA_TYPE_VIDEO_FORMAT_BASE "NV21"
#define SPA_TYPE_VIDEO_FORMAT__GRAY8		SPA_TYPE_VIDEO_FORMAT_BASE "GRAY8"
#define SPA_TYPE_VIDEO_FORMAT__GRAY16_BE	SPA_TYPE_VIDEO_FORMAT_BASE "GRAY16_BE"
#define SPA_TYPE_VIDEO_FORMAT__GRAY16_LE	SPA_TYPE_VIDEO_FORMAT_BASE "GRAY16_LE"
#define SPA_TYPE_VIDEO_FORMAT__v308		SPA_TYPE_VIDEO_FORMAT_BASE "v308"
#define SPA_TYPE_VIDEO_FORMAT__RGB16		SPA_TYPE_VIDEO_FORMAT_BASE "RGB16"
#define SPA_TYPE_VIDEO_FORMAT__BGR16		SPA_TYPE_VIDEO_FORMAT_BASE "BGR16"
#define SPA_TYPE_VIDEO_FORMAT__RGB15		SPA_TYPE_VIDEO_FORMAT_BASE "RGB15"
#define SPA_TYPE_VIDEO_FORMAT__BGR15		SPA_TYPE_VIDEO_FORMAT_BASE "BGR15"
#define SPA_TYPE_VIDEO_FORMAT__UYVP		SPA_TYPE_VIDEO_FORMAT_BASE "UYVP"
#define SPA_TYPE_VIDEO_FORMAT__A420		SPA_TYPE_VIDEO_FORMAT_BASE "A420"
#define SPA_TYPE_VIDEO_FORMAT__RGB8P		SPA_TYPE_VIDEO_FORMAT_BASE "RGB8P"
#define SPA_TYPE_VIDEO_FORMAT__YUV9		SPA_TYPE_VIDEO_FORMAT_BASE "YUV9"
#define SPA_TYPE_VIDEO_FORMAT__YVU9		SPA_TYPE_VIDEO_FORMAT_BASE "YVU9"
#define SPA_TYPE_VIDEO_FORMAT__IYU1		SPA_TYPE_VIDEO_FORMAT_BASE "IYU1"
#define SPA_TYPE_VIDEO_FORMAT__ARGB64		SPA_TYPE_VIDEO_FORMAT_BASE "ARGB64"
#define SPA_TYPE_VIDEO_FORMAT__AYUV64		SPA_TYPE_VIDEO_FORMAT_BASE "AYUV64"
#define SPA_TYPE_VIDEO_FORMAT__r210		SPA_TYPE_VIDEO_FORMAT_BASE "r210"
#define SPA_TYPE_VIDEO_FORMAT__I420_10BE	SPA_TYPE_VIDEO_FORMAT_BASE "I420_10BE"
#define SPA_TYPE_VIDEO_FORMAT__I420_10LE	SPA_TYPE_VIDEO_FORMAT_BASE "I420_10LE"
#define SPA_TYPE_VIDEO_FORMAT__I422_10BE	SPA_TYPE_VIDEO_FORMAT_BASE "I422_10BE"
#define SPA_TYPE_VIDEO_FORMAT__I422_10LE	SPA_TYPE_VIDEO_FORMAT_BASE "I422_10LE"
#define SPA_TYPE_VIDEO_FORMAT__Y444_10BE	SPA_TYPE_VIDEO_FORMAT_BASE "Y444_10BE"
#define SPA_TYPE_VIDEO_FORMAT__Y444_10LE	SPA_TYPE_VIDEO_FORMAT_BASE "Y444_10LE"
#define SPA_TYPE_VIDEO_FORMAT__GBR		SPA_TYPE_VIDEO_FORMAT_BASE "GBR"
#define SPA_TYPE_VIDEO_FORMAT__GBR_10BE		SPA_TYPE_VIDEO_FORMAT_BASE "GBR_10BE"
#define SPA_TYPE_VIDEO_FORMAT__GBR_10LE		SPA_TYPE_VIDEO_FORMAT_BASE "GBR_10LE"
#define SPA_TYPE_VIDEO_FORMAT__NV16		SPA_TYPE_VIDEO_FORMAT_BASE "NV16"
#define SPA_TYPE_VIDEO_FORMAT__NV24		SPA_TYPE_VIDEO_FORMAT_BASE "NV24"
#define SPA_TYPE_VIDEO_FORMAT__NV12_64Z32	SPA_TYPE_VIDEO_FORMAT_BASE "NV12_64Z32"
#define SPA_TYPE_VIDEO_FORMAT__A420_10BE	SPA_TYPE_VIDEO_FORMAT_BASE "A420_10BE"
#define SPA_TYPE_VIDEO_FORMAT__A420_10LE	SPA_TYPE_VIDEO_FORMAT_BASE "A420_10LE"
#define SPA_TYPE_VIDEO_FORMAT__A422_10BE	SPA_TYPE_VIDEO_FORMAT_BASE "A422_10BE"
#define SPA_TYPE_VIDEO_FORMAT__A422_10LE	SPA_TYPE_VIDEO_FORMAT_BASE "A422_10LE"
#define SPA_TYPE_VIDEO_FORMAT__A444_10BE	SPA_TYPE_VIDEO_FORMAT_BASE "A444_10BE"
#define SPA_TYPE_VIDEO_FORMAT__A444_10LE	SPA_TYPE_VIDEO_FORMAT_BASE "A444_10LE"
#define SPA_TYPE_VIDEO_FORMAT__NV61		SPA_TYPE_VIDEO_FORMAT_BASE "NV61"
#define SPA_TYPE_VIDEO_FORMAT__P010_10BE	SPA_TYPE_VIDEO_FORMAT_BASE "P010_10BE"
#define SPA_TYPE_VIDEO_FORMAT__P010_10LE	SPA_TYPE_VIDEO_FORMAT_BASE "P010_10LE"
#define SPA_TYPE_VIDEO_FORMAT__IYU2		SPA_TYPE_VIDEO_FORMAT_BASE "IYU2"
#define SPA_TYPE_VIDEO_FORMAT__VYUY		SPA_TYPE_VIDEO_FORMAT_BASE "VYUY"
#define SPA_TYPE_VIDEO_FORMAT__GBRA		SPA_TYPE_VIDEO_FORMAT_BASE "GBRA"
#define SPA_TYPE_VIDEO_FORMAT__GBRA_10BE	SPA_TYPE_VIDEO_FORMAT_BASE "GBRA_10BE"
#define SPA_TYPE_VIDEO_FORMAT__GBRA_10LE	SPA_TYPE_VIDEO_FORMAT_BASE "GBRA_10LE"
#define SPA_TYPE_VIDEO_FORMAT__GBR_12BE		SPA_TYPE_VIDEO_FORMAT_BASE "GBR_12BE"
#define SPA_TYPE_VIDEO_FORMAT__GBR_12LE		SPA_TYPE_VIDEO_FORMAT_BASE "GBR_12LE"
#define SPA_TYPE_VIDEO_FORMAT__GBRA_12BE	SPA_TYPE_VIDEO_FORMAT_BASE "GBRA_12BE"
#define SPA_TYPE_VIDEO_FORMAT__GBRA_12LE	SPA_TYPE_VIDEO_FORMAT_BASE "GBRA_12LE"
#define SPA_TYPE_VIDEO_FORMAT__I420_12BE	SPA_TYPE_VIDEO_FORMAT_BASE "I420_12BE"
#define SPA_TYPE_VIDEO_FORMAT__I420_12LE	SPA_TYPE_VIDEO_FORMAT_BASE "I420_12LE"
#define SPA_TYPE_VIDEO_FORMAT__I422_12BE	SPA_TYPE_VIDEO_FORMAT_BASE "I422_12BE"
#define SPA_TYPE_VIDEO_FORMAT__I422_12LE	SPA_TYPE_VIDEO_FORMAT_BASE "I422_12LE"
#define SPA_TYPE_VIDEO_FORMAT__Y444_12BE	SPA_TYPE_VIDEO_FORMAT_BASE "Y444_12BE"
#define SPA_TYPE_VIDEO_FORMAT__Y444_12LE	SPA_TYPE_VIDEO_FORMAT_BASE "Y444_12LE"

/**
 * spa_video_flags:
 * @SPA_VIDEO_FLAG_NONE: no flags
 * @SPA_VIDEO_FLAG_VARIABLE_FPS: a variable fps is selected, fps_n and fps_d
 *     denote the maximum fps of the video
 * @SPA_VIDEO_FLAG_PREMULTIPLIED_ALPHA: Each color has been scaled by the alpha
 *     value.
 *
 * Extra video flags
 */
enum spa_video_flags {
	SPA_VIDEO_FLAG_NONE = 0,
	SPA_VIDEO_FLAG_VARIABLE_FPS = (1 << 0),
	SPA_VIDEO_FLAG_PREMULTIPLIED_ALPHA = (1 << 1)
};

/**
 * spa_video_interlace_mode:
 * @SPA_VIDEO_INTERLACE_MODE_PROGRESSIVE: all frames are progressive
 * @SPA_VIDEO_INTERLACE_MODE_INTERLEAVED: 2 fields are interleaved in one video
 *     frame. Extra buffer flags describe the field order.
 * @SPA_VIDEO_INTERLACE_MODE_MIXED: frames contains both interlaced and
 *     progressive video, the buffer flags describe the frame and fields.
 * @SPA_VIDEO_INTERLACE_MODE_FIELDS: 2 fields are stored in one buffer, use the
 *     frame ID to get access to the required field. For multiview (the
 *     'views' property > 1) the fields of view N can be found at frame ID
 *     (N * 2) and (N * 2) + 1.
 *     Each field has only half the amount of lines as noted in the
 *     height property. This mode requires multiple spa_data
 *     to describe the fields.
 *
 * The possible values of the #spa_video_interlace_mode describing the interlace
 * mode of the stream.
 */
enum spa_video_interlace_mode {
	SPA_VIDEO_INTERLACE_MODE_PROGRESSIVE = 0,
	SPA_VIDEO_INTERLACE_MODE_INTERLEAVED,
	SPA_VIDEO_INTERLACE_MODE_MIXED,
	SPA_VIDEO_INTERLACE_MODE_FIELDS
};

/**
 * spa_video_info_raw:
 * @format: the format
 * @size: the frame size of the video
 * @framerate: the framerate of the video 0/1 means variable rate
 * @max_framerate: the maximum framerate of the video. This is only valid when
 *             @framerate is 0/1
 * @views: the number of views in this video
 * @interlace_mode: the interlace mode
 * @pixel_aspect_ratio: The pixel aspect ratio
 * @multiview_mode: multiview mode
 * @multiview_flags: multiview flags
 * @chroma_site: the chroma siting
 * @color_range: the color range. This is the valid range for the samples.
 *         It is used to convert the samples to Y'PbPr values.
 * @color_matrix: the color matrix. Used to convert between Y'PbPr and
 *         non-linear RGB (R'G'B')
 * @transfer_function: the transfer function. used to convert between R'G'B' and RGB
 * @color_primaries: color primaries. used to convert between R'G'B' and CIE XYZ
 */
struct spa_video_info_raw {
	uint32_t format;
	struct spa_rectangle size;
	struct spa_fraction framerate;
	struct spa_fraction max_framerate;
	uint32_t views;
	enum spa_video_interlace_mode interlace_mode;
	struct spa_fraction pixel_aspect_ratio;
	enum spa_video_multiview_mode multiview_mode;
	enum spa_video_multiview_flags multiview_flags;
	enum spa_video_chroma_site chroma_site;
	enum spa_video_color_range color_range;
	enum spa_video_color_matrix color_matrix;
	enum spa_video_transfer_function transfer_function;
	enum spa_video_color_primaries color_primaries;
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __SPA_VIDEO_RAW_H__ */
