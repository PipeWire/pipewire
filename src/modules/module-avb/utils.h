/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef AVB_UTILS_H
#define AVB_UTILS_H

#include <spa/utils/json.h>

#include "internal.h"

static inline char *avb_utils_format_id(char *str, size_t size, const uint64_t id)
{
	snprintf(str, size, "%02x:%02x:%02x:%02x:%02x:%02x:%04x",
			(uint8_t)(id >> 56),
			(uint8_t)(id >> 48),
			(uint8_t)(id >> 40),
			(uint8_t)(id >> 32),
			(uint8_t)(id >> 24),
			(uint8_t)(id >> 16),
			(uint16_t)(id));
	return str;
}

static inline int avb_utils_parse_id(const char *str, int len, uint64_t *id)
{
	char s[64];
	uint8_t v[6];
	uint16_t unique_id;
	if (spa_json_parse_stringn(str, len, s, sizeof(s)) <= 0)
		return -EINVAL;
	if (sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx:%hx",
			&v[0], &v[1], &v[2], &v[3],
			&v[4], &v[5], &unique_id) == 7) {
		*id = (uint64_t) v[0] << 56 |
			    (uint64_t) v[1] << 48 |
			    (uint64_t) v[2] << 40 |
			    (uint64_t) v[3] << 32 |
			    (uint64_t) v[4] << 24 |
			    (uint64_t) v[5] << 16 |
			    unique_id;
	} else if (!spa_atou64(str, id, 0))
		return -EINVAL;
	return 0;
}

static inline char *avb_utils_format_addr(char *str, size_t size, const uint8_t addr[6])
{
	snprintf(str, size, "%02x:%02x:%02x:%02x:%02x:%02x",
			addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
	return str;
}
static inline int avb_utils_parse_addr(const char *str, int len, uint8_t addr[6])
{
	char s[64];
	uint8_t v[6];
	if (spa_json_parse_stringn(str, len, s, sizeof(s)) <= 0)
		return -EINVAL;
	if (sscanf(s, "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
			&v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6)
		return -EINVAL;
	memcpy(addr, v, 6);
	return 0;
}

#endif /* AVB_UTILS_H */
