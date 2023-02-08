/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <spa/support/log-impl.h>
#include <spa/debug/mem.h>

static uint32_t cpu_flags;

SPA_LOG_IMPL(logger);

#define MATRIX(...) (float[]) { __VA_ARGS__ }

#include "test-helper.h"
#include "channelmix-ops.c"

#define CLOSE_ENOUGH(a,b)	(fabs((a)-(b)) < 0.000001f)

static void dump_matrix(struct channelmix *mix, float *coeff)
{
	uint32_t i, j;

	for (i = 0; i < mix->dst_chan; i++) {
		for (j = 0; j < mix->src_chan; j++) {
			float v = mix->matrix[i][j];
			spa_log_debug(mix->log, "%d %d: %f <-> %f", i, j, v, *coeff);
			spa_assert_se(CLOSE_ENOUGH(v, *coeff));
			coeff++;
		}
	}
}

static void test_mix(uint32_t src_chan, uint32_t src_mask, uint32_t dst_chan, uint32_t dst_mask, uint32_t options, float *coeff)
{
	struct channelmix mix;

	spa_log_debug(&logger.log, "start %d->%d (%08x -> %08x)", src_chan, dst_chan, src_mask, dst_mask);

	spa_zero(mix);
	mix.options = options;
	mix.src_chan = src_chan;
	mix.dst_chan = dst_chan;
	mix.src_mask = src_mask;
	mix.dst_mask = dst_mask;
	mix.log = &logger.log;
	mix.fc_cutoff = 120.0f;
	mix.lfe_cutoff = 12000.0f;

	spa_assert_se(channelmix_init(&mix) == 0);
	channelmix_set_volume(&mix, 1.0f, false, 0, NULL);
	dump_matrix(&mix, coeff);
}

static void test_1_N_MONO(void)
{
	test_mix(1, _M(MONO), 2, _M(FL)|_M(FR), 0,
			MATRIX(1.0, 1.0));
	test_mix(1, _M(MONO), 3, _M(FL)|_M(FR)|_M(LFE), 0,
			MATRIX(1.0, 1.0, 0.0));
	test_mix(1, _M(MONO), 3, _M(FL)|_M(FR)|_M(LFE), CHANNELMIX_OPTION_UPMIX,
			MATRIX(1.0, 1.0, 1.0));
	test_mix(1, _M(MONO), 4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 0,
			MATRIX(1.0, 1.0, 0.0, 0.0));
	test_mix(1, _M(MONO), 4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), CHANNELMIX_OPTION_UPMIX,
			MATRIX(1.0, 1.0, 1.0, 1.0));
	test_mix(1, _M(MONO), 4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 0,
			MATRIX(1.0, 1.0, 0.0, 0.0));
	test_mix(1, _M(MONO), 4, _M(FL)|_M(FR)|_M(RL)|_M(RR), CHANNELMIX_OPTION_UPMIX,
			MATRIX(1.0, 1.0, 0.0, 0.0));
	test_mix(1, _M(MONO), 12, 0, 0,
			MATRIX(1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
			       1.0, 1.0, 1.0, 1.0, 1.0, 1.0));
}

static void test_1_N_FC(void)
{
	test_mix(1, _M(FC), 2, _M(FL)|_M(FR), 0,
			MATRIX(0.707107, 0.707107));
	test_mix(1, _M(FC), 3, _M(FL)|_M(FR)|_M(LFE), 0,
			MATRIX(0.707107, 0.707107, 0.0));
	test_mix(1, _M(FC), 4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 0,
			MATRIX(0.0, 0.0, 1.0, 0.0));
	test_mix(1, _M(FC), 4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 0,
			MATRIX(0.707107, 0.707107, 0.0, 0.0));
	test_mix(1, _M(FC), 12, 0, 0,
			MATRIX(1.0, 1.0, 1.0, 1.0, 1.0, 1.0,
			       1.0, 1.0, 1.0, 1.0, 1.0, 1.0));
}

static void test_N_1(void)
{
	test_mix(1, _M(MONO), 1, _M(MONO), 0,
			MATRIX(1.0));
	test_mix(1, _M(MONO), 1, _M(FC), 0,
			MATRIX(1.0));
	test_mix(1, _M(FC), 1, _M(MONO), 0,
			MATRIX(1.0));
	test_mix(1, _M(FC), 1, _M(FC), 0,
			MATRIX(1.0));
	test_mix(2, _M(FL)|_M(FR), 1, _M(MONO), 0,
			MATRIX(0.5, 0.5));
	test_mix(12, 0, 1, _M(MONO), 0,
			MATRIX(0.083333, 0.083333, 0.083333, 0.083333, 0.083333, 0.083333,
			       0.083333, 0.083333, 0.083333, 0.083333, 0.083333, 0.0833333));
}

