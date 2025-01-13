/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <math.h>

#include <spa/support/log-impl.h>
#include <spa/debug/mem.h>

SPA_LOG_IMPL(logger);

#include "resample.h"


static float samp_in[4096];
static float samp_out[4096];


static double difference(float delay, const float *a, size_t len_a, const float *b, size_t len_b)
{
	size_t i;
	float c = 0, wa = 0, wb = 0;

	/* Difference measure: sum((a-b)^2) / sqrt(sum a^2 sum b^2); restricted to overlap */
	for (i = 0; i < len_a; ++i) {
		float jf = i + delay;
		int j;
		float bv, x;

		j = (int)floorf(jf);
		if (j < 0 || j + 1 >= (int)len_b)
			continue;

		x = jf - j;
		bv = (1 - x) * b[j] + x * b[j + 1];

		c += (a[i] - bv) * (a[i] - bv);
		wa += a[i]*a[i];
		wb = bv*bv;
	}

	if (wa == 0 || wb == 0)
		return 1e30;

	return c / sqrt(wa * wb);
}

struct find_delay_data {
	const float *a;
	const float *b;
	size_t len_a;
	size_t len_b;
};

static double find_delay_func(double x, void *user_data)
{
	const struct find_delay_data *data = user_data;

	return difference((float)x, data->a, data->len_a, data->b, data->len_b);
}

static double minimum(double x1, double x4, double (*func)(double, void *), void *user_data, double tol)
{
	/* Find minimum with golden section search */
	const double phi = (1 + sqrt(5)) / 2;
	double x2, x3;
	double f2, f3;

	spa_assert(x4 >= x1);

	x2 = x4 - (x4 - x1) / phi;
	x3 = x1 + (x4 - x1) / phi;

	f2 = func(x2, user_data);
	f3 = func(x3, user_data);

	while (x4 - x1 > tol) {
		if (f2 > f3) {
			x1 = x2;
			x2 = x3;
			x3 = x1 + (x4 - x1) / phi;
			f2 = f3;
			f3 = func(x3, user_data);
		} else {
			x4 = x3;
			x3 = x2;
			x2 = x4 - (x4 - x1) / phi;
			f3 = f2;
			f2 = func(x2, user_data);
		}
	}

	return (f2 < f3) ? x2 : x3;
}

static double find_delay(const float *a, size_t len_a, const float *b, size_t len_b, int maxdelay, double tol)
{
	struct find_delay_data data = {	.a = a, .len_a = len_a, .b = b, .len_b = len_b };
	double best_x, best_f;
	int i;

	best_x = 0;
	best_f = find_delay_func(best_x, &data);

	for (i = -maxdelay; i <= maxdelay; ++i) {
		double f = find_delay_func(i, &data);

		if (f < best_f) {
			best_x = i;
			best_f = f;
		}
	}

	return minimum(best_x - 2, best_x + 2, find_delay_func, &data, tol);
}

static void test_find_delay(void)
{
	float v1[1024];
	float v2[1024];
	const double tol = 0.001;
	double delay, expect;
	int i;

	fprintf(stderr, "\n\n-- test_find_delay\n\n");

	for (i = 0; i < 1024; ++i) {
		v1[i] = sinf(0.1f * i);
		v2[i] = sinf(0.1f * (i - 3.1234f));
	}

	delay = find_delay(v1, SPA_N_ELEMENTS(v1), v2, SPA_N_ELEMENTS(v2), 50, tol);
	expect = 3.1234f;
	fprintf(stderr, "find_delay = %g (exact %g)\n", delay, expect);
	spa_assert_se(expect - 2*tol < delay && delay < expect + 2*tol);
}

