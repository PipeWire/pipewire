/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_AUDIO_RAW_JSON_H
#define SPA_AUDIO_RAW_JSON_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

#include <spa/utils/json.h>
#include <spa/param/audio/raw-types.h>

static inline int
spa_audio_parse_position(const char *str, size_t len,
		uint32_t *position, uint32_t *n_channels)
{
	struct spa_json iter;
        char v[256];
	uint32_t channels = 0;

        if (spa_json_begin_array_relax(&iter, str, len) <= 0)
                return 0;

        while (spa_json_get_string(&iter, v, sizeof(v)) > 0 &&
		channels < SPA_AUDIO_MAX_CHANNELS) {
                position[channels++] = spa_type_audio_channel_from_short_name(v);
        }
	*n_channels = channels;
	return channels;
}

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_AUDIO_RAW_JSON_H */
