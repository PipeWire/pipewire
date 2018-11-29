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

#ifndef __SPA_PARAM_TYPES_H__
#define __SPA_PARAM_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/defs.h>
#include <spa/param/props.h>
#include <spa/param/format.h>
#include <spa/node/io.h>

/* base for parameter object enumerations */
#define SPA_TYPE__ParamId		SPA_TYPE_ENUM_BASE "ParamId"
#define SPA_TYPE_PARAM_ID_BASE		SPA_TYPE__ParamId ":"

static const struct spa_type_info spa_type_param[] = {
	{ SPA_PARAM_Invalid, SPA_TYPE_PARAM_ID_BASE "Invalid", SPA_TYPE_Int, },
	{ SPA_PARAM_List,  SPA_TYPE_PARAM_ID_BASE "List", SPA_TYPE_Int, },
	{ SPA_PARAM_PropInfo, SPA_TYPE_PARAM_ID_BASE "PropInfo", SPA_TYPE_Int, },
	{ SPA_PARAM_Props, SPA_TYPE_PARAM_ID_BASE "Props", SPA_TYPE_Int, },
	{ SPA_PARAM_EnumFormat, SPA_TYPE_PARAM_ID_BASE "EnumFormat", SPA_TYPE_Int, },
	{ SPA_PARAM_Format, SPA_TYPE_PARAM_ID_BASE "Format", SPA_TYPE_Int, },
	{ SPA_PARAM_Buffers, SPA_TYPE_PARAM_ID_BASE "Buffers", SPA_TYPE_Int, },
	{ SPA_PARAM_Meta, SPA_TYPE_PARAM_ID_BASE "Meta", SPA_TYPE_Int, },
	{ SPA_PARAM_IO, SPA_TYPE_PARAM_ID_BASE "IO", SPA_TYPE_Int, },
	{ SPA_PARAM_Profile, SPA_TYPE_PARAM_ID_BASE "Profile", SPA_TYPE_Int, },
	{ 0, NULL, },
};

/* base for parameter objects */
#define SPA_TYPE__Param			SPA_TYPE_OBJECT_BASE "Param"
#define SPA_TYPE_PARAM_BASE		SPA_TYPE__Param ":"

/* object with supported parameter id */
#define SPA_TYPE_PARAM__List		SPA_TYPE_PARAM_BASE "List"
#define SPA_TYPE_PARAM_LIST_BASE	SPA_TYPE_PARAM__List ":"

static const struct spa_type_info spa_type_param_list[] = {
	{ SPA_PARAM_LIST_START, SPA_TYPE_PARAM_LIST_BASE,  SPA_TYPE_Id, spa_type_param },
	{ SPA_PARAM_LIST_id, SPA_TYPE_PARAM_LIST_BASE "id",  SPA_TYPE_Id, spa_type_param },
	{ 0, NULL, },
};

#define SPA_TYPE__Props			SPA_TYPE_PARAM_BASE "Props"
#define SPA_TYPE_PROPS_BASE		SPA_TYPE__Props ":"

