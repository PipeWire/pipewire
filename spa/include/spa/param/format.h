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

/** media type for SPA_TYPE_OBJECT_Format */
enum spa_media_type {
	SPA_MEDIA_TYPE_audio,
	SPA_MEDIA_TYPE_video,
	SPA_MEDIA_TYPE_image,
	SPA_MEDIA_TYPE_binary,
	SPA_MEDIA_TYPE_stream,
};

/** media subtype for SPA_TYPE_OBJECT_Format */
enum spa_media_subtype {
	SPA_MEDIA_SUBTYPE_START_Generic,
	SPA_MEDIA_SUBTYPE_raw,

	SPA_MEDIA_SUBTYPE_START_Audio	= 0x10000,
	SPA_MEDIA_SUBTYPE_mp3,
	SPA_MEDIA_SUBTYPE_aac,
	SPA_MEDIA_SUBTYPE_vorbis,
	SPA_MEDIA_SUBTYPE_wma,
	SPA_MEDIA_SUBTYPE_ra,
	SPA_MEDIA_SUBTYPE_sbc,
	SPA_MEDIA_SUBTYPE_adpcm,
	SPA_MEDIA_SUBTYPE_g723,
	SPA_MEDIA_SUBTYPE_g726,
	SPA_MEDIA_SUBTYPE_g729,
	SPA_MEDIA_SUBTYPE_amr,
	SPA_MEDIA_SUBTYPE_gsm,

	SPA_MEDIA_SUBTYPE_START_Video	= 0x20000,
	SPA_MEDIA_SUBTYPE_h264,
	SPA_MEDIA_SUBTYPE_mjpg,
	SPA_MEDIA_SUBTYPE_dv,
	SPA_MEDIA_SUBTYPE_mpegts,
	SPA_MEDIA_SUBTYPE_h263,
	SPA_MEDIA_SUBTYPE_mpeg1,
	SPA_MEDIA_SUBTYPE_mpeg2,
	SPA_MEDIA_SUBTYPE_mpeg4,
	SPA_MEDIA_SUBTYPE_xvid,
	SPA_MEDIA_SUBTYPE_vc1,
	SPA_MEDIA_SUBTYPE_vp8,
	SPA_MEDIA_SUBTYPE_vp9,
	SPA_MEDIA_SUBTYPE_jpeg,
	SPA_MEDIA_SUBTYPE_bayer,

	SPA_MEDIA_SUBTYPE_START_Image	= 0x30000,

	SPA_MEDIA_SUBTYPE_START_Binary	= 0x40000,

	SPA_MEDIA_SUBTYPE_START_Stream	= 0x50000,
	SPA_MEDIA_SUBTYPE_midi,
};

/** properties for audio SPA_TYPE_OBJECT_Format */
enum spa_format {
	SPA_FORMAT_START,		/**< id of the object, one of enum spa_param_type */

	SPA_FORMAT_MediaType,		/**< first int in object, one of enum spa_media_type */
	SPA_FORMAT_MediaSubtype,	/**< second int in object, one of enum spa_media_subtype */

	/* Audio format keys */
	SPA_FORMAT_START_AUDIO,
	SPA_FORMAT_AUDIO_format,
	SPA_FORMAT_AUDIO_flags,
	SPA_FORMAT_AUDIO_layout,
	SPA_FORMAT_AUDIO_rate,
	SPA_FORMAT_AUDIO_channels,
	SPA_FORMAT_AUDIO_channelMask,

	/* Video Format keys */
	SPA_FORMAT_START_VIDEO = 0x10000,
	SPA_FORMAT_VIDEO_format,
	SPA_FORMAT_VIDEO_size,
	SPA_FORMAT_VIDEO_framerate,
	SPA_FORMAT_VIDEO_maxFramerate,
	SPA_FORMAT_VIDEO_views,
	SPA_FORMAT_VIDEO_interlaceMode,
	SPA_FORMAT_VIDEO_pixelAspectRatio,
	SPA_FORMAT_VIDEO_multiviewMode,
	SPA_FORMAT_VIDEO_multiviewFlags,
	SPA_FORMAT_VIDEO_chromaSite,
	SPA_FORMAT_VIDEO_colorRange,
	SPA_FORMAT_VIDEO_colorMatrix,
	SPA_FORMAT_VIDEO_transferFunction,
	SPA_FORMAT_VIDEO_colorPrimaries,
	SPA_FORMAT_VIDEO_profile,
	SPA_FORMAT_VIDEO_level,
	SPA_FORMAT_VIDEO_streamFormat,
	SPA_FORMAT_VIDEO_alignment,

	/* Image Format keys */
	SPA_FORMAT_START_IMAGE = 0x20000,
	/* Binary Format keys */
	SPA_FORMAT_START_BINARY = 0x30000,
	/* Stream Format keys */
	SPA_FORMAT_START_STREAM = 0x40000,
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_FORMAT_H__ */
