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

#ifndef __SPA_PARAM_AUDIO_FORMAT_UTILS_H__
#define __SPA_PARAM_AUDIO_FORMAT_UTILS_H__

#ifdef __cplusplus
extern "C" {
#endif


#include <spa/pod/parser.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format.h>
#include <spa/param/format-utils.h>

static inline int
spa_format_audio_raw_parse(const struct spa_pod *format, struct spa_audio_info_raw *info)
{
	return spa_pod_object_parse(format,
		":", SPA_FORMAT_AUDIO_format,		"I", &info->format,
		":", SPA_FORMAT_AUDIO_layout,		"I", &info->layout,
		":", SPA_FORMAT_AUDIO_rate,		"i", &info->rate,
		":", SPA_FORMAT_AUDIO_channels,		"i", &info->channels,
		":", SPA_FORMAT_AUDIO_flags,		"?i", &info->flags,
		":", SPA_FORMAT_AUDIO_channelMask,	"?i", &info->channel_mask, NULL);
}

static inline struct spa_pod *
spa_format_audio_raw_build(struct spa_pod_builder *builder, uint32_t id, struct spa_audio_info_raw *info)
{
	return spa_pod_builder_object(builder,
                        SPA_TYPE_OBJECT_Format, id,
                        "I", SPA_MEDIA_TYPE_audio,
                        "I", SPA_MEDIA_SUBTYPE_raw,
                        ":", SPA_FORMAT_AUDIO_format,   "I", info->format,
                        ":", SPA_FORMAT_AUDIO_layout,   "I", info->layout,
                        ":", SPA_FORMAT_AUDIO_rate,     "i", info->rate,
                        ":", SPA_FORMAT_AUDIO_channels, "i", info->channels);
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_AUDIO_FORMAT_UTILS */
