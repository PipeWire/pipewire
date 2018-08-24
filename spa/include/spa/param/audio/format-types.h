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

#ifndef __SPA_PARAM_AUDIO_FORMAT_TYPES_H__
#define __SPA_PARAM_AUDIO_FORMAT_TYPES_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/param/audio/format.h>
#include <spa/param/audio/raw-types.h>

#define SPA_TYPE__FormatAudio		SPA_TYPE_FORMAT_BASE "Audio"
#define SPA_TYPE_FORMAT_AUDIO_BASE	SPA_TYPE__FormatAudio ":"

static const struct spa_type_info spa_type_format_audio_ids[] = {
	{ SPA_FORMAT_AUDIO_format, SPA_TYPE_FORMAT_AUDIO_BASE "format", SPA_ID_Enum,
		spa_type_audio_format },
	{ SPA_FORMAT_AUDIO_flags, SPA_TYPE_FORMAT_AUDIO_BASE "flags", SPA_ID_Enum,
		spa_type_audio_flags },
	{ SPA_FORMAT_AUDIO_layout, SPA_TYPE_FORMAT_AUDIO_BASE "layout", SPA_ID_Enum,
		spa_type_audio_layout },
	{ SPA_FORMAT_AUDIO_rate, SPA_TYPE_FORMAT_AUDIO_BASE "rate", SPA_ID_Int, },
	{ SPA_FORMAT_AUDIO_channels, SPA_TYPE_FORMAT_AUDIO_BASE "channels", SPA_ID_Int, },
	{ SPA_FORMAT_AUDIO_channelMask, SPA_TYPE_FORMAT_AUDIO_BASE "channelMask", SPA_ID_Int, },
        { 0, NULL, },
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_AUDIO_FORMAT_TYPES_H */
