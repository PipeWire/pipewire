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
	struct spa_pod *position = NULL;
	int res;
	res = spa_pod_object_parse(format,
		":", SPA_FORMAT_AUDIO_format,		"I", &info->format,
		":", SPA_FORMAT_AUDIO_rate,		"i", &info->rate,
		":", SPA_FORMAT_AUDIO_channels,		"i", &info->channels,
		":", SPA_FORMAT_AUDIO_flags,		"?i", &info->flags,
		":", SPA_FORMAT_AUDIO_position,		"?P", &position, NULL);
	if (position && position->type == SPA_TYPE_Array &&
			SPA_POD_ARRAY_TYPE(position) == SPA_TYPE_Id) {
		uint32_t *values = SPA_POD_ARRAY_VALUES(position);
		uint32_t n_values = SPA_MIN(SPA_POD_ARRAY_N_VALUES(position), SPA_AUDIO_MAX_CHANNELS);
		memcpy(info->position, values, n_values * sizeof(uint32_t));
	}
	return res;
}

static inline struct spa_pod *
spa_format_audio_raw_build(struct spa_pod_builder *builder, uint32_t id, struct spa_audio_info_raw *info)
{
	spa_pod_builder_push_object(builder, SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_props(builder,
                        SPA_FORMAT_mediaType,		&SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                        SPA_FORMAT_mediaSubtype,	&SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_AUDIO_format,	&SPA_POD_Id(info->format),
                        SPA_FORMAT_AUDIO_rate,		&SPA_POD_Int(info->rate),
                        SPA_FORMAT_AUDIO_channels,	&SPA_POD_Int(info->channels),
			0);

	if (info->channels > 1) {
		spa_pod_builder_prop(builder, SPA_FORMAT_AUDIO_position, 0);
		spa_pod_builder_array(builder, sizeof(uint32_t), SPA_TYPE_Id, info->channels, info->position);
	}
	return spa_pod_builder_pop(builder);
}


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* __SPA_PARAM_AUDIO_FORMAT_UTILS */
