/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <spa/support/log-impl.h>
#include <spa/debug/mem.h>

SPA_LOG_IMPL(logger);

#include "resample.h"
#include "resample-native-impl.h"

#define N_SAMPLES	253
#define N_CHANNELS	11

static float samp_in[N_SAMPLES * 4];
static float samp_out[N_SAMPLES * 4];

static void feed_1(struct resample *r)
{
	uint32_t i;
	const void *src[1];
	void *dst[1];

	spa_zero(samp_out);
	src[0] = samp_in;
	dst[0] = samp_out;

	for (i = 0; i < 500; i++) {
		uint32_t in, out;

		in = out = 1;
		samp_in[0] = i;
		resample_process(r, src, &in, dst, &out);
		fprintf(stderr, "%d %d %f %d\n", i, in, samp_out[0], out);
	}
}

static void test_native(void)
{
	struct resample r;

	spa_zero(r);
	r.log = &logger.log;
	r.channels = 1;
	r.i_rate = 44100;
	r.o_rate = 44100;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	resample_native_init(&r);

	feed_1(&r);
	resample_free(&r);

	spa_zero(r);
	r.log = &logger.log;
	r.channels = 1;
	r.i_rate = 44100;
	r.o_rate = 48000;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	resample_native_init(&r);

	feed_1(&r);
	resample_free(&r);
}

static void pull_blocks(struct resample *r, uint32_t first, uint32_t size, uint32_t count)
{
	uint32_t i;
	float in[SPA_MAX(size, first) * 2];
	float out[SPA_MAX(size, first) * 2];
	const void *src[1];
	void *dst[1];
	uint32_t in_len, out_len;
	uint32_t pin_len, pout_len;

	src[0] = in;
	dst[0] = out;

	for (i = 0; i < count; i++) {
		pout_len = out_len = i == 0 ? first : size;
		pin_len = in_len = resample_in_len(r, out_len);

		resample_process(r, src, &pin_len, dst, &pout_len);

		fprintf(stderr, "%d: %d %d %d %d %d\n", i,
				in_len, pin_len, out_len, pout_len,
				resample_in_len(r, size));

		spa_assert_se(in_len == pin_len);
		spa_assert_se(out_len == pout_len);
	}
}

static void pull_blocks_out(struct resample *r, uint32_t first, uint32_t size, uint32_t count)
{
	uint32_t i;
	float in[SPA_MAX(size, first) * 2];
	float out[SPA_MAX(size, first) * 2];
	const void *src[1];
	void *dst[1];
	uint32_t in_len, out_len;
	uint32_t pin_len, pout_len;

	src[0] = in;
	dst[0] = out;

	for (i = 0; i < count; i++) {
		pin_len = in_len = i == 0 ? first : size;
		pout_len = out_len = resample_out_len(r, in_len);

		resample_process(r, src, &pin_len, dst, &pout_len);

		fprintf(stderr, "%d: %d %d %d %d %d\n", i,
				in_len, pin_len, out_len, pout_len,
				resample_out_len(r, size));

		spa_assert_se(in_len == pin_len);
		spa_assert_se(out_len == pout_len);
	}
}

static void check_inout_len(struct resample *r, uint32_t first, uint32_t size, double rate, float phase)
{
	struct native_data *data = r->data;

	resample_reset(r);
	resample_update_rate(r, rate);
	if (phase != 0.0)
		data->phase = FLOAT_TO_FIXP(phase);
	pull_blocks(r, first, size, 500);

	resample_reset(r);
	resample_update_rate(r, rate);
	if (phase != 0.0)
		data->phase = FLOAT_TO_FIXP(phase);
	pull_blocks_out(r, first, size, 500);
}

static void test_inout_len(void)
{
	struct resample r;

	spa_zero(r);
	r.log = &logger.log;
	r.channels = 1;
	r.i_rate = 32000;
	r.o_rate = 48000;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	resample_native_init(&r);

	check_inout_len(&r, 1024, 1024, 1.0, 0);
	resample_free(&r);

	spa_zero(r);
	r.log = &logger.log;
	r.channels = 1;
	r.i_rate = 44100;
	r.o_rate = 48000;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	resample_native_init(&r);

	check_inout_len(&r, 1024, 1024, 1.0, 0);
	resample_free(&r);

	spa_zero(r);
	r.log = &logger.log;
	r.channels = 1;
	r.i_rate = 48000;
	r.o_rate = 44100;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	resample_native_init(&r);

	check_inout_len(&r, 1024, 1024, 1.0, 0);
	resample_free(&r);

	spa_zero(r);
	r.log = &logger.log;
	r.channels = 1;
	r.i_rate = 44100;
	r.o_rate = 48000;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	resample_native_init(&r);

	check_inout_len(&r, 513, 64, 1.0, 0);
	resample_free(&r);

	spa_zero(r);
	r.log = &logger.log;
	r.channels = 1;
	r.i_rate = 32000;
	r.o_rate = 48000;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	resample_native_init(&r);

	check_inout_len(&r, 513, 64, 1.02, 0);
	resample_free(&r);

	spa_zero(r);
	r.log = &logger.log;
	r.channels = 1;
	r.i_rate = 32000;
	r.o_rate = 48000;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	resample_native_init(&r);

	check_inout_len(&r, 513, 64, 1.0002, 0);
	resample_free(&r);

	spa_zero(r);
	r.log = &logger.log;
	r.channels = 1;
	r.i_rate = 32000;
	r.o_rate = 48000;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	r.options = RESAMPLE_OPTION_PREFILL;
	resample_native_init(&r);

	check_inout_len(&r, 513, 64, 1.0002, 0);
	resample_free(&r);

	/* Test value of phase that in floating-point arithmetic produces
	 * inconsistent in_len
	 */
	spa_zero(r);
	r.log = &logger.log;
	r.channels = 1;
	r.i_rate = 8000;
	r.o_rate = 8000;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	r.options = RESAMPLE_OPTION_PREFILL;
	resample_native_init(&r);

	check_inout_len(&r, 64, 64, 1.0 + 1e-10, 7999.99f);
	resample_free(&r);

	/* Test value of phase that overflows filter buffer due to floating point rounding
	 * up to nearest
	 */
	spa_zero(r);
	r.log = &logger.log;
	r.channels = 1;
	r.i_rate = 8000;
	r.o_rate = 8000;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	r.options = RESAMPLE_OPTION_PREFILL;
	resample_native_init(&r);

	check_inout_len(&r, 64, 64, 1.0 + 1e-10, nextafterf(8000, 0));
	resample_free(&r);
}

int main(int argc, char *argv[])
{
	logger.log.level = SPA_LOG_LEVEL_TRACE;

	test_native();
	test_inout_len();

	return 0;
}
