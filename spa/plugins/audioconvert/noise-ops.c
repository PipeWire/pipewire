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

#include <spa/param/audio/format-utils.h>
#include <spa/support/cpu.h>
#include <spa/support/log.h>
#include <spa/utils/defs.h>

#include "noise-ops.h"

typedef void (*noise_func_t) (struct noise *ns, void * SPA_RESTRICT dst[], uint32_t n_samples);

static const struct noise_info {
	noise_func_t process;
	uint32_t cpu_flags;
} noise_table[] =
{
	{ noise_f32_c, 0 },
};

#define MATCH_CPU_FLAGS(a,b)	((a) == 0 || ((a) & (b)) == a)

static const struct noise_info *find_noise_info(uint32_t cpu_flags)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(noise_table); i++) {
		if (!MATCH_CPU_FLAGS(noise_table[i].cpu_flags, cpu_flags))
			continue;
		return &noise_table[i];
	}
	return NULL;
}

static void impl_noise_free(struct noise *ns)
{
	ns->process = NULL;
}

int noise_init(struct noise *ns)
{
	const struct noise_info *info;
	int i;

	info = find_noise_info(ns->cpu_flags);
	if (info == NULL)
		return -ENOTSUP;

	for (i = 0; i < NOISE_SIZE; i++)
		ns->tab[i] = (drand48() - 0.5) / (1 << ns->intensity);

	ns->free = impl_noise_free;
	ns->process = info->process;
	return 0;
}
