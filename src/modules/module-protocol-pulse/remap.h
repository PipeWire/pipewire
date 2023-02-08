/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PULSE_SERVER_REMAP_H
#define PULSE_SERVER_REMAP_H

#include <stddef.h>

#include <spa/utils/string.h>

struct str_map {
	const char *pw_str;
	const char *pa_str;
	const struct str_map *child;
};

extern const struct str_map media_role_map[];

extern const struct str_map props_key_map[];

static inline const struct str_map *str_map_find(const struct str_map *map, const char *pw, const char *pa)
{
	size_t i;
	for (i = 0; map[i].pw_str; i++)
		if ((pw && spa_streq(map[i].pw_str, pw)) ||
		    (pa && spa_streq(map[i].pa_str, pa)))
			return &map[i];
	return NULL;
}

#endif /* PULSE_SERVER_REMAP_H */
