/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <time.h>

#include <spa/support/cpu.h>
#include <spa/utils/defs.h>
#include <spa/param/audio/format-utils.h>

#include "pffft.h"

#include "audio-dsp-impl.h"

struct dsp_info {
	uint32_t cpu_flags;

	struct spa_fga_dsp_methods funcs;
};

static const struct dsp_info dsp_table[] =
{
#if defined (HAVE_AVX2)
	{ SPA_CPU_FLAG_AVX2,
		.funcs.clear = dsp_clear_c,
		.funcs.copy = dsp_copy_c,
		.funcs.mix_gain = dsp_mix_gain_avx2,
		.funcs.biquad_run = dsp_biquad_run_sse,
		.funcs.sum = dsp_sum_avx2,
		.funcs.linear = dsp_linear_c,
		.funcs.mult = dsp_mult_c,
		.funcs.fft_new = dsp_fft_new_c,
		.funcs.fft_free = dsp_fft_free_c,
		.funcs.fft_memalloc = dsp_fft_memalloc_c,
		.funcs.fft_memfree = dsp_fft_memfree_c,
		.funcs.fft_memclear = dsp_fft_memclear_c,
		.funcs.fft_run = dsp_fft_run_c,
		.funcs.fft_cmul = dsp_fft_cmul_avx2,
		.funcs.fft_cmuladd = dsp_fft_cmuladd_avx2,
		.funcs.delay = dsp_delay_sse,
	},
#endif
#if defined (HAVE_SSE)
	{ SPA_CPU_FLAG_SSE,
		.funcs.clear = dsp_clear_c,
		.funcs.copy = dsp_copy_c,
		.funcs.mix_gain = dsp_mix_gain_sse,
		.funcs.biquad_run = dsp_biquad_run_sse,
		.funcs.sum = dsp_sum_sse,
		.funcs.linear = dsp_linear_c,
		.funcs.mult = dsp_mult_c,
		.funcs.fft_new = dsp_fft_new_c,
		.funcs.fft_free = dsp_fft_free_c,
		.funcs.fft_memalloc = dsp_fft_memalloc_c,
		.funcs.fft_memfree = dsp_fft_memfree_c,
		.funcs.fft_memclear = dsp_fft_memclear_c,
		.funcs.fft_run = dsp_fft_run_c,
		.funcs.fft_cmul = dsp_fft_cmul_sse,
		.funcs.fft_cmuladd = dsp_fft_cmuladd_sse,
		.funcs.delay = dsp_delay_sse,
	},
#endif
	{ 0,
		.funcs.clear = dsp_clear_c,
		.funcs.copy = dsp_copy_c,
		.funcs.mix_gain = dsp_mix_gain_c,
		.funcs.biquad_run = dsp_biquad_run_c,
		.funcs.sum = dsp_sum_c,
		.funcs.linear = dsp_linear_c,
		.funcs.mult = dsp_mult_c,
		.funcs.fft_new = dsp_fft_new_c,
		.funcs.fft_free = dsp_fft_free_c,
		.funcs.fft_memalloc = dsp_fft_memalloc_c,
		.funcs.fft_memfree = dsp_fft_memfree_c,
		.funcs.fft_memclear = dsp_fft_memclear_c,
		.funcs.fft_run = dsp_fft_run_c,
		.funcs.fft_cmul = dsp_fft_cmul_c,
		.funcs.fft_cmuladd = dsp_fft_cmuladd_c,
		.funcs.delay = dsp_delay_c,
	},
};

#define MATCH_CPU_FLAGS(a,b)	((a) == 0 || ((a) & (b)) == a)

static const struct dsp_info *find_dsp_info(uint32_t cpu_flags)
{
	SPA_FOR_EACH_ELEMENT_VAR(dsp_table, t) {
		if (MATCH_CPU_FLAGS(t->cpu_flags, cpu_flags))
			return t;
	}
	return NULL;
}

void spa_fga_dsp_free(struct spa_fga_dsp *dsp)
{
	free(dsp);
}

struct spa_fga_dsp * spa_fga_dsp_new(uint32_t cpu_flags)
{
	const struct dsp_info *info;
	struct spa_fga_dsp *dsp;

	info = find_dsp_info(cpu_flags);
	if (info == NULL) {
		errno = ENOTSUP;
		return NULL;
	}
	dsp = calloc(1, sizeof(*dsp));
	if (dsp == NULL)
		return NULL;

	pffft_select_cpu(cpu_flags);
	dsp->cpu_flags = cpu_flags;
	dsp->iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioDSP,
			SPA_VERSION_FGA_DSP,
			&info->funcs, dsp);

	return dsp;
}