static const struct spa_type_info spa_type_props[] = {
	{ SPA_PROP_START, SPA_TYPE_PROPS_BASE, SPA_TYPE_Id, spa_type_param, },
	{ SPA_PROP_unknown, SPA_TYPE_PROPS_BASE "unknown", SPA_TYPE_None, },
	{ SPA_PROP_device, SPA_TYPE_PROPS_BASE "device", SPA_TYPE_String, },
	{ SPA_PROP_deviceName, SPA_TYPE_PROPS_BASE "deviceName", SPA_TYPE_String, },
	{ SPA_PROP_deviceFd, SPA_TYPE_PROPS_BASE "deviceFd", SPA_TYPE_Fd, },
	{ SPA_PROP_card, SPA_TYPE_PROPS_BASE "card", SPA_TYPE_String, },
	{ SPA_PROP_cardName, SPA_TYPE_PROPS_BASE "cardName", SPA_TYPE_String, },
	{ SPA_PROP_minLatency, SPA_TYPE_PROPS_BASE "minLatency", SPA_TYPE_Int, },
	{ SPA_PROP_maxLatency, SPA_TYPE_PROPS_BASE "maxLatency", SPA_TYPE_Int, },
	{ SPA_PROP_periods, SPA_TYPE_PROPS_BASE "periods", SPA_TYPE_Int, },
	{ SPA_PROP_periodSize, SPA_TYPE_PROPS_BASE "periodSize", SPA_TYPE_Int, },
	{ SPA_PROP_periodEvent, SPA_TYPE_PROPS_BASE "periodEvent", SPA_TYPE_Bool, },
	{ SPA_PROP_live, SPA_TYPE_PROPS_BASE "live", SPA_TYPE_Bool, },
	{ SPA_PROP_rate, SPA_TYPE_PROPS_BASE "rate", SPA_TYPE_Double, },

	{ SPA_PROP_waveType, SPA_TYPE_PROPS_BASE "waveType", SPA_TYPE_Id, },
	{ SPA_PROP_frequency, SPA_TYPE_PROPS_BASE "frequency", SPA_TYPE_Int, },
	{ SPA_PROP_volume, SPA_TYPE_PROPS_BASE "volume", SPA_TYPE_Float, },
	{ SPA_PROP_mute, SPA_TYPE_PROPS_BASE "mute", SPA_TYPE_Bool, },
	{ SPA_PROP_patternType, SPA_TYPE_PROPS_BASE "patternType", SPA_TYPE_Id, },
	{ SPA_PROP_ditherType, SPA_TYPE_PROPS_BASE "ditherType", SPA_TYPE_Id, },
	{ SPA_PROP_truncate, SPA_TYPE_PROPS_BASE "truncate", SPA_TYPE_Bool, },

	{ SPA_PROP_brightness, SPA_TYPE_PROPS_BASE "brightness", SPA_TYPE_Int, },
	{ SPA_PROP_contrast, SPA_TYPE_PROPS_BASE "contrast", SPA_TYPE_Int, },
	{ SPA_PROP_saturation, SPA_TYPE_PROPS_BASE "saturation", SPA_TYPE_Int, },
	{ SPA_PROP_hue, SPA_TYPE_PROPS_BASE "hue", SPA_TYPE_Int, },
	{ SPA_PROP_gamma, SPA_TYPE_PROPS_BASE "gamma", SPA_TYPE_Int, },
	{ SPA_PROP_exposure, SPA_TYPE_PROPS_BASE "exposure", SPA_TYPE_Int, },
	{ SPA_PROP_gain, SPA_TYPE_PROPS_BASE "gain", SPA_TYPE_Int, },
	{ SPA_PROP_sharpness, SPA_TYPE_PROPS_BASE "sharpness", SPA_TYPE_Int, },
	{ 0, NULL, },
};

/** Enum Property info */
#define SPA_TYPE__PropInfo		SPA_TYPE_PARAM_BASE "PropInfo"
#define SPA_TYPE_PROP_INFO_BASE		SPA_TYPE__PropInfo ":"

static const struct spa_type_info spa_type_prop_info[] = {
	{ SPA_PROP_INFO_START, SPA_TYPE_PROP_INFO_BASE, SPA_TYPE_Id, spa_type_param, },
	{ SPA_PROP_INFO_id, SPA_TYPE_PROP_INFO_BASE "id",  SPA_TYPE_Id, spa_type_props },
	{ SPA_PROP_INFO_name, SPA_TYPE_PROP_INFO_BASE "name",  SPA_TYPE_String, },
	{ SPA_PROP_INFO_type, SPA_TYPE_PROP_INFO_BASE "type",  SPA_TYPE_Id, },
	{ SPA_PROP_INFO_labels, SPA_TYPE_PROP_INFO_BASE "labels",  SPA_TYPE_Struct, },
	{ 0, NULL, },
};

#define SPA_TYPE_PARAM__Meta			SPA_TYPE_PARAM_BASE "Meta"
#define SPA_TYPE_PARAM_META_BASE		SPA_TYPE_PARAM__Meta ":"

static const struct spa_type_info spa_type_param_meta[] = {
	{ SPA_PARAM_META_START, SPA_TYPE_PARAM_META_BASE, SPA_TYPE_Id, spa_type_param, },
	{ SPA_PARAM_META_type, SPA_TYPE_PARAM_META_BASE "type",  SPA_TYPE_Id, },
	{ SPA_PARAM_META_size, SPA_TYPE_PARAM_META_BASE "size",  SPA_TYPE_Int, },
	{ 0, NULL, },
};

/** Base for parameters that describe IO areas to exchange data,
 * control and properties with a node.
 */
#define SPA_TYPE_PARAM__IO		SPA_TYPE_PARAM_BASE "IO"
#define SPA_TYPE_PARAM_IO_BASE		SPA_TYPE_PARAM__IO ":"

