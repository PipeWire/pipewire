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

#ifndef SPA_PARAM_FORMAT_H
#define SPA_PARAM_FORMAT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/param/param.h>

/** media type for SPA_TYPE_OBJECT_Format */
enum spa_media_type {
	SPA_MEDIA_TYPE_unknown,
	SPA_MEDIA_TYPE_audio,
	SPA_MEDIA_TYPE_video,
	SPA_MEDIA_TYPE_image,
	SPA_MEDIA_TYPE_binary,
	SPA_MEDIA_TYPE_stream,
};

/** media subtype for SPA_TYPE_OBJECT_Format */
enum spa_media_subtype {
	SPA_MEDIA_SUBTYPE_unknown,
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

	SPA_FORMAT_mediaType,		/**< one of enum spa_media_type */
	SPA_FORMAT_mediaSubtype,	/**< one of enum spa_media_subtype */

	/* Audio format keys */
	SPA_FORMAT_START_Audio,
	SPA_FORMAT_AUDIO_format,	/**< audio format, one of enum spa_audio_format */
	SPA_FORMAT_AUDIO_flags,
	SPA_FORMAT_AUDIO_rate,
	SPA_FORMAT_AUDIO_channels,	/**< number of audio channels */
	SPA_FORMAT_AUDIO_position,	/**< channel positions one of enum spa_audio_position */

	/* Video Format keys */
	SPA_FORMAT_START_Video = 0x10000,
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
	SPA_FORMAT_START_Image = 0x20000,
	/* Binary Format keys */
	SPA_FORMAT_START_Binary = 0x30000,
	/* Stream Format keys */
	SPA_FORMAT_START_Stream = 0x40000,
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_PARAM_FORMAT_H */
