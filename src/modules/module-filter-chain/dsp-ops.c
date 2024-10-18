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
		.funcs.mix_gain = dsp_mix_gain_avx,
		.funcs.biquad_run = dsp_biquad_run_sse,
		.funcs.sum = dsp_sum_avx,
		.funcs.linear = dsp_linear_c,
		.funcs.mult = dsp_mult_c,
		.funcs.fft_new = dsp_fft_new_c,
		.funcs.fft_free = dsp_fft_free_c,
		.funcs.fft_run = dsp_fft_run_c,
		.funcs.fft_cmul = dsp_fft_cmul_avx,
		.funcs.fft_cmuladd = dsp_fft_cmuladd_avx,
		.funcs.biquadn_run = dsp_biquadn_run_sse,
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
		.funcs.fft_run = dsp_fft_run_c,
		.funcs.fft_cmul = dsp_fft_cmul_sse,
		.funcs.fft_cmuladd = dsp_fft_cmuladd_sse,
		.funcs.biquadn_run = dsp_biquadn_run_sse,
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
		.funcs.fft_run = dsp_fft_run_c,
		.funcs.fft_cmul = dsp_fft_cmul_c,
		.funcs.fft_cmuladd = dsp_fft_cmuladd_c,
		.funcs.biquadn_run = dsp_biquadn_run_c,
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

static void impl_dsp_ops_free(struct dsp_ops *ops)
{
	spa_zero(*ops);
}

int dsp_ops_init(struct dsp_ops *ops, uint32_t cpu_flags)
{
	const struct dsp_info *info;

	info = find_dsp_info(cpu_flags);
	if (info == NULL)
		return -ENOTSUP;

	ops->cpu_flags = cpu_flags;
	ops->priv = info;
	ops->free = impl_dsp_ops_free;
	ops->funcs = info->funcs;

	return 0;
}

int dsp_ops_benchmark(void)
{
	struct dsp_ops ops[3];
	uint32_t i;
	struct biquad bq;
	float in[2048], out[2048];
	struct timespec ts;
	uint64_t t1, t2, t3, t4;

	dsp_ops_init(&ops[0], 0);
	dsp_ops_init(&ops[1], SPA_CPU_FLAG_SSE);
	dsp_ops_init(&ops[2], SPA_CPU_FLAG_AVX);

	clock_gettime(CLOCK_MONOTONIC, &ts);
        t1 = SPA_TIMESPEC_TO_NSEC(&ts);

	for (i = 0; i < 8192; i++)
		dsp_ops_biquad_run(&ops[0], &bq, out, in, 2048);

	clock_gettime(CLOCK_MONOTONIC, &ts);
        t2 = SPA_TIMESPEC_TO_NSEC(&ts);

	for (i = 0; i < 8192; i++)
		dsp_ops_biquad_run(&ops[1], &bq, out, in, 2048);

	clock_gettime(CLOCK_MONOTONIC, &ts);
        t3 = SPA_TIMESPEC_TO_NSEC(&ts);

	for (i = 0; i < 8192; i++)
		dsp_ops_biquad_run(&ops[2], &bq, out, in, 2048);

	clock_gettime(CLOCK_MONOTONIC, &ts);
        t4 = SPA_TIMESPEC_TO_NSEC(&ts);

	fprintf(stderr, "%"PRIu64" %"PRIu64" %"PRIu64" speedup:%f\n",
			t2-t1, t3-t2, t4-t3,
			((double)(t2-t1))/(t3-t2));
	return 0;
}
