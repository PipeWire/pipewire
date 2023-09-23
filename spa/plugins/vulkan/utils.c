/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2023 columbarius */
/* SPDX-License-Identifier: MIT */

#include "utils.h"

#include <spa/param/param.h>
#include <spa/param/video/dsp.h>
#include <spa/param/video/raw.h>

// This function enumerates the available formats in vulkan_state::formats, announcing all formats capable to support DmaBufs
// first and then falling back to those supported with SHM buffers.
bool find_EnumFormatInfo(struct vulkan_format_infos *fmtInfos, uint32_t index, uint32_t caps, uint32_t *fmt_idx, bool *has_modifier)
{
	int64_t fmtIterator = 0;
	int64_t maxIterator = 0;
	if (caps & VULKAN_BUFFER_TYPE_CAP_SHM)
		maxIterator += fmtInfos->formatCount;
	if (caps & VULKAN_BUFFER_TYPE_CAP_DMABUF)
		maxIterator += fmtInfos->formatCount;
	// Count available formats until index underflows, while fmtIterator indexes the current format.
	// Iterate twice over formats first time with modifiers, second time without if both caps are supported.
	while (index < (uint32_t)-1 && fmtIterator < maxIterator) {
		const struct vulkan_format_info *f_info = &fmtInfos->infos[fmtIterator%fmtInfos->formatCount];
		if (caps & VULKAN_BUFFER_TYPE_CAP_DMABUF && fmtIterator < fmtInfos->formatCount) {
			// First round, check for modifiers
			if (f_info->modifierCount > 0) {
				index--;
			}
		} else if (caps & VULKAN_BUFFER_TYPE_CAP_SHM) {
			// Second round, every format should be supported.
			index--;
		}
		fmtIterator++;
	}

	if (index != (uint32_t)-1) {
		// No more formats available
		return false;
	}
	// Undo end of loop increment
	fmtIterator--;
	*fmt_idx = fmtIterator%fmtInfos->formatCount;
	// Loop finished in first round
	*has_modifier = caps & VULKAN_BUFFER_TYPE_CAP_DMABUF && fmtIterator < fmtInfos->formatCount;
	return true;
}

struct spa_pod *build_dsp_EnumFormat(const struct vulkan_format_info *fmt, bool with_modifiers, struct spa_pod_builder *builder)
{
	struct spa_pod_frame f[2];
	uint32_t i, c;

	spa_pod_builder_push_object(builder, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
	spa_pod_builder_add(builder, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_dsp), 0);
	spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_format, SPA_POD_Id(fmt->spa_format), 0);
	if (with_modifiers && fmt->modifierCount > 0) {
		spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
		spa_pod_builder_push_choice(builder, &f[1], SPA_CHOICE_Enum, 0);
		for (i = 0, c = 0; i < fmt->modifierCount; i++) {
			spa_pod_builder_long(builder, fmt->infos[i].props.drmFormatModifier);
			if (c++ == 0)
				spa_pod_builder_long(builder, fmt->infos[i].props.drmFormatModifier);
		}
		spa_pod_builder_pop(builder, &f[1]);
	}
	return spa_pod_builder_pop(builder, &f[0]);
}

struct spa_pod *build_raw_EnumFormat(const struct vulkan_format_info *fmt, bool with_modifiers, struct spa_pod_builder *builder)
{
	struct spa_pod_frame f[2];
	uint32_t i, c;

	spa_pod_builder_push_object(builder, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
	spa_pod_builder_add(builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
	spa_pod_builder_add(builder, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
	spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_format, SPA_POD_Id(fmt->spa_format), 0);
	if (with_modifiers && fmt->modifierCount > 0) {
		spa_pod_builder_prop(builder, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
		spa_pod_builder_push_choice(builder, &f[1], SPA_CHOICE_Enum, 0);
		for (i = 0, c = 0; i < fmt->modifierCount; i++) {
			spa_pod_builder_long(builder, fmt->infos[i].props.drmFormatModifier);
			if (c++ == 0)
				spa_pod_builder_long(builder, fmt->infos[i].props.drmFormatModifier);
		}
		spa_pod_builder_pop(builder, &f[1]);
	}
	return spa_pod_builder_pop(builder, &f[0]);
}
