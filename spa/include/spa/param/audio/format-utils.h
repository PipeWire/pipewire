/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_PARAM_AUDIO_FORMAT_UTILS_H
#define SPA_PARAM_AUDIO_FORMAT_UTILS_H

#include <spa/pod/parser.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format.h>
#include <spa/param/format-utils.h>

#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/dsp-utils.h>
#include <spa/param/audio/iec958-utils.h>
#include <spa/param/audio/dsd-utils.h>
#include <spa/param/audio/mp3-utils.h>
#include <spa/param/audio/aac-utils.h>
#include <spa/param/audio/vorbis-utils.h>
#include <spa/param/audio/wma-utils.h>
#include <spa/param/audio/ra-utils.h>
#include <spa/param/audio/amr-utils.h>
#include <spa/param/audio/alac-utils.h>
#include <spa/param/audio/flac-utils.h>
#include <spa/param/audio/ape-utils.h>
#include <spa/param/audio/ac3-utils.h>
#include <spa/param/audio/eac3-utils.h>
#include <spa/param/audio/truehd-utils.h>
#include <spa/param/audio/dts-utils.h>
#include <spa/param/audio/mpegh-utils.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

#ifndef SPA_API_AUDIO_FORMAT_UTILS
 #ifdef SPA_API_IMPL
  #define SPA_API_AUDIO_FORMAT_UTILS SPA_API_IMPL
 #else
  #define SPA_API_AUDIO_FORMAT_UTILS static inline
 #endif
#endif

SPA_API_AUDIO_FORMAT_UTILS int
spa_format_audio_ext_parse(const struct spa_pod *format, struct spa_audio_info *info, size_t size)
{
	int res;

	if ((res = spa_format_parse(format, &info->media_type, &info->media_subtype)) < 0)
		return res;

	if (info->media_type != SPA_MEDIA_TYPE_audio)
		return -EINVAL;

	switch (info->media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		return spa_format_audio_raw_ext_parse(format, &info->info.raw,
				size - offsetof(struct spa_audio_info, info.raw));
	case SPA_MEDIA_SUBTYPE_dsp:
		return spa_format_audio_dsp_parse(format, &info->info.dsp);
	case SPA_MEDIA_SUBTYPE_iec958:
		return spa_format_audio_iec958_parse(format, &info->info.iec958);
	case SPA_MEDIA_SUBTYPE_dsd:
		return spa_format_audio_dsd_parse(format, &info->info.dsd);
	case SPA_MEDIA_SUBTYPE_mp3:
		return spa_format_audio_mp3_parse(format, &info->info.mp3);
	case SPA_MEDIA_SUBTYPE_aac:
		return spa_format_audio_aac_parse(format, &info->info.aac);
	case SPA_MEDIA_SUBTYPE_vorbis:
		return spa_format_audio_vorbis_parse(format, &info->info.vorbis);
	case SPA_MEDIA_SUBTYPE_wma:
		return spa_format_audio_wma_parse(format, &info->info.wma);
	case SPA_MEDIA_SUBTYPE_ra:
		return spa_format_audio_ra_parse(format, &info->info.ra);
	case SPA_MEDIA_SUBTYPE_amr:
		return spa_format_audio_amr_parse(format, &info->info.amr);
	case SPA_MEDIA_SUBTYPE_alac:
		return spa_format_audio_alac_parse(format, &info->info.alac);
	case SPA_MEDIA_SUBTYPE_flac:
		return spa_format_audio_flac_parse(format, &info->info.flac);
	case SPA_MEDIA_SUBTYPE_ape:
		return spa_format_audio_ape_parse(format, &info->info.ape);
	case SPA_MEDIA_SUBTYPE_ac3:
		return spa_format_audio_ac3_parse(format, &info->info.ac3);
	case SPA_MEDIA_SUBTYPE_eac3:
		return spa_format_audio_eac3_parse(format, &info->info.eac3);
	case SPA_MEDIA_SUBTYPE_truehd:
		return spa_format_audio_truehd_parse(format, &info->info.truehd);
	case SPA_MEDIA_SUBTYPE_dts:
		return spa_format_audio_dts_parse(format, &info->info.dts);
	case SPA_MEDIA_SUBTYPE_mpegh:
		return spa_format_audio_mpegh_parse(format, &info->info.mpegh);
	}
	return -ENOTSUP;
}