static const struct spa_type_info spa_type_param_io[] = {
	{ SPA_PARAM_IO_START, SPA_TYPE_PARAM_IO_BASE, SPA_TYPE_Id, spa_type_param, },
	{ SPA_PARAM_IO_id, SPA_TYPE_PARAM_IO_BASE "id",  SPA_TYPE_Id, spa_type_io },
	{ SPA_PARAM_IO_size, SPA_TYPE_PARAM_IO_BASE "size",  SPA_TYPE_Int, },
	{ 0, NULL, },
};

#define SPA_TYPE__Format		SPA_TYPE_PARAM_BASE "Format"
#define SPA_TYPE_FORMAT_BASE		SPA_TYPE__Format ":"

#define SPA_TYPE__MediaType		SPA_TYPE_ENUM_BASE "MediaType"
#define SPA_TYPE_MEDIA_TYPE_BASE	SPA_TYPE__MediaType ":"

#include <spa/param/audio/type-info.h>
#include <spa/param/video/type-info.h>

static const struct spa_type_info spa_type_media_type[] = {
	{ SPA_MEDIA_TYPE_unknown, SPA_TYPE_MEDIA_TYPE_BASE "unknown", SPA_TYPE_Int, },
	{ SPA_MEDIA_TYPE_audio,   SPA_TYPE_MEDIA_TYPE_BASE "audio",   SPA_TYPE_Int, },
	{ SPA_MEDIA_TYPE_video,   SPA_TYPE_MEDIA_TYPE_BASE "video",   SPA_TYPE_Int, },
	{ SPA_MEDIA_TYPE_image,   SPA_TYPE_MEDIA_TYPE_BASE "image",   SPA_TYPE_Int, },
	{ SPA_MEDIA_TYPE_binary,  SPA_TYPE_MEDIA_TYPE_BASE "binary",  SPA_TYPE_Int, },
	{ SPA_MEDIA_TYPE_stream,  SPA_TYPE_MEDIA_TYPE_BASE "stream",  SPA_TYPE_Int, },
	{ 0, NULL, },
};

#define SPA_TYPE__MediaSubtype		SPA_TYPE_ENUM_BASE "MediaSubtype"
#define SPA_TYPE_MEDIA_SUBTYPE_BASE	SPA_TYPE__MediaSubtype ":"

static const struct spa_type_info spa_type_media_subtype[] = {
	{ SPA_MEDIA_SUBTYPE_unknown, SPA_TYPE_MEDIA_SUBTYPE_BASE "unknown", SPA_TYPE_Int, },
	/* generic subtypes */
	{ SPA_MEDIA_SUBTYPE_raw, SPA_TYPE_MEDIA_SUBTYPE_BASE "raw", SPA_TYPE_Int, },
	/* audio subtypes */
	{ SPA_MEDIA_SUBTYPE_mp3, SPA_TYPE_MEDIA_SUBTYPE_BASE "mp3", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_aac, SPA_TYPE_MEDIA_SUBTYPE_BASE "aac", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_vorbis, SPA_TYPE_MEDIA_SUBTYPE_BASE "vorbis", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_wma, SPA_TYPE_MEDIA_SUBTYPE_BASE "wma", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_ra, SPA_TYPE_MEDIA_SUBTYPE_BASE "ra", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_sbc, SPA_TYPE_MEDIA_SUBTYPE_BASE "sbc", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_adpcm, SPA_TYPE_MEDIA_SUBTYPE_BASE "adpcm", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_g723, SPA_TYPE_MEDIA_SUBTYPE_BASE "g723", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_g726, SPA_TYPE_MEDIA_SUBTYPE_BASE "g726", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_g729, SPA_TYPE_MEDIA_SUBTYPE_BASE "g729", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_amr, SPA_TYPE_MEDIA_SUBTYPE_BASE "amr", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_gsm, SPA_TYPE_MEDIA_SUBTYPE_BASE "gsm", SPA_TYPE_Int, },
	/* video subtypes */
	{ SPA_MEDIA_SUBTYPE_h264, SPA_TYPE_MEDIA_SUBTYPE_BASE "h264", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_mjpg, SPA_TYPE_MEDIA_SUBTYPE_BASE "mjpg", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_dv, SPA_TYPE_MEDIA_SUBTYPE_BASE "dv", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_mpegts, SPA_TYPE_MEDIA_SUBTYPE_BASE "mpegts", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_h263, SPA_TYPE_MEDIA_SUBTYPE_BASE "h263", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_mpeg1, SPA_TYPE_MEDIA_SUBTYPE_BASE "mpeg1", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_mpeg2, SPA_TYPE_MEDIA_SUBTYPE_BASE "mpeg2", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_mpeg4, SPA_TYPE_MEDIA_SUBTYPE_BASE "mpeg4", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_xvid, SPA_TYPE_MEDIA_SUBTYPE_BASE "xvid", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_vc1, SPA_TYPE_MEDIA_SUBTYPE_BASE "vc1", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_vp8, SPA_TYPE_MEDIA_SUBTYPE_BASE "vp8", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_vp9, SPA_TYPE_MEDIA_SUBTYPE_BASE "vp9", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_jpeg, SPA_TYPE_MEDIA_SUBTYPE_BASE "jpeg", SPA_TYPE_Int, },
	{ SPA_MEDIA_SUBTYPE_bayer, SPA_TYPE_MEDIA_SUBTYPE_BASE "bayer", SPA_TYPE_Int, },

	{ SPA_MEDIA_SUBTYPE_midi, SPA_TYPE_MEDIA_SUBTYPE_BASE "midi", SPA_TYPE_Int, },
	{ 0, NULL, },
};

