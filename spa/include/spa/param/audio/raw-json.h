/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_AUDIO_RAW_JSON_H
#define SPA_AUDIO_RAW_JSON_H

#include <spa/utils/dict.h>
#include <spa/utils/json.h>
#include <spa/param/audio/raw.h>
#include <spa/param/audio/raw-types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \addtogroup spa_param
 * \{
 */

#ifndef SPA_API_AUDIO_RAW_JSON
 #ifdef SPA_API_IMPL
  #define SPA_API_AUDIO_RAW_JSON SPA_API_IMPL
 #else
  #define SPA_API_AUDIO_RAW_JSON static inline
 #endif
#endif

SPA_API_AUDIO_RAW_JSON int
spa_audio_parse_position_n(const char *str, size_t len,
		uint32_t *position, uint32_t max_position, uint32_t *n_channels)
{
	struct spa_json iter;
        char v[256];
	uint32_t channels = 0;

        if (spa_json_begin_array_relax(&iter, str, len) <= 0)
                return 0;

        while (spa_json_get_string(&iter, v, sizeof(v)) > 0) {
		if (channels < max_position)
	                position[channels] = spa_type_audio_channel_from_short_name(v);
		channels++;
        }
	*n_channels = channels;
	return channels;
}

SPA_API_AUDIO_RAW_JSON int
spa_audio_parse_position(const char *str, size_t len,
		uint32_t *position, uint32_t *n_channels)
{
	return spa_audio_parse_position_n(str, len, position, SPA_AUDIO_MAX_CHANNELS, n_channels);
}

SPA_API_AUDIO_RAW_JSON int
spa_audio_info_raw_ext_update(struct spa_audio_info_raw *info, size_t size,
		const char *key, const char *val, bool force)
{
	uint32_t v;
	uint32_t max_position = SPA_AUDIO_INFO_RAW_MAX_POSITION(size);
	if (spa_streq(key, SPA_KEY_AUDIO_FORMAT)) {
		if (force || info->format == 0)
			info->format = (enum spa_audio_format)spa_type_audio_format_from_short_name(val);
	} else if (spa_streq(key, SPA_KEY_AUDIO_RATE)) {
		if (spa_atou32(val, &v, 0) && (force || info->rate == 0))
			info->rate = v;
	} else if (spa_streq(key, SPA_KEY_AUDIO_CHANNELS)) {
		if (spa_atou32(val, &v, 0) && (force || info->channels == 0)) {
			if (v > max_position)
				return -ECHRNG;
			info->channels = v;
		}
	} else if (spa_streq(key, SPA_KEY_AUDIO_POSITION)) {
		if (force || info->channels == 0) {
			if (spa_audio_parse_position_n(val, strlen(val), info->position,
						max_position, &v) > 0) {
				if (v > max_position)
					return -ECHRNG;
				SPA_FLAG_CLEAR(info->flags, SPA_AUDIO_FLAG_UNPOSITIONED);
			}
		}
	}
	return 0;
}

SPA_API_AUDIO_RAW_JSON int
spa_audio_info_raw_update(struct spa_audio_info_raw *info,
		const char *key, const char *val, bool force)
{
	return spa_audio_info_raw_ext_update(info, sizeof(*info), key, val, force);
}

SPA_API_AUDIO_RAW_JSON int
spa_audio_info_raw_ext_init_dict_keys_va(struct spa_audio_info_raw *info, size_t size,
		const struct spa_dict *defaults,
		const struct spa_dict *dict, va_list args)
{
	int res;

	memset(info, 0, size);
	SPA_FLAG_SET(info->flags, SPA_AUDIO_FLAG_UNPOSITIONED);
	if (dict) {
		const char *val, *key;
		while ((key = va_arg(args, const char *))) {
			if ((val = spa_dict_lookup(dict, key)) == NULL)
				continue;
			if ((res = spa_audio_info_raw_ext_update(info, size,
							key, val, true)) < 0)
				return res;
		}
	}
	if (defaults) {
		const struct spa_dict_item *it;
		spa_dict_for_each(it, defaults)
			if ((res = spa_audio_info_raw_ext_update(info, size,
							it->key, it->value, false)) < 0)
				return res;
	}
	return 0;
}

SPA_API_AUDIO_RAW_JSON int SPA_SENTINEL
spa_audio_info_raw_ext_init_dict_keys(struct spa_audio_info_raw *info, size_t size,
		const struct spa_dict *defaults,
		const struct spa_dict *dict, ...)
{
	va_list args;
	int res;
	va_start(args, dict);
	res = spa_audio_info_raw_ext_init_dict_keys_va(info, size, defaults, dict, args);
	va_end(args);
	return res;
}

SPA_API_AUDIO_RAW_JSON int SPA_SENTINEL
spa_audio_info_raw_init_dict_keys(struct spa_audio_info_raw *info,
		const struct spa_dict *defaults,
		const struct spa_dict *dict, ...)
{
	va_list args;
	int res;
	va_start(args, dict);
	res = spa_audio_info_raw_ext_init_dict_keys_va(info, sizeof(*info), defaults, dict, args);
	va_end(args);
	return res;
}

/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_AUDIO_RAW_JSON_H */