static void test_2_N(void)
{
	test_mix(2, _M(FL)|_M(FR), 1, _M(MONO), 0, MATRIX(0.5, 0.5));
	test_mix(2, _M(FL)|_M(FR), 1, 0, 0, MATRIX(0.5, 0.5));
	test_mix(2, _M(FL)|_M(FR), 2, 0, 0, MATRIX(1.0, 0.0, 0.0, 1.0));
	test_mix(2, _M(FL)|_M(FR), 2, _M(MONO), 0, MATRIX(1.0, 0.0, 0.0, 1.0));
	test_mix(2, _M(FL)|_M(FR), 2, _M(FL)|_M(FR), 0, MATRIX(1.0, 0.0, 0.0, 1.0));
	test_mix(2, _M(FL)|_M(FR), 4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 0,
			MATRIX(1.0, 0.0,
			       0.0, 1.0,
			       0.0, 0.0,
			       0.0, 0.0));
	test_mix(2, _M(FL)|_M(FR), 4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), CHANNELMIX_OPTION_UPMIX,
			MATRIX(1.0, 0.0,
			       0.0, 1.0,
			       0.707107, 0.707107,
			       0.5, 0.5));
	test_mix(2, _M(FL)|_M(FR), 6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 0,
			MATRIX(1.0, 0.0,
			       0.0, 1.0,
			       0.0, 0.0,
			       0.0, 0.0,
			       0.0, 0.0,
			       0.0, 0.0));
	test_mix(2, _M(FL)|_M(FR), 6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), CHANNELMIX_OPTION_UPMIX,
			MATRIX(1.0, 0.0,
			       0.0, 1.0,
			       0.707107, 0.707107,
			       0.5, 0.5,
			       0.0, 0.0,
			       0.0, 0.0));
}

static void test_3p1_N(void)
{
	test_mix(4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 1, _M(MONO), 0,
			MATRIX(0.333333, 0.333333, 0.333333, 0.0));
	test_mix(4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 2, _M(FL)|_M(FR), 0,
			MATRIX(1.0, 0.0, 0.707107, 0.0,
			       0.0, 1.0, 0.707107, 0.0 ));
	test_mix(4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 3, _M(FL)|_M(FR)|_M(LFE), 0,
			MATRIX(1.0, 0.0, 0.707107, 0.0,
			       0.0, 1.0, 0.707107, 0.0,
			       0.0, 0.0, 0.0, 1.0 ));
	test_mix(4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 0,
			MATRIX(1.0, 0.0, 0.0, 0.0,
			       0.0, 1.0, 0.0, 0.0,
			       0.0, 0.0, 1.0, 0.0,
			       0.0, 0.0, 0.0, 1.0,));
	test_mix(4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 0,
			MATRIX(1.0, 0.0, 0.707107, 0.0,
			       0.0, 1.0, 0.707107, 0.0,
			       0.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 0.0, 0.0,));
}

static void test_4_N(void)
{
	test_mix(4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 1, _M(MONO), 0,
			MATRIX(0.25, 0.25, 0.25, 0.25));
	test_mix(4, _M(FL)|_M(FR)|_M(SL)|_M(SR), 1, _M(MONO), 0,
			MATRIX(0.25, 0.25, 0.25, 0.25));
	test_mix(4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 2, _M(FL)|_M(FR), 0,
			MATRIX(1.0, 0.0, 0.707107, 0.0,
			       0.0, 1.0, 0.0, 0.707107));
	test_mix(4, _M(FL)|_M(FR)|_M(SL)|_M(SR), 2, _M(FL)|_M(FR), 0,
			MATRIX(1.0, 0.0, 0.707107, 0.0,
			       0.0, 1.0, 0.0, 0.707107));
	test_mix(4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 3, _M(FL)|_M(FR)|_M(LFE), 0,
			MATRIX(1.0, 0.0, 0.707107, 0.0,
			       0.0, 1.0, 0.0, 0.707107,
			       0.0, 0.0, 0.0, 0.0));
	test_mix(4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 0,
			MATRIX(1.0, 0.0, 0.0, 0.0,
			       0.0, 1.0, 0.0, 0.0,
			       0.0, 0.0, 1.0, 0.0,
			       0.0, 0.0, 0.0, 1.0));
	test_mix(4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 0,
			MATRIX(1.0, 0.0, 0.707107, 0.0,
			       0.0, 1.0, 0.0, 0.707107,
			       0.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 0.0, 0.0));
	test_mix(4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), CHANNELMIX_OPTION_UPMIX,
			MATRIX(1.0, 0.0, 0.707107, 0.0,
			       0.0, 1.0, 0.0, 0.707107,
			       0.707107, 0.707107, 0.0, 0.0,
			       0.5, 0.5, 0.0, 0.0));
}

