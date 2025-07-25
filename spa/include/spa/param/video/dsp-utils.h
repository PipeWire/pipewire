/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_VIDEO_DSP_UTILS_H
#define SPA_VIDEO_DSP_UTILS_H

#include <spa/pod/parser.h>
#include <spa/pod/builder.h>
#include <spa/param/video/dsp.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

#ifndef SPA_API_VIDEO_DSP_UTILS
 #ifdef SPA_API_IMPL
  #define SPA_API_VIDEO_DSP_UTILS SPA_API_IMPL
 #else
  #define SPA_API_VIDEO_DSP_UTILS static inline
 #endif
#endif

SPA_API_VIDEO_DSP_UTILS int
spa_format_video_dsp_parse(const struct spa_pod *format,
			   struct spa_video_info_dsp *info)
{
	info->flags = SPA_VIDEO_FLAG_NONE;
	const struct spa_pod_prop *mod_prop;
	if ((mod_prop = spa_pod_find_prop (format, NULL, SPA_FORMAT_VIDEO_modifier)) != NULL) {
		info->flags |= SPA_VIDEO_FLAG_MODIFIER;
		if ((mod_prop->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) == SPA_POD_PROP_FLAG_DONT_FIXATE)
			info->flags |= SPA_VIDEO_FLAG_MODIFIER_FIXATION_REQUIRED;
	}

	return spa_pod_parse_object(format,
		SPA_TYPE_OBJECT_Format, NULL,
		SPA_FORMAT_VIDEO_format,		SPA_POD_OPT_Id(&info->format),
		SPA_FORMAT_VIDEO_modifier,		SPA_POD_OPT_Long(&info->modifier));
}

SPA_API_VIDEO_DSP_UTILS struct spa_pod *
spa_format_video_dsp_build(struct spa_pod_builder *builder, uint32_t id,
			   const struct spa_video_info_dsp *info)
{
	struct spa_pod_frame f;
	spa_pod_builder_push_object(builder, &f, SPA_TYPE_OBJECT_Format, id);
	spa_pod_builder_add(builder,
			SPA_FORMAT_mediaType,		SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype,	SPA_POD_Id(SPA_MEDIA_SUBTYPE_dsp),
			0);
	if (info->format != SPA_VIDEO_FORMAT_UNKNOWN)
		spa_pod_builder_add(builder,
			SPA_FORMAT_VIDEO_format,	SPA_POD_Id(info->format), 0);
	if (info->modifier != 0 || info->flags & SPA_VIDEO_FLAG_MODIFIER) {
		spa_pod_builder_prop(builder,
			SPA_FORMAT_VIDEO_modifier,	SPA_POD_PROP_FLAG_MANDATORY);
		spa_pod_builder_long(builder,           info->modifier);
	}
	return (struct spa_pod*)spa_pod_builder_pop(builder, &f);
}

/**
 * \}
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* SPA_VIDEO_DSP_UTILS_H */