SPA_API_AUDIO_FORMAT_UTILS int
spa_format_audio_parse(const struct spa_pod *format, struct spa_audio_info *info)
{
	return spa_format_audio_ext_parse(format, info, sizeof(*info));
}

SPA_API_AUDIO_FORMAT_UTILS struct spa_pod *
spa_format_audio_ext_build(struct spa_pod_builder *builder, uint32_t id,
		       const struct spa_audio_info *info, size_t size)
{
	switch (info->media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		return spa_format_audio_raw_ext_build(builder, id, &info->info.raw,
				size - offsetof(struct spa_audio_info, info.raw));
	case SPA_MEDIA_SUBTYPE_dsp:
		return spa_format_audio_dsp_build(builder, id, &info->info.dsp);
	case SPA_MEDIA_SUBTYPE_iec958:
		return spa_format_audio_iec958_build(builder, id, &info->info.iec958);
	case SPA_MEDIA_SUBTYPE_dsd:
		return spa_format_audio_dsd_build(builder, id, &info->info.dsd);
	case SPA_MEDIA_SUBTYPE_mp3:
		return spa_format_audio_mp3_build(builder, id, &info->info.mp3);
	case SPA_MEDIA_SUBTYPE_aac:
		return spa_format_audio_aac_build(builder, id, &info->info.aac);
	case SPA_MEDIA_SUBTYPE_vorbis:
		return spa_format_audio_vorbis_build(builder, id, &info->info.vorbis);
	case SPA_MEDIA_SUBTYPE_wma:
		return spa_format_audio_wma_build(builder, id, &info->info.wma);
	case SPA_MEDIA_SUBTYPE_ra:
		return spa_format_audio_ra_build(builder, id, &info->info.ra);
	case SPA_MEDIA_SUBTYPE_amr:
		return spa_format_audio_amr_build(builder, id, &info->info.amr);
	case SPA_MEDIA_SUBTYPE_alac:
		return spa_format_audio_alac_build(builder, id, &info->info.alac);
	case SPA_MEDIA_SUBTYPE_flac:
		return spa_format_audio_flac_build(builder, id, &info->info.flac);
	case SPA_MEDIA_SUBTYPE_ape:
		return spa_format_audio_ape_build(builder, id, &info->info.ape);
	case SPA_MEDIA_SUBTYPE_ac3:
		return spa_format_audio_ac3_build(builder, id, &info->info.ac3);
	case SPA_MEDIA_SUBTYPE_eac3:
		return spa_format_audio_eac3_build(builder, id, &info->info.eac3);
	case SPA_MEDIA_SUBTYPE_truehd:
		return spa_format_audio_truehd_build(builder, id, &info->info.truehd);
	case SPA_MEDIA_SUBTYPE_dts:
		return spa_format_audio_dts_build(builder, id, &info->info.dts);
	case SPA_MEDIA_SUBTYPE_mpegh:
		return spa_format_audio_mpegh_build(builder, id, &info->info.mpegh);
	}
	errno = ENOTSUP;
	return NULL;
}

SPA_API_AUDIO_FORMAT_UTILS struct spa_pod *
spa_format_audio_build(struct spa_pod_builder *builder, uint32_t id,
		       const struct spa_audio_info *info)
{
	return spa_format_audio_ext_build(builder, id, info, sizeof(*info));
}
/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_PARAM_AUDIO_FORMAT_UTILS_H */
