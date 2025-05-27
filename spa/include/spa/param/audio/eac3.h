/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2025 Carlos Rafael Giani */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_AUDIO_EAC3_H
#define SPA_AUDIO_EAC3_H

#include <spa/param/audio/raw.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

/** Dolby E-AC-3 audio info. */
struct spa_audio_info_eac3 {
	uint32_t rate;				/*< sample rate */
	uint32_t channels;			/*< number of channels */
};

#define SPA_AUDIO_INFO_EAC3_INIT(...)		((struct spa_audio_info_eac3) { __VA_ARGS__ })

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_AUDIO_EAC3_H */