static void test_5p1_N(void)
{
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 1, _M(MONO), 0,
			MATRIX(0.20, 0.20, 0.20, 0.0, 0.20, 0.20));
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 2, _M(FL)|_M(FR), 0,
			MATRIX(1.0, 0.0, 0.707107, 0.0, 0.707107, 0.0,
			       0.0, 1.0, 0.707107, 0.0, 0.0, 0.707107));
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(RL)|_M(RR), 2, _M(FL)|_M(FR), 0,
			MATRIX(1.0, 0.0, 0.707107, 0.0, 0.707107, 0.0,
			       0.0, 1.0, 0.707107, 0.0, 0.0, 0.707107));
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 3, _M(FL)|_M(FR)|_M(LFE), 0,
			MATRIX(1.0, 0.0, 0.707107, 0.0, 0.707107, 0.0,
			       0.0, 1.0, 0.707107, 0.0, 0.0, 0.707107,
			       0.0, 0.0, 0.0, 1.0, 0.0, 0.0));
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 0,
			MATRIX(1.0, 0.0, 0.0, 0.0, 0.707107, 0.0,
			       0.0, 1.0, 0.0, 0.0, 0.0, 0.707107,
			       0.0, 0.0, 1.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 0.0, 1.0, 0.0, 0.0));
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 0,
			MATRIX(1.0, 0.0, 0.707107, 0.0, 0.0, 0.0,
			       0.0, 1.0, 0.707107, 0.0, 0.0, 0.0,
			       0.0, 0.0, 0.0, 0.0, 1.0, 0.0,
			       0.0, 0.0, 0.0, 0.0, 0.0, 1.0));
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 5, _M(FL)|_M(FR)|_M(FC)|_M(SL)|_M(SR), 0,
			MATRIX(1.0, 0.0, 0.0, 0.0, 0.0, 0.0,
			       0.0, 1.0, 0.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 1.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 0.0, 0.0, 1.0, 0.0,
			       0.0, 0.0, 0.0, 0.0, 0.0, 1.0));
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 0,
			MATRIX(1.0, 0.0, 0.0, 0.0, 0.0, 0.0,
			       0.0, 1.0, 0.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 1.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
			       0.0, 0.0, 0.0, 0.0, 1.0, 0.0,
			       0.0, 0.0, 0.0, 0.0, 0.0, 1.0));
}

static void test_6p1_N(void)
{
	test_mix(7, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(RC)|_M(SL)|_M(SR), 1, _M(MONO), 0,
			MATRIX(0.166667, 0.166667, 0.166667, 0.0, 0.166667, 0.166667, 0.166667));
	test_mix(7, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR)|_M(RC),
		 6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 0,
			MATRIX(1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
			       0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.707107,
			       0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.707107));
	test_mix(7, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR)|_M(RC),
		 6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(RL)|_M(RR), 0,
			MATRIX(1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
			       0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.707107,
			       0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.707107));
	test_mix(7, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(RC)|_M(RL)|_M(RR),
		 6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(RL)|_M(RR), 0,
			MATRIX(1.0, 0.0, 0.0, 0.0, 0.0,      0.0, 0.0,
			       0.0, 1.0, 0.0, 0.0, 0.0,      0.0, 0.0,
			       0.0, 0.0, 1.0, 0.0, 0.0,      0.0, 0.0,
			       0.0, 0.0, 0.0, 1.0, 0.0,      0.0, 0.0,
			       0.0, 0.0, 0.0, 0.0, 0.707107, 1.0, 0.0,
			       0.0, 0.0, 0.0, 0.0, 0.707107, 0.0, 1.0));
	test_mix(7, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR)|_M(RC),
		 8, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR)|_M(RL)|_M(RR), 0,
			MATRIX(1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
			       0.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0,
			       0.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0,
			       0.0, 0.0, 0.0, 0.0, 0.0, 1.0, 0.0,
			       0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.707107,
			       0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.707107));
}