#define SPA_TYPE__FormatAudio		SPA_TYPE_FORMAT_BASE "Audio"
#define SPA_TYPE_FORMAT_AUDIO_BASE	SPA_TYPE__FormatAudio ":"

#define SPA_TYPE__FormatVideo		SPA_TYPE_FORMAT_BASE "Video"
#define SPA_TYPE_FORMAT_VIDEO_BASE	SPA_TYPE__FormatVideo ":"

static const struct spa_type_info spa_type_format[] = {
	{ SPA_FORMAT_START, SPA_TYPE_FORMAT_BASE, SPA_TYPE_Id, spa_type_param, },

	{ SPA_FORMAT_mediaType, SPA_TYPE_FORMAT_BASE "mediaType", SPA_TYPE_Id,
		spa_type_media_type, },
	{ SPA_FORMAT_mediaSubtype, SPA_TYPE_FORMAT_BASE "mediaSubtype", SPA_TYPE_Id,
		spa_type_media_subtype, },

	{ SPA_FORMAT_AUDIO_format, SPA_TYPE_FORMAT_AUDIO_BASE "format", SPA_TYPE_Id,
		spa_type_audio_format },
	{ SPA_FORMAT_AUDIO_flags, SPA_TYPE_FORMAT_AUDIO_BASE "flags", SPA_TYPE_Id,
		spa_type_audio_flags },
	{ SPA_FORMAT_AUDIO_rate, SPA_TYPE_FORMAT_AUDIO_BASE "rate", SPA_TYPE_Int, },
	{ SPA_FORMAT_AUDIO_channels, SPA_TYPE_FORMAT_AUDIO_BASE "channels", SPA_TYPE_Int, },
	{ SPA_FORMAT_AUDIO_position, SPA_TYPE_FORMAT_AUDIO_BASE "position", SPA_TYPE_Id, spa_type_audio_channel },

	{ SPA_FORMAT_VIDEO_format, SPA_TYPE_FORMAT_VIDEO_BASE "format", SPA_TYPE_Id,
		spa_type_video_format, },
	{ SPA_FORMAT_VIDEO_size,  SPA_TYPE_FORMAT_VIDEO_BASE "size", SPA_TYPE_Rectangle, },
	{ SPA_FORMAT_VIDEO_framerate, SPA_TYPE_FORMAT_VIDEO_BASE "framerate", SPA_TYPE_Fraction, },
	{ SPA_FORMAT_VIDEO_maxFramerate, SPA_TYPE_FORMAT_VIDEO_BASE "maxFramerate", SPA_TYPE_Fraction, },
	{ SPA_FORMAT_VIDEO_views, SPA_TYPE_FORMAT_VIDEO_BASE "views", SPA_TYPE_Int, },
	{ SPA_FORMAT_VIDEO_interlaceMode, SPA_TYPE_FORMAT_VIDEO_BASE "interlaceMode", SPA_TYPE_Int, },
	{ SPA_FORMAT_VIDEO_pixelAspectRatio, SPA_TYPE_FORMAT_VIDEO_BASE "pixelAspectRatio", SPA_TYPE_Int, },
	{ SPA_FORMAT_VIDEO_multiviewMode, SPA_TYPE_FORMAT_VIDEO_BASE "multiviewMode", SPA_TYPE_Int, },
	{ SPA_FORMAT_VIDEO_multiviewFlags, SPA_TYPE_FORMAT_VIDEO_BASE "multiviewFlags", SPA_TYPE_Int, },
	{ SPA_FORMAT_VIDEO_chromaSite, SPA_TYPE_FORMAT_VIDEO_BASE "chromaSite", SPA_TYPE_Int, },
	{ SPA_FORMAT_VIDEO_colorRange, SPA_TYPE_FORMAT_VIDEO_BASE "colorRange", SPA_TYPE_Int, },
	{ SPA_FORMAT_VIDEO_colorMatrix, SPA_TYPE_FORMAT_VIDEO_BASE "colorMatrix", SPA_TYPE_Int, },
	{ SPA_FORMAT_VIDEO_transferFunction, SPA_TYPE_FORMAT_VIDEO_BASE "transferFunction", SPA_TYPE_Int, },
	{ SPA_FORMAT_VIDEO_colorPrimaries, SPA_TYPE_FORMAT_VIDEO_BASE "colorPrimaries", SPA_TYPE_Int, },
	{ SPA_FORMAT_VIDEO_profile, SPA_TYPE_FORMAT_VIDEO_BASE "profile", SPA_TYPE_Int, },
	{ SPA_FORMAT_VIDEO_level, SPA_TYPE_FORMAT_VIDEO_BASE "level", SPA_TYPE_Int, },
	{ SPA_FORMAT_VIDEO_streamFormat, SPA_TYPE_FORMAT_VIDEO_BASE "streamFormat", SPA_TYPE_Int, },
	{ SPA_FORMAT_VIDEO_alignment, SPA_TYPE_FORMAT_VIDEO_BASE "alignment", SPA_TYPE_Int, },
	{ 0, NULL, },
};

