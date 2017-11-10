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

#ifndef __SPA_VIDEO_MULTIVIEW_H__
#define __SPA_VIDEO_MULTIVIEW_H__

#ifdef __cplusplus
extern "C" {
#endif

/**
 * spa_video_multiview_mode:
 * @SPA_VIDEO_MULTIVIEW_MODE_NONE: A special value indicating
 * no multiview information. Used in spa_video_info and other places to
 * indicate that no specific multiview handling has been requested or
 * provided. This value is never carried on caps.
 * @SPA_VIDEO_MULTIVIEW_MODE_MONO: All frames are monoscopic.
 * @SPA_VIDEO_MULTIVIEW_MODE_LEFT: All frames represent a left-eye view.
 * @SPA_VIDEO_MULTIVIEW_MODE_RIGHT: All frames represent a right-eye view.
 * @SPA_VIDEO_MULTIVIEW_MODE_SIDE_BY_SIDE: Left and right eye views are
 * provided in the left and right half of the frame respectively.
 * @SPA_VIDEO_MULTIVIEW_MODE_SIDE_BY_SIDE_QUINCUNX: Left and right eye
 * views are provided in the left and right half of the frame, but
 * have been sampled using quincunx method, with half-pixel offset
 * between the 2 views.
 * @SPA_VIDEO_MULTIVIEW_MODE_COLUMN_INTERLEAVED: Alternating vertical
 * columns of pixels represent the left and right eye view respectively.
 * @SPA_VIDEO_MULTIVIEW_MODE_ROW_INTERLEAVED: Alternating horizontal
 * rows of pixels represent the left and right eye view respectively.
 * @SPA_VIDEO_MULTIVIEW_MODE_TOP_BOTTOM: The top half of the frame
 * contains the left eye, and the bottom half the right eye.
 * @SPA_VIDEO_MULTIVIEW_MODE_CHECKERBOARD: Pixels are arranged with
 * alternating pixels representing left and right eye views in a
 * checkerboard fashion.
 * @SPA_VIDEO_MULTIVIEW_MODE_FRAME_BY_FRAME: Left and right eye views
 * are provided in separate frames alternately.
 * @SPA_VIDEO_MULTIVIEW_MODE_MULTIVIEW_FRAME_BY_FRAME: Multiple
 * independent views are provided in separate frames in sequence.
 * This method only applies to raw video buffers at the moment.
 * Specific view identification is via the #spa_video_multiview_meta
 * on raw video buffers.
 * @SPA_VIDEO_MULTIVIEW_MODE_SEPARATED: Multiple views are
 * provided as separate #spa_data framebuffers attached to each
 * #spa_buffer, described by the #spa_video_multiview_meta
 *
 * All possible stereoscopic 3D and multiview representations.
 * In conjunction with #soa_video_multiview_flags, describes how
 * multiview content is being transported in the stream.
 */
enum spa_video_multiview_mode {
	SPA_VIDEO_MULTIVIEW_MODE_NONE = -1,
	SPA_VIDEO_MULTIVIEW_MODE_MONO = 0,
	/* Single view modes */
	SPA_VIDEO_MULTIVIEW_MODE_LEFT,
	SPA_VIDEO_MULTIVIEW_MODE_RIGHT,
	/* Stereo view modes */
	SPA_VIDEO_MULTIVIEW_MODE_SIDE_BY_SIDE,
	SPA_VIDEO_MULTIVIEW_MODE_SIDE_BY_SIDE_QUINCUNX,
	SPA_VIDEO_MULTIVIEW_MODE_COLUMN_INTERLEAVED,
	SPA_VIDEO_MULTIVIEW_MODE_ROW_INTERLEAVED,
	SPA_VIDEO_MULTIVIEW_MODE_TOP_BOTTOM,
	SPA_VIDEO_MULTIVIEW_MODE_CHECKERBOARD,
	/* Padding for new frame packing modes */

	SPA_VIDEO_MULTIVIEW_MODE_FRAME_BY_FRAME = 32,
	/* Multivew mode(s) */
	SPA_VIDEO_MULTIVIEW_MODE_MULTIVIEW_FRAME_BY_FRAME,
	SPA_VIDEO_MULTIVIEW_MODE_SEPARATED
	    /* future expansion for annotated modes */
};

/**
 * spa_video_multiview_flags:
 * @SPA_VIDEO_MULTIVIEW_FLAGS_NONE: No flags
 * @SPA_VIDEO_MULTIVIEW_FLAGS_RIGHT_VIEW_FIRST: For stereo streams, the
 *     normal arrangement of left and right views is reversed.
 * @SPA_VIDEO_MULTIVIEW_FLAGS_LEFT_FLIPPED: The left view is vertically
 *     mirrored.
 * @SPA_VIDEO_MULTIVIEW_FLAGS_LEFT_FLOPPED: The left view is horizontally
 *     mirrored.
 * @SPA_VIDEO_MULTIVIEW_FLAGS_RIGHT_FLIPPED: The right view is
 *     vertically mirrored.
 * @SPA_VIDEO_MULTIVIEW_FLAGS_RIGHT_FLOPPED: The right view is
 *     horizontally mirrored.
 * @SPA_VIDEO_MULTIVIEW_FLAGS_HALF_ASPECT: For frame-packed
 *     multiview modes, indicates that the individual
 *     views have been encoded with half the true width or height
 *     and should be scaled back up for display. This flag
 *     is used for overriding input layout interpretation
 *     by adjusting pixel-aspect-ratio.
 *     For side-by-side, column interleaved or checkerboard packings, the
 *     pixel width will be doubled. For row interleaved and top-bottom
 *     encodings, pixel height will be doubled.
 * @SPA_VIDEO_MULTIVIEW_FLAGS_MIXED_MONO: The video stream contains both
 *     mono and multiview portions, signalled on each buffer by the
 *     absence or presence of the @SPA_VIDEO_BUFFER_FLAG_MULTIPLE_VIEW
 *     buffer flag.
 *
 * spa_video_multiview_flags are used to indicate extra properties of a
 * stereo/multiview stream beyond the frame layout and buffer mapping
 * that is conveyed in the #spa_video_multiview_mode.
 */
enum spa_video_multiview_flags {
	SPA_VIDEO_MULTIVIEW_FLAGS_NONE = 0,
	SPA_VIDEO_MULTIVIEW_FLAGS_RIGHT_VIEW_FIRST = (1 << 0),
	SPA_VIDEO_MULTIVIEW_FLAGS_LEFT_FLIPPED = (1 << 1),
	SPA_VIDEO_MULTIVIEW_FLAGS_LEFT_FLOPPED = (1 << 2),
	SPA_VIDEO_MULTIVIEW_FLAGS_RIGHT_FLIPPED = (1 << 3),
	SPA_VIDEO_MULTIVIEW_FLAGS_RIGHT_FLOPPED = (1 << 4),
	SPA_VIDEO_MULTIVIEW_FLAGS_HALF_ASPECT = (1 << 14),
	SPA_VIDEO_MULTIVIEW_FLAGS_MIXED_MONO = (1 << 15)
};


#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* __SPA_VIDEO_MULTIVIEW_H__ */