static void test_7p1_N(void)
{
	test_mix(8, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR)|_M(RL)|_M(RR), 1, _M(MONO), 0,
			MATRIX(0.142857, 0.142857, 0.142857, 0.0, 0.142857, 0.142857, 0.142857, 0.142857));
	test_mix(8, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR)|_M(RL)|_M(RR), 2, _M(FL)|_M(FR), 0,
			MATRIX(1.0, 0.0, 0.707107, 0.0, 0.707107, 0.0, 0.707107, 0.0,
			       0.0, 1.0, 0.707107, 0.0, 0.0, 0.707107, 0.0, 0.707107));
}

static void check_samples(float **s1, float **s2, uint32_t n_s, uint32_t n_samples)
{
	uint32_t i, j;
	for (i = 0; i < n_s; i++) {
		for (j = 0; j < n_samples; j++) {
			spa_assert_se(CLOSE_ENOUGH(s1[i][j], s2[i][j]));
		}
	}
}

static void run_n_m_impl(struct channelmix *mix, const void **src, uint32_t n_samples)
{
	uint32_t dst_chan = mix->dst_chan, i;
	float dst_c_data[dst_chan][n_samples];
	float dst_x_data[dst_chan][n_samples];
	void *dst_c[dst_chan], *dst_x[dst_chan];

	for (i = 0; i < dst_chan; i++) {
		dst_c[i] = dst_c_data[i];
		dst_x[i] = dst_x_data[i];
	}

	channelmix_f32_n_m_c(mix, dst_c, src, n_samples);

	channelmix_f32_n_m_c(mix, dst_x, src, n_samples);
	check_samples((float**)dst_c, (float**)dst_x, dst_chan, n_samples);

#if defined(HAVE_SSE)
	if (cpu_flags & SPA_CPU_FLAG_SSE) {
		channelmix_f32_n_m_sse(mix, dst_x, src, n_samples);
		check_samples((float**)dst_c, (float**)dst_x, dst_chan, n_samples);
	}
#endif
}

static void test_n_m_impl(void)
{
	struct channelmix mix;
	unsigned int i, j;
#define N_SAMPLES	251
	float src_data[16][N_SAMPLES], *src[16];

	spa_log_debug(&logger.log, "start");

	for (i = 0; i < 16; i++) {
		for (j = 0; j < N_SAMPLES; j++)
			src_data[i][j] = (drand48() - 0.5f) * 2.5f;
		src[i] = src_data[i];
	}

	spa_zero(mix);
	mix.src_chan = 16;
	mix.dst_chan = 12;
	mix.log = &logger.log;
	mix.cpu_flags = cpu_flags;
	spa_assert_se(channelmix_init(&mix) == 0);
	channelmix_set_volume(&mix, 1.0f, false, 0, NULL);

	/* identity matrix */
	run_n_m_impl(&mix, (const void**)src, N_SAMPLES);

	/* some zero destination */
	mix.matrix_orig[2][2] = 0.0f;
	mix.matrix_orig[7][7] = 0.0f;
	channelmix_set_volume(&mix, 1.0f, false, 0, NULL);
	run_n_m_impl(&mix, (const void**)src, N_SAMPLES);

	/* random matrix */
	for (i = 0; i < mix.dst_chan; i++) {
		for (j = 0; j < mix.src_chan; j++) {
			mix.matrix_orig[i][j] = drand48() - 0.5f;
		}
	}
	channelmix_set_volume(&mix, 1.0f, false, 0, NULL);

	run_n_m_impl(&mix, (const void**)src, N_SAMPLES);
}

int main(int argc, char *argv[])
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	srand48(SPA_TIMESPEC_TO_NSEC(&ts));

	logger.log.level = SPA_LOG_LEVEL_TRACE;

	cpu_flags = get_cpu_flags();
	printf("got CPU flags %d\n", cpu_flags);

	test_1_N_MONO();
	test_1_N_FC();
	test_N_1();
	test_2_N();
	test_3p1_N();
	test_4_N();
	test_5p1_N();
	test_6p1_N();
	test_7p1_N();

	test_n_m_impl();

	return 0;
}
