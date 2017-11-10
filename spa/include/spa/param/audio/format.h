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

#ifndef __SPA_PARAM_AUDIO_FORMAT_H__
#define __SPA_PARAM_AUDIO_FORMAT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/param/format.h>
#include <spa/param/audio/raw.h>

#define SPA_TYPE_FORMAT__Audio			SPA_TYPE_FORMAT_BASE "Audio"
#define SPA_TYPE_FORMAT_AUDIO_BASE		SPA_TYPE_FORMAT__Audio ":"

#define SPA_TYPE_FORMAT_AUDIO__format		SPA_TYPE_FORMAT_AUDIO_BASE "format"
#define SPA_TYPE_FORMAT_AUDIO__flags		SPA_TYPE_FORMAT_AUDIO_BASE "flags"
#define SPA_TYPE_FORMAT_AUDIO__layout		SPA_TYPE_FORMAT_AUDIO_BASE "layout"
#define SPA_TYPE_FORMAT_AUDIO__rate		SPA_TYPE_FORMAT_AUDIO_BASE "rate"
#define SPA_TYPE_FORMAT_AUDIO__channels		SPA_TYPE_FORMAT_AUDIO_BASE "channels"
#define SPA_TYPE_FORMAT_AUDIO__channelMask	SPA_TYPE_FORMAT_AUDIO_BASE "channel-mask"

struct spa_audio_info {
	uint32_t media_type;
	uint32_t media_subtype;
	union {
		struct spa_audio_info_raw raw;
	} info;
};

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_AUDIO_FORMAT_H */
