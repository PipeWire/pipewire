/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>

#include <spa/support/cpu.h>
#include <spa/support/log.h>
#include <spa/utils/defs.h>

#include "gaps-ops.h"

typedef int (*gaps_check_func_t) (struct gaps *gaps, const float * SPA_RESTRICT src[],
		uint32_t n_samples);
typedef void (*gaps_fix_func_t) (struct gaps *gaps, float * SPA_RESTRICT dst[],
		const float * SPA_RESTRICT src[], uint32_t n_samples);

#define MAKE(check,fix,...) \
	{ check, fix, #fix , __VA_ARGS__ }

static const struct gaps_info {
	gaps_check_func_t check;
	gaps_fix_func_t fix;
	const char *name;
	uint32_t cpu_flags;
} gaps_table[] =
{
	MAKE(gaps_check_c, gaps_fix_c, 0),
};
#undef MAKE

#define MATCH_CPU_FLAGS(a,b)	((a) == 0 || ((a) & (b)) == a)

static const struct gaps_info *find_gaps_info(uint32_t cpu_flags)
{
	SPA_FOR_EACH_ELEMENT_VAR(gaps_table, t) {
		if (MATCH_CPU_FLAGS(t->cpu_flags, cpu_flags))
			return t;
	}
	return NULL;
}

static void impl_gaps_free(struct gaps *gaps)
{
	gaps->fix = NULL;
}

int gaps_init(struct gaps *gaps)
{
	const struct gaps_info *info;
	uint32_t i;

	info = find_gaps_info(gaps->cpu_flags);
	if (info == NULL)
		return -ENOTSUP;

	if (gaps->channels > SPA_AUDIO_MAX_CHANNELS)
		return -EINVAL;

	gaps->duration = SPA_MIN(gaps->duration, GAPS_MAX_CURVE);

	for (i = 0; i < gaps->duration; i++)
		gaps->curve[i] = (float)(0.5 + 0.5 * cos(M_PI + M_PI * i / gaps->duration));

	for (i = 0; i < gaps->channels; i++)
		spa_zero(gaps->states[i]);

	gaps->cpu_flags = info->cpu_flags;
	gaps->func_name = info->name;
	gaps->free = impl_gaps_free;
	gaps->check = info->check;
	gaps->fix = info->fix;
	return 0;
}
