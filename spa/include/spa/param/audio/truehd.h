/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2025 Carlos Rafael Giani */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_AUDIO_TRUEHD_H
#define SPA_AUDIO_TRUEHD_H

#include <spa/param/audio/raw.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

/** Dolby TrueHD audio info. */
struct spa_audio_info_truehd {
	uint32_t rate;				/*< sample rate */
	uint32_t channels;			/*< number of channels */
};

#define SPA_AUDIO_INFO_TRUEHD_INIT(...)		((struct spa_audio_info_truehd) { __VA_ARGS__ })

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_AUDIO_TRUEHD_H */
