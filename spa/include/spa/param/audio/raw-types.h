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

#ifndef __SPA_AUDIO_RAW_TYPES_H__
#define __SPA_AUDIO_RAW_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/param/audio/raw.h>

#define SPA_TYPE__AudioFormat		SPA_TYPE_ENUM_BASE "AudioFormat"
#define SPA_TYPE_AUDIO_FORMAT_BASE	SPA_TYPE__AudioFormat ":"

static const struct spa_type_info spa_type_audio_format[] = {
	{ SPA_AUDIO_FORMAT_UNKNOWN, SPA_TYPE_AUDIO_FORMAT_BASE "UNKNOWN", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_ENCODED, SPA_TYPE_AUDIO_FORMAT_BASE "ENCODED", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_S8, SPA_TYPE_AUDIO_FORMAT_BASE "S8", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_U8, SPA_TYPE_AUDIO_FORMAT_BASE "U8", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_S16_LE, SPA_TYPE_AUDIO_FORMAT_BASE "S16LE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_S16_BE, SPA_TYPE_AUDIO_FORMAT_BASE "S16BE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_U16_LE, SPA_TYPE_AUDIO_FORMAT_BASE "U16LE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_U16_BE, SPA_TYPE_AUDIO_FORMAT_BASE "U16BE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_S24_32_LE, SPA_TYPE_AUDIO_FORMAT_BASE "S24_32LE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_S24_32_BE, SPA_TYPE_AUDIO_FORMAT_BASE "S24_32BE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_U24_32_LE, SPA_TYPE_AUDIO_FORMAT_BASE "U24_32LE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_U24_32_BE, SPA_TYPE_AUDIO_FORMAT_BASE "U24_32BE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_S32_LE, SPA_TYPE_AUDIO_FORMAT_BASE "S32LE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_S32_BE, SPA_TYPE_AUDIO_FORMAT_BASE "S32BE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_U32_LE, SPA_TYPE_AUDIO_FORMAT_BASE "U32LE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_U32_BE, SPA_TYPE_AUDIO_FORMAT_BASE "U32BE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_S24_LE, SPA_TYPE_AUDIO_FORMAT_BASE "S24LE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_S24_BE, SPA_TYPE_AUDIO_FORMAT_BASE "S24BE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_U24_LE, SPA_TYPE_AUDIO_FORMAT_BASE "U24LE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_U24_BE, SPA_TYPE_AUDIO_FORMAT_BASE "U24BE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_S20_LE, SPA_TYPE_AUDIO_FORMAT_BASE "S20LE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_S20_BE, SPA_TYPE_AUDIO_FORMAT_BASE "S20BE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_U20_LE, SPA_TYPE_AUDIO_FORMAT_BASE "U20LE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_U20_BE, SPA_TYPE_AUDIO_FORMAT_BASE "U20BE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_S18_LE, SPA_TYPE_AUDIO_FORMAT_BASE "S18LE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_S18_BE, SPA_TYPE_AUDIO_FORMAT_BASE "S18BE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_U18_LE, SPA_TYPE_AUDIO_FORMAT_BASE "U18LE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_U18_BE, SPA_TYPE_AUDIO_FORMAT_BASE "U18BE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_F32_LE, SPA_TYPE_AUDIO_FORMAT_BASE "F32LE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_F32_BE, SPA_TYPE_AUDIO_FORMAT_BASE "F32BE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_F64_LE, SPA_TYPE_AUDIO_FORMAT_BASE "F64LE", SPA_ID_Int, },
	{ SPA_AUDIO_FORMAT_F64_BE, SPA_TYPE_AUDIO_FORMAT_BASE "F64BE", SPA_ID_Int, },
        { 0, NULL, },
};

#define SPA_TYPE__AudioFlags		SPA_TYPE_FLAGS_BASE "AudioFlags"
#define SPA_TYPE_AUDIO_FLAGS_BASE	SPA_TYPE__AudioFlags ":"

static const struct spa_type_info spa_type_audio_flags[] = {
	{ SPA_AUDIO_FLAG_NONE, SPA_TYPE_AUDIO_FLAGS_BASE "none", SPA_ID_Int, },
	{ SPA_AUDIO_FLAG_UNPOSITIONED, SPA_TYPE_AUDIO_FLAGS_BASE "unpositioned", SPA_ID_Int, },
        { 0, NULL, },
};

#define SPA_TYPE__AudioLayout		SPA_TYPE_ENUM_BASE "AudioLayout"
#define SPA_TYPE_AUDIO_ENUM_BASE	SPA_TYPE__AudioLayout ":"

static const struct spa_type_info spa_type_audio_layout[] = {
	{ SPA_AUDIO_LAYOUT_INTERLEAVED, SPA_TYPE_AUDIO_ENUM_BASE "interleaved", SPA_ID_Int, },
	{ SPA_AUDIO_LAYOUT_NON_INTERLEAVED, SPA_TYPE_AUDIO_ENUM_BASE "non-interleaved", SPA_ID_Int, },
        { 0, NULL, },
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_AUDIO_RAW_TYPES_H__ */
