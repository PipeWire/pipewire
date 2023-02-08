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

#include "peaks-ops.h"

typedef void (*peaks_min_max_func_t) (struct peaks *peaks, const float * SPA_RESTRICT src,
		uint32_t n_samples, float *min, float *max);
typedef float (*peaks_abs_max_func_t) (struct peaks *peaks, const float * SPA_RESTRICT src,
			uint32_t n_samples, float max);

#define MAKE(min_max,abs_max,...) \
	{ min_max, abs_max, #min_max , __VA_ARGS__ }

static const struct peaks_info {
	peaks_min_max_func_t min_max;
	peaks_abs_max_func_t abs_max;
	const char *name;
	uint32_t cpu_flags;
} peaks_table[] =
{
#if defined (HAVE_SSE)
	MAKE(peaks_min_max_sse, peaks_abs_max_sse, SPA_CPU_FLAG_SSE),
#endif
	MAKE(peaks_min_max_c, peaks_abs_max_c),
};
#undef MAKE

#define MATCH_CPU_FLAGS(a,b)	((a) == 0 || ((a) & (b)) == a)

static const struct peaks_info *find_peaks_info(uint32_t cpu_flags)
{
	SPA_FOR_EACH_ELEMENT_VAR(peaks_table, t) {
		if (MATCH_CPU_FLAGS(t->cpu_flags, cpu_flags))
			return t;
	}
	return NULL;
}

static void impl_peaks_free(struct peaks *peaks)
{
	peaks->min_max = NULL;
	peaks->abs_max = NULL;
}

int peaks_init(struct peaks *peaks)
{
	const struct peaks_info *info;

	info = find_peaks_info(peaks->cpu_flags);
	if (info == NULL)
		return -ENOTSUP;

	peaks->cpu_flags = info->cpu_flags;
	peaks->func_name = info->name;
	peaks->free = impl_peaks_free;
	peaks->min_max = info->min_max;
	peaks->abs_max = info->abs_max;
	return 0;
}
