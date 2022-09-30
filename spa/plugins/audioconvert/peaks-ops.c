/* Spa
 *
 * Copyright © 2022 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

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
