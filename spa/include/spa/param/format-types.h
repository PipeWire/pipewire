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

#ifndef __SPA_PARAM_FORMAT_TYPES_H__
#define __SPA_PARAM_FORMAT_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/param/format.h>

#define SPA_TYPE__Format		SPA_TYPE_PARAM_BASE "Format"
#define SPA_TYPE_FORMAT_BASE		SPA_TYPE__Format ":"

#define SPA_TYPE__MediaType		SPA_TYPE_ENUM_BASE "MediaType"
#define SPA_TYPE_MEDIA_TYPE_BASE	SPA_TYPE__MediaType ":"

#include <spa/param/audio/raw-types.h>
#include <spa/param/video/raw-types.h>

static const struct spa_type_info spa_type_media_type[] = {
	{ SPA_MEDIA_TYPE_audio, SPA_TYPE_MEDIA_TYPE_BASE "audio", SPA_ID_Int, },
	{ SPA_MEDIA_TYPE_video, SPA_TYPE_MEDIA_TYPE_BASE "video", SPA_ID_Int, },
	{ SPA_MEDIA_TYPE_image, SPA_TYPE_MEDIA_TYPE_BASE "image", SPA_ID_Int, },
	{ SPA_MEDIA_TYPE_binary, SPA_TYPE_MEDIA_TYPE_BASE "binary", SPA_ID_Int, },
	{ SPA_MEDIA_TYPE_stream, SPA_TYPE_MEDIA_TYPE_BASE "stream", SPA_ID_Int, },
	{ 0, NULL, },
};

#define SPA_TYPE__MediaSubtype		SPA_TYPE_ENUM_BASE "MediaSubtype"
#define SPA_TYPE_MEDIA_SUBTYPE_BASE	SPA_TYPE__MediaSubtype ":"

static const struct spa_type_info spa_type_media_subtype[] = {
	/* generic subtypes */
	{ SPA_MEDIA_SUBTYPE_raw, SPA_TYPE_MEDIA_SUBTYPE_BASE "raw", SPA_ID_Int, },
	/* audio subtypes */
	{ SPA_MEDIA_SUBTYPE_mp3, SPA_TYPE_MEDIA_SUBTYPE_BASE "mp3", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_aac, SPA_TYPE_MEDIA_SUBTYPE_BASE "aac", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_vorbis, SPA_TYPE_MEDIA_SUBTYPE_BASE "vorbis", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_wma, SPA_TYPE_MEDIA_SUBTYPE_BASE "wma", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_ra, SPA_TYPE_MEDIA_SUBTYPE_BASE "ra", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_sbc, SPA_TYPE_MEDIA_SUBTYPE_BASE "sbc", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_adpcm, SPA_TYPE_MEDIA_SUBTYPE_BASE "adpcm", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_g723, SPA_TYPE_MEDIA_SUBTYPE_BASE "g723", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_g726, SPA_TYPE_MEDIA_SUBTYPE_BASE "g726", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_g729, SPA_TYPE_MEDIA_SUBTYPE_BASE "g729", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_amr, SPA_TYPE_MEDIA_SUBTYPE_BASE "amr", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_gsm, SPA_TYPE_MEDIA_SUBTYPE_BASE "gsm", SPA_ID_Int, },
	/* video subtypes */
	{ SPA_MEDIA_SUBTYPE_h264, SPA_TYPE_MEDIA_SUBTYPE_BASE "h264", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_mjpg, SPA_TYPE_MEDIA_SUBTYPE_BASE "mjpg", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_dv, SPA_TYPE_MEDIA_SUBTYPE_BASE "dv", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_mpegts, SPA_TYPE_MEDIA_SUBTYPE_BASE "mpegts", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_h263, SPA_TYPE_MEDIA_SUBTYPE_BASE "h263", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_mpeg1, SPA_TYPE_MEDIA_SUBTYPE_BASE "mpeg1", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_mpeg2, SPA_TYPE_MEDIA_SUBTYPE_BASE "mpeg2", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_mpeg4, SPA_TYPE_MEDIA_SUBTYPE_BASE "mpeg4", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_xvid, SPA_TYPE_MEDIA_SUBTYPE_BASE "xvid", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_vc1, SPA_TYPE_MEDIA_SUBTYPE_BASE "vc1", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_vp8, SPA_TYPE_MEDIA_SUBTYPE_BASE "vp8", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_vp9, SPA_TYPE_MEDIA_SUBTYPE_BASE "vp9", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_jpeg, SPA_TYPE_MEDIA_SUBTYPE_BASE "jpeg", SPA_ID_Int, },
	{ SPA_MEDIA_SUBTYPE_bayer, SPA_TYPE_MEDIA_SUBTYPE_BASE "bayer", SPA_ID_Int, },

	{ SPA_MEDIA_SUBTYPE_midi, SPA_TYPE_MEDIA_SUBTYPE_BASE "midi", SPA_ID_Int, },
	{ 0, NULL, },
};

#define SPA_TYPE__FormatAudio		SPA_TYPE_FORMAT_BASE "Audio"
#define SPA_TYPE_FORMAT_AUDIO_BASE	SPA_TYPE__FormatAudio ":"

#define SPA_TYPE__FormatVideo		SPA_TYPE_FORMAT_BASE "Video"
#define SPA_TYPE_FORMAT_VIDEO_BASE	SPA_TYPE__FormatVideo ":"

static const struct spa_type_info spa_type_format[] = {
	{ SPA_FORMAT_AUDIO_format, SPA_TYPE_FORMAT_AUDIO_BASE "format", SPA_ID_Enum,
		spa_type_audio_format },
	{ SPA_FORMAT_AUDIO_flags, SPA_TYPE_FORMAT_AUDIO_BASE "flags", SPA_ID_Enum,
		spa_type_audio_flags },
	{ SPA_FORMAT_AUDIO_layout, SPA_TYPE_FORMAT_AUDIO_BASE "layout", SPA_ID_Enum,
		spa_type_audio_layout },
	{ SPA_FORMAT_AUDIO_rate, SPA_TYPE_FORMAT_AUDIO_BASE "rate", SPA_ID_Int, },
	{ SPA_FORMAT_AUDIO_channels, SPA_TYPE_FORMAT_AUDIO_BASE "channels", SPA_ID_Int, },
	{ SPA_FORMAT_AUDIO_channelMask, SPA_TYPE_FORMAT_AUDIO_BASE "channelMask", SPA_ID_Int, },

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

#endif /* __SPA_PARAM_FORMAT_TYPES_H__ */