#define SPA_TYPE_PARAM__Buffers			SPA_TYPE_PARAM_BASE "Buffers"
#define SPA_TYPE_PARAM_BUFFERS_BASE		SPA_TYPE_PARAM__Buffers ":"

#define SPA_TYPE_PARAM__BlockInfo		SPA_TYPE_PARAM_BUFFERS_BASE "BlockInfo"
#define SPA_TYPE_PARAM_BLOCK_INFO_BASE		SPA_TYPE_PARAM__BlockInfo ":"

static const struct spa_type_info spa_type_param_buffers[] = {
	{ SPA_PARAM_BUFFERS_START,   SPA_TYPE_PARAM_BUFFERS_BASE, SPA_TYPE_Id, spa_type_param, },
	{ SPA_PARAM_BUFFERS_buffers, SPA_TYPE_PARAM_BUFFERS_BASE "buffers",  SPA_TYPE_Int, },
	{ SPA_PARAM_BUFFERS_blocks,  SPA_TYPE_PARAM_BUFFERS_BASE "blocks",   SPA_TYPE_Int, },
	{ SPA_PARAM_BUFFERS_size,    SPA_TYPE_PARAM_BLOCK_INFO_BASE "size",   SPA_TYPE_Int, },
	{ SPA_PARAM_BUFFERS_stride,  SPA_TYPE_PARAM_BLOCK_INFO_BASE "stride", SPA_TYPE_Int, },
	{ SPA_PARAM_BUFFERS_align,   SPA_TYPE_PARAM_BLOCK_INFO_BASE "align",  SPA_TYPE_Int, },
	{ 0, NULL, },
};

#define SPA_TYPE_PARAM__Profile		SPA_TYPE_PARAM_BASE "Profile"
#define SPA_TYPE_PARAM_PROFILE_BASE	SPA_TYPE_PARAM__Profile ":"

static const struct spa_type_info spa_type_param_profile[] = {
	{ SPA_PARAM_PROFILE_START, SPA_TYPE_PARAM_PROFILE_BASE, SPA_TYPE_Id, spa_type_param, },
	{ SPA_PARAM_PROFILE_id, SPA_TYPE_PARAM_PROFILE_BASE "id",  SPA_TYPE_Int,  },
	{ SPA_PARAM_PROFILE_name, SPA_TYPE_PARAM_PROFILE_BASE "name",  SPA_TYPE_String,  },
	{ SPA_PARAM_PROFILE_direction, SPA_TYPE_PARAM_PROFILE_BASE "direction",  SPA_TYPE_Id, spa_type_direction },
	{ SPA_PARAM_PROFILE_format, SPA_TYPE_PARAM_PROFILE_BASE "format",  SPA_TYPE_Object, NULL, },
	{ 0, NULL, },
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_TYPES_H__ */
