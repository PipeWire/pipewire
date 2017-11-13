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

#ifndef __SPA_AUDIO_RAW_H__
#define __SPA_AUDIO_RAW_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <endian.h>

#define SPA_TYPE__AudioFormat			SPA_TYPE_ENUM_BASE "AudioFormat"
#define SPA_TYPE_AUDIO_FORMAT_BASE		SPA_TYPE__AudioFormat ":"

#define SPA_TYPE_AUDIO_FORMAT__UNKNOWN		SPA_TYPE_AUDIO_FORMAT_BASE "UNKNOWN"
#define SPA_TYPE_AUDIO_FORMAT__ENCODED		SPA_TYPE_AUDIO_FORMAT_BASE "ENCODED"
#define SPA_TYPE_AUDIO_FORMAT__S8		SPA_TYPE_AUDIO_FORMAT_BASE "S8"
#define SPA_TYPE_AUDIO_FORMAT__U8		SPA_TYPE_AUDIO_FORMAT_BASE "U8"
#define SPA_TYPE_AUDIO_FORMAT__S16LE		SPA_TYPE_AUDIO_FORMAT_BASE "S16LE"
#define SPA_TYPE_AUDIO_FORMAT__S16BE		SPA_TYPE_AUDIO_FORMAT_BASE "S16BE"
#define SPA_TYPE_AUDIO_FORMAT__U16LE		SPA_TYPE_AUDIO_FORMAT_BASE "U16LE"
#define SPA_TYPE_AUDIO_FORMAT__U16BE		SPA_TYPE_AUDIO_FORMAT_BASE "U16BE"
#define SPA_TYPE_AUDIO_FORMAT__S24_32LE		SPA_TYPE_AUDIO_FORMAT_BASE "S24_32LE"
#define SPA_TYPE_AUDIO_FORMAT__S24_32BE		SPA_TYPE_AUDIO_FORMAT_BASE "S24_32BE"
#define SPA_TYPE_AUDIO_FORMAT__U24_32LE		SPA_TYPE_AUDIO_FORMAT_BASE "U24_32LE"
#define SPA_TYPE_AUDIO_FORMAT__U24_32BE		SPA_TYPE_AUDIO_FORMAT_BASE "U24_32BE"
#define SPA_TYPE_AUDIO_FORMAT__S32LE		SPA_TYPE_AUDIO_FORMAT_BASE "S32LE"
#define SPA_TYPE_AUDIO_FORMAT__S32BE		SPA_TYPE_AUDIO_FORMAT_BASE "S32BE"
#define SPA_TYPE_AUDIO_FORMAT__U32LE		SPA_TYPE_AUDIO_FORMAT_BASE "U32LE"
#define SPA_TYPE_AUDIO_FORMAT__U32BE		SPA_TYPE_AUDIO_FORMAT_BASE "U32BE"
#define SPA_TYPE_AUDIO_FORMAT__S24LE		SPA_TYPE_AUDIO_FORMAT_BASE "S24LE"
#define SPA_TYPE_AUDIO_FORMAT__S24BE		SPA_TYPE_AUDIO_FORMAT_BASE "S24BE"
#define SPA_TYPE_AUDIO_FORMAT__U24LE		SPA_TYPE_AUDIO_FORMAT_BASE "U24LE"
#define SPA_TYPE_AUDIO_FORMAT__U24BE		SPA_TYPE_AUDIO_FORMAT_BASE "U24BE"
#define SPA_TYPE_AUDIO_FORMAT__S20LE		SPA_TYPE_AUDIO_FORMAT_BASE "S20LE"
#define SPA_TYPE_AUDIO_FORMAT__S20BE		SPA_TYPE_AUDIO_FORMAT_BASE "S20BE"
#define SPA_TYPE_AUDIO_FORMAT__U20LE		SPA_TYPE_AUDIO_FORMAT_BASE "U20LE"
#define SPA_TYPE_AUDIO_FORMAT__U20BE		SPA_TYPE_AUDIO_FORMAT_BASE "U20BE"
#define SPA_TYPE_AUDIO_FORMAT__S18LE		SPA_TYPE_AUDIO_FORMAT_BASE "S18LE"
#define SPA_TYPE_AUDIO_FORMAT__S18BE		SPA_TYPE_AUDIO_FORMAT_BASE "S18BE"
#define SPA_TYPE_AUDIO_FORMAT__U18LE		SPA_TYPE_AUDIO_FORMAT_BASE "U18LE"
#define SPA_TYPE_AUDIO_FORMAT__U18BE		SPA_TYPE_AUDIO_FORMAT_BASE "U18BE"
#define SPA_TYPE_AUDIO_FORMAT__F32LE		SPA_TYPE_AUDIO_FORMAT_BASE "F32LE"
#define SPA_TYPE_AUDIO_FORMAT__F32BE		SPA_TYPE_AUDIO_FORMAT_BASE "F32BE"
#define SPA_TYPE_AUDIO_FORMAT__F64LE		SPA_TYPE_AUDIO_FORMAT_BASE "F64LE"
#define SPA_TYPE_AUDIO_FORMAT__F64BE		SPA_TYPE_AUDIO_FORMAT_BASE "F64BE"

/** Extra audio flags */
enum spa_audio_flags {
	SPA_AUDIO_FLAG_NONE		= 0,		/*< no valid flag */
	SPA_AUDIO_FLAG_UNPOSITIONED	= (1 << 0),	/*< the position array explicitly
							 *  contains unpositioned channels. */
};

/** Layout of the audio samples for the different channels.  */
enum spa_audio_layout {
	SPA_AUDIO_LAYOUT_INTERLEAVED = 0,	/*< interleaved audio */
	SPA_AUDIO_LAYOUT_NON_INTERLEAVED	/*< non-interleaved audio */
};

/** Audio information description */
struct spa_audio_info_raw {
	uint32_t format;		/*< format, one of SPA_TYPE__AudioFormat */
	enum spa_audio_flags flags;	/*< extra flags */
	enum spa_audio_layout layout;	/*< sample layout */
	uint32_t rate;			/*< sample rate */
	uint32_t channels;		/*< number of channels */
	uint32_t channel_mask;		/*< channel mask */
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_AUDIO_RAW_H__ */
