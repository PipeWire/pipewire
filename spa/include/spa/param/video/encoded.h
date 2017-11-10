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

#ifndef __SPA_VIDEO_ENCODED_H__
#define __SPA_VIDEO_ENCODED_H__

#ifdef __cplusplus
extern "C" {
#endif

struct spa_video_info_h264;
struct spa_video_info_mjpg;

#include <spa/param/format.h>
#include <spa/param/video/format.h>

enum spa_h264_stream_format {
	SPA_H264_STREAM_FORMAT_UNKNOWN = 0,
	SPA_H264_STREAM_FORMAT_AVC,
	SPA_H264_STREAM_FORMAT_AVC3,
	SPA_H264_STREAM_FORMAT_BYTESTREAM
};

enum spa_h264_alignment {
	SPA_H264_ALIGNMENT_UNKNOWN = 0,
	SPA_H264_ALIGNMENT_AU,
	SPA_H264_ALIGNMENT_NAL
};

struct spa_video_info_h264 {
	struct spa_rectangle size;
	struct spa_fraction framerate;
	struct spa_fraction max_framerate;
	enum spa_h264_stream_format stream_format;
	enum spa_h264_alignment alignment;
};

struct spa_video_info_mjpg {
	struct spa_rectangle size;
	struct spa_fraction framerate;
	struct spa_fraction max_framerate;
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __SPA_VIDEO_ENCODED_H__ */
