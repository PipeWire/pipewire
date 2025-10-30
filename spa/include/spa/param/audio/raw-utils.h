/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_AUDIO_RAW_UTILS_H
#define SPA_AUDIO_RAW_UTILS_H

#include <spa/pod/parser.h>
#include <spa/pod/iter.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format.h>
#include <spa/param/format-utils.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

#ifndef SPA_API_AUDIO_RAW_UTILS
 #ifdef SPA_API_IMPL
  #define SPA_API_AUDIO_RAW_UTILS SPA_API_IMPL
 #else
  #define SPA_API_AUDIO_RAW_UTILS static inline
 #endif
#endif

SPA_API_AUDIO_RAW_UTILS int
spa_format_audio_raw_ext_parse(const struct spa_pod *format, struct spa_audio_info_raw *info, size_t size)
{
	struct spa_pod *position = NULL;
	int res;
	uint32_t max_position = SPA_AUDIO_INFO_RAW_MAX_POSITION(size);

	if (!SPA_AUDIO_INFO_RAW_VALID_SIZE(size))
		return -EINVAL;

	info->flags = 0;
	res = spa_pod_parse_object(format,
			SPA_TYPE_OBJECT_Format, NULL,
			SPA_FORMAT_AUDIO_format,	SPA_POD_OPT_Id(&info->format),
			SPA_FORMAT_AUDIO_rate,		SPA_POD_OPT_Int(&info->rate),
			SPA_FORMAT_AUDIO_channels,	SPA_POD_OPT_Int(&info->channels),
			SPA_FORMAT_AUDIO_position,	SPA_POD_OPT_Pod(&position));
	if (info->channels > max_position)
		return -ECHRNG;
	if (position == NULL ||
	    spa_pod_copy_array(position, SPA_TYPE_Id, info->position, max_position) != info->channels)
		SPA_FLAG_SET(info->flags, SPA_AUDIO_FLAG_UNPOSITIONED);

	return res;
}

SPA_API_AUDIO_RAW_UTILS int
spa_format_audio_raw_parse(const struct spa_pod *format, struct spa_audio_info_raw *info)
{
	return spa_format_audio_raw_ext_parse(format, info, sizeof(*info));
}

SPA_API_AUDIO_RAW_UTILS struct spa_pod *
spa_format_audio_raw_ext_build(struct spa_pod_builder *builder, uint32_t id,
			   const struct spa_audio_info_raw *info, size_t size)
{
	struct spa_pod_frame f;
	uint32_t max_position = SPA_AUDIO_INFO_RAW_MAX_POSITION(size);

	if (!SPA_AUDIO_INFO_RAW_VALID_SIZE(size)) {
		errno = EINVAL;
		return NULL;
	}

	spa_pod_builder_push_object(builder, &f, SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(builder,
			SPA_FORMAT_mediaType,		SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,	SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			0);
	if (info->format != SPA_AUDIO_FORMAT_UNKNOWN)
		spa_pod_builder_add(builder,
			SPA_FORMAT_AUDIO_format,	SPA_POD_Id(info->format), 0);
	if (info->rate != 0)
		spa_pod_builder_add(builder,
			SPA_FORMAT_AUDIO_rate,		SPA_POD_Int(info->rate), 0);
	if (info->channels != 0) {
		spa_pod_builder_add(builder,
			SPA_FORMAT_AUDIO_channels,	SPA_POD_Int(info->channels), 0);
		/* we drop the positions here when we can't read all of them. This is
		 * really a malformed spa_audio_info structure. */
		if (!SPA_FLAG_IS_SET(info->flags, SPA_AUDIO_FLAG_UNPOSITIONED) &&
		    info->channels <= max_position) {
			spa_pod_builder_add(builder, SPA_FORMAT_AUDIO_position,
				SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Id,
					info->channels, info->position), 0);
		}
	}
	return (struct spa_pod*)spa_pod_builder_pop(builder, &f);
}

SPA_API_AUDIO_RAW_UTILS struct spa_pod *
spa_format_audio_raw_build(struct spa_pod_builder *builder, uint32_t id,
			   const struct spa_audio_info_raw *info)
{
	return spa_format_audio_raw_ext_build(builder, id, info, sizeof(*info));
}

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_AUDIO_RAW_UTILS_H */
