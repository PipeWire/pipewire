/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <spa/support/cpu.h>
#include <spa/utils/defs.h>
#include <spa/param/audio/format-utils.h>

#include "dsp-ops.h"

struct dsp_info {
	uint32_t cpu_flags;

	struct dsp_ops_funcs funcs;
};

static struct dsp_info dsp_table[] =
{
#if defined (HAVE_AVX)
	{ SPA_CPU_FLAG_AVX,
		.funcs.clear = dsp_clear_c,
		.funcs.copy = dsp_copy_c,
		.funcs.mix_gain = dsp_mix_gain_sse,
		.funcs.biquad_run = dsp_biquad_run_c,
		.funcs.sum = dsp_sum_avx,
		.funcs.linear = dsp_linear_c,
		.funcs.mult = dsp_mult_c,
		.funcs.fft_new = dsp_fft_new_c,
		.funcs.fft_free = dsp_fft_free_c,
		.funcs.fft_run = dsp_fft_run_c,
		.funcs.fft_cmul = dsp_fft_cmul_c,
		.funcs.fft_cmuladd = dsp_fft_cmuladd_c,
	},
#endif
#if defined (HAVE_SSE)
	{ SPA_CPU_FLAG_SSE,
		.funcs.clear = dsp_clear_c,
		.funcs.copy = dsp_copy_c,
		.funcs.mix_gain = dsp_mix_gain_sse,
		.funcs.biquad_run = dsp_biquad_run_c,
		.funcs.sum = dsp_sum_sse,
		.funcs.linear = dsp_linear_c,
		.funcs.mult = dsp_mult_c,
		.funcs.fft_new = dsp_fft_new_c,
		.funcs.fft_free = dsp_fft_free_c,
		.funcs.fft_run = dsp_fft_run_c,
		.funcs.fft_cmul = dsp_fft_cmul_c,
		.funcs.fft_cmuladd = dsp_fft_cmuladd_c,
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
		.funcs.fft_run = dsp_fft_run_c,
		.funcs.fft_cmul = dsp_fft_cmul_c,
		.funcs.fft_cmuladd = dsp_fft_cmuladd_c,
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

static void impl_dsp_ops_free(struct dsp_ops *ops)
{
	spa_zero(*ops);
}

int dsp_ops_init(struct dsp_ops *ops)
{
	const struct dsp_info *info;

	info = find_dsp_info(ops->cpu_flags);
	if (info == NULL)
		return -ENOTSUP;

	ops->priv = info;
	ops->free = impl_dsp_ops_free;
	ops->funcs = info->funcs;

	return 0;
}