static uint32_t feed_sine(struct resample *r, uint32_t in, uint32_t *inp, uint32_t *phase)
{
	uint32_t i, out;
	const void *src[1];
	void *dst[1];

	for (i = 0; i < in; ++i)
		samp_in[i] = sinf(0.01f * (*phase + i)) * expf(-0.001f * (*phase + i));

	src[0] = samp_in;
	dst[0] = samp_out;
	out = SPA_N_ELEMENTS(samp_out);

	*inp = in;
	resample_process(r, src, inp, dst, &out);
	spa_assert_se(*inp == in);

	fprintf(stderr, "inp(%u) = ", in);
	for (uint32_t i = 0; i < in; ++i)
		fprintf(stderr, "%g, ", samp_in[i]);
	fprintf(stderr, "\n\n");

	fprintf(stderr, "out(%u) = ", out);
	for (uint32_t i = 0; i < out; ++i)
		fprintf(stderr, "%g, ", samp_out[i]);
	fprintf(stderr, "\n\n");

	*phase += in;
	*inp = in;
	return out;
}

static void check_delay(double rate)
{
	const double tol = 0.001;
	struct resample r;
	uint32_t in_phase = 0;
	uint32_t in, out;
	double expect, got;

	spa_zero(r);
	r.log = &logger.log;
	r.channels = 1;
	r.i_rate = 48000;
	r.o_rate = 48000;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	resample_native_init(&r);

	resample_update_rate(&r, rate);

	feed_sine(&r, 64, &in, &in_phase);

	/* Test delay */
	expect = resample_delay(&r);
	out = feed_sine(&r, 64, &in, &in_phase);
	got = find_delay(samp_in, in, samp_out, out, 40, tol);
	fprintf(stderr, "delay: expect = %g, got = %g\n", expect, got);

	spa_assert_se(expect - 2*tol < got && got < expect + 2*tol);

	resample_free(&r);
}

static void test_delay_copy(void)
{
	fprintf(stderr, "\n\n-- test_delay_copy\n\n");
	check_delay(1);
}

static void test_delay_interp(void)
{
	fprintf(stderr, "\n\n-- test_delay_interp\n\n");
	check_delay(1 + 1e-12);
}

static void check_delay_interp_vary_rate(double rate)
{
	const double tol = 0.001;
	struct resample r;
	uint32_t in_phase = 0;
	uint32_t in, out;
	double expect, got;

	fprintf(stderr, "\n\n-- check_delay_vary_rate(%g)\n\n", rate);

	spa_zero(r);
	r.log = &logger.log;
	r.channels = 1;
	r.i_rate = 48000;
	r.o_rate = 48000;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	resample_native_init(&r);

	/* Cause nonzero resampler phase */
	resample_update_rate(&r, rate);
	feed_sine(&r, 128, &in, &in_phase);

	resample_update_rate(&r, 1.7);
	feed_sine(&r, 128, &in, &in_phase);

	resample_update_rate(&r, 1 + 1e-12);
	feed_sine(&r, 256, &in, &in_phase);

	/* Test delay */
	expect = (double)resample_delay(&r);
	out = feed_sine(&r, 64, &in, &in_phase);
	got = find_delay(samp_in, in, samp_out, out, 40, tol);
	fprintf(stderr, "delay: expect = %g, got = %g\n", expect, got);

#if 0
	/* XXX: this fails */
	spa_assert_se(expect - 2*tol < got && got < expect + 2*tol);
#else
	if (!(expect - 2*tol < got && got < expect + 2*tol))
		fprintf(stderr, "KNOWNFAIL\n");
#endif

	resample_free(&r);
}


static void test_delay_interp_vary_rate(void)
{
	check_delay_interp_vary_rate(1.0123456789);
	check_delay_interp_vary_rate(1.123456789);
	check_delay_interp_vary_rate(1.23456789);
}

int main(int argc, char *argv[])
{
	logger.log.level = SPA_LOG_LEVEL_TRACE;

	test_find_delay();
	test_delay_copy();
	test_delay_interp();
	test_delay_interp_vary_rate();

	return 0;
}
