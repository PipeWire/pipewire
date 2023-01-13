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

#ifndef SPA_PARAM_AUDIO_FORMAT_UTILS_H
#define SPA_PARAM_AUDIO_FORMAT_UTILS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/pod/parser.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format.h>
#include <spa/param/format-utils.h>

#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/dsp-utils.h>
#include <spa/param/audio/iec958-utils.h>
#include <spa/param/audio/dsd-utils.h>


/**
 * \addtogroup spa_param
 * \{
 */

static inline int
spa_format_audio_parse(const struct spa_pod *format, struct spa_audio_info *info)
{
	int res;

	if ((res = spa_format_parse(format, &info->media_type, &info->media_subtype)) < 0)
		return res;

	if (info->media_type != SPA_MEDIA_TYPE_audio)
		return -EINVAL;

	switch (info->media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		return spa_format_audio_raw_parse(format, &info->info.raw);
	case SPA_MEDIA_SUBTYPE_dsp:
		return spa_format_audio_dsp_parse(format, &info->info.dsp);
	case SPA_MEDIA_SUBTYPE_iec958:
		return spa_format_audio_iec958_parse(format, &info->info.iec958);
	case SPA_MEDIA_SUBTYPE_dsd:
		return spa_format_audio_dsd_parse(format, &info->info.dsd);
	}
	return -ENOTSUP;
}

static inline struct spa_pod *
spa_format_audio_build(struct spa_pod_builder *builder, uint32_t id, struct spa_audio_info *info)
{
	switch (info->media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		return spa_format_audio_raw_build(builder, id, &info->info.raw);
	case SPA_MEDIA_SUBTYPE_dsp:
		return spa_format_audio_dsp_build(builder, id, &info->info.dsp);
	case SPA_MEDIA_SUBTYPE_iec958:
		return spa_format_audio_iec958_build(builder, id, &info->info.iec958);
	case SPA_MEDIA_SUBTYPE_dsd:
		return spa_format_audio_dsd_build(builder, id, &info->info.dsd);
	}
	errno = ENOTSUP;
	return NULL;
}
/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_PARAM_AUDIO_FORMAT_UTILS_H */
