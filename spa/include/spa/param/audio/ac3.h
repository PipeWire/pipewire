/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2025 Carlos Rafael Giani */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_AUDIO_AC3_H
#define SPA_AUDIO_AC3_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/param/audio/raw.h>

/**
 * \addtogroup spa_param
 * \{
 */

/** Dolby AC-3 audio info. */
struct spa_audio_info_ac3 {
	uint32_t rate;				/*< sample rate */
	uint32_t channels;			/*< number of channels */
};

#define SPA_AUDIO_INFO_AC3_INIT(...)		((struct spa_audio_info_ac3) { __VA_ARGS__ })

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_AUDIO_AC3_H */
