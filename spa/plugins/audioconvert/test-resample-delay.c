/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <errno.h>
#include <time.h>
#include <math.h>

#include <spa/support/log-impl.h>
#include <spa/debug/mem.h>

SPA_LOG_IMPL(logger);

#include "resample.h"


static float samp_in[65536];
static float samp_out[65536];
static bool force_print;

static void assert_test(bool check)
{
	if (!check)
		fprintf(stderr, "FAIL\n\n");
#if 1
	spa_assert_se(check);
#endif
}

static double difference(double delay, const float *a, size_t len_a, const float *b, size_t len_b, double b_rate)
{
	size_t i;
	float c = 0, wa = 0, wb = 0;

	/* Difference measure: sum((a-b)^2) / sqrt(sum a^2 sum b^2); restricted to overlap */
	for (i = 0; i < len_a; ++i) {
		double jf = (i + delay) * b_rate;
		int j;
		double x;
		float bv;

		j = (int)floor(jf);
		if (j < 0 || j + 1 >= (int)len_b)
			continue;

		x = jf - j;
		bv = (float)((1 - x) * b[j] + x * b[j + 1]);

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
	double b_rate;
};

static double find_delay_func(double x, void *user_data)
{
	const struct find_delay_data *data = user_data;

	return difference((float)x, data->a, data->len_a, data->b, data->len_b, data->b_rate);
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

static double find_delay(const float *a, size_t len_a, const float *b, size_t len_b, double b_rate, int maxdelay, double tol)
{
	struct find_delay_data data = { .a = a, .len_a = len_a, .b = b, .len_b = len_b, .b_rate = b_rate };
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

	delay = find_delay(v1, SPA_N_ELEMENTS(v1), v2, SPA_N_ELEMENTS(v2), 1, 50, tol);
	expect = 3.1234f;
	fprintf(stderr, "find_delay = %g (exact %g)\n", delay, expect);
	assert_test(expect - 2*tol < delay && delay < expect + 2*tol);

	for (i = 0; i < 1024; ++i) {
		v1[i] = sinf(0.1f * i);
		v2[i] = sinf(0.1f * (i*3.0f/4 - 3.1234f));
	}

	delay = find_delay(v1, SPA_N_ELEMENTS(v1), v2, SPA_N_ELEMENTS(v2), 4.0/3.0, 50, tol);
	expect = 3.1234f;
	fprintf(stderr, "find_delay = %g (exact %g)\n", delay, expect);
	assert_test(expect - 2*tol < delay && delay < expect + 2*tol);
}

static uint32_t feed_sine(struct resample *r, uint32_t in, uint32_t *inp, uint32_t *phase, bool print)
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

	if (print || force_print) {
		fprintf(stderr, "inp(%u) = ", in);
		for (uint32_t i = 0; i < in; ++i)
			fprintf(stderr, "%g, ", samp_in[i]);
		fprintf(stderr, "\n\n");

		fprintf(stderr, "out(%u) = ", out);
		for (uint32_t i = 0; i < out; ++i)
			fprintf(stderr, "%g, ", samp_out[i]);
		fprintf(stderr, "\n\n");
	} else {
		fprintf(stderr, "inp(%u) = ...\n", in);
		fprintf(stderr, "out(%u) = ...\n", out);
	}

	*phase += in;
	*inp = in;
	return out;
}

static void check_delay(double rate, uint32_t out_rate, uint32_t options)
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
	r.o_rate = out_rate;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	r.options = options;
	resample_native_init(&r);

	resample_update_rate(&r, rate);

	feed_sine(&r, 512, &in, &in_phase, false);

	/* Test delay */
	expect = resample_delay(&r) + (double)resample_phase_ns(&r) * 48000 / SPA_NSEC_PER_SEC;
	out = feed_sine(&r, 256, &in, &in_phase, true);
	got = find_delay(samp_in, in, samp_out, out, out_rate / 48000.0, 100, tol);

	fprintf(stderr, "delay: expect = %g, got = %g\n", expect, got);
	assert_test(expect - 4*tol < got && got < expect + 4*tol);

	resample_free(&r);
}

static void test_delay_copy(void)
{
	fprintf(stderr, "\n\n-- test_delay_copy (no prefill)\n\n");
	check_delay(1, 48000, 0);

	fprintf(stderr, "\n\n-- test_delay_copy (prefill)\n\n");
	check_delay(1, 48000, RESAMPLE_OPTION_PREFILL);
}

static void test_delay_full(void)
{
	const uint32_t rates[] = { 16000, 32000, 44100, 48000, 88200, 96000, 144000, 192000 };
	unsigned int i;

	for (i = 0; i < SPA_N_ELEMENTS(rates); ++i) {
		fprintf(stderr, "\n\n-- test_delay_full(%u, no prefill)\n\n", rates[i]);
		check_delay(1, rates[i], 0);
		fprintf(stderr, "\n\n-- test_delay_full(%u, prefill)\n\n", rates[i]);
		check_delay(1, rates[i], RESAMPLE_OPTION_PREFILL);
	}
}

static void test_delay_interp(void)
{
	fprintf(stderr, "\n\n-- test_delay_interp(no prefill)\n\n");
	check_delay(1 + 1e-12, 48000, 0);

	fprintf(stderr, "\n\n-- test_delay_interp(prefill)\n\n");
	check_delay(1 + 1e-12, 48000, RESAMPLE_OPTION_PREFILL);
}

static void check_delay_vary_rate(double rate, double end_rate, uint32_t out_rate, uint32_t options)
{
	const double tol = 0.001;
	struct resample r;
	uint32_t in_phase = 0;
	uint32_t in, out;
	double expect, got;

	fprintf(stderr, "\n\n-- check_delay_vary_rate(%g, %.14g, %u, %s)\n\n", rate, end_rate, out_rate,
			(options & RESAMPLE_OPTION_PREFILL) ? "prefill" : "no prefill");

	spa_zero(r);
	r.log = &logger.log;
	r.channels = 1;
	r.i_rate = 48000;
	r.o_rate = out_rate;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	r.options = options;
	resample_native_init(&r);

	/* Cause nonzero resampler phase */
	resample_update_rate(&r, rate);
	feed_sine(&r, 128, &in, &in_phase, false);

	resample_update_rate(&r, 1.7);
	feed_sine(&r, 128, &in, &in_phase, false);

	resample_update_rate(&r, end_rate);
	feed_sine(&r, 128, &in, &in_phase, false);
	feed_sine(&r, 255, &in, &in_phase, false);

	/* Test delay */
	expect = (double)resample_delay(&r) + (double)resample_phase_ns(&r) * 48000 / SPA_NSEC_PER_SEC;
	out = feed_sine(&r, 256, &in, &in_phase, true);
	got = find_delay(samp_in, in, samp_out, out, out_rate/48000.0, 100, tol);

	fprintf(stderr, "delay: expect = %g, got = %g\n", expect, got);
	assert_test(expect - 4*tol < got && got < expect + 4*tol);

	resample_free(&r);
}


static void test_delay_interp_vary_rate(void)
{
	const uint32_t rates[] = { 32000, 44100, 48000, 88200, 96000 };
	const double factors[] = { 1.0123456789, 1.123456789, 1.203883, 1.23456789, 1.3456789 };
	unsigned int i, j;

	for (i = 0; i < SPA_N_ELEMENTS(rates); ++i) {
		for (j = 0; j < SPA_N_ELEMENTS(factors); ++j) {
			/* Interp at end */
			check_delay_vary_rate(factors[j], 1 + 1e-12, rates[i], 0);

			/* Copy/full at end */
			check_delay_vary_rate(factors[j], 1, rates[i], 0);

			/* Interp at end */
			check_delay_vary_rate(factors[j], 1 + 1e-12, rates[i], RESAMPLE_OPTION_PREFILL);

			/* Copy/full at end */
			check_delay_vary_rate(factors[j], 1, rates[i], RESAMPLE_OPTION_PREFILL);
		}
	}
}

static void run(uint32_t in_rate, uint32_t out_rate, double end_rate, double mid_rate, uint32_t options)
{
	const double tol = 0.001;
	struct resample r;
	uint32_t in_phase = 0;
	uint32_t in, out;
	double expect, got;

	spa_zero(r);
	r.log = &logger.log;
	r.channels = 1;
	r.i_rate = in_rate;
	r.o_rate = out_rate;
	r.quality = RESAMPLE_DEFAULT_QUALITY;
	r.options = options;
	resample_native_init(&r);

	/* Cause nonzero resampler phase */
	if (mid_rate != 0.0) {
		resample_update_rate(&r, mid_rate);
		feed_sine(&r, 128, &in, &in_phase, true);

		resample_update_rate(&r, 1.7);
		feed_sine(&r, 128, &in, &in_phase, true);
	}

	resample_update_rate(&r, end_rate);
	feed_sine(&r, 128, &in, &in_phase, true);
	feed_sine(&r, 255, &in, &in_phase, true);

	/* Test delay */
	expect = (double)resample_delay(&r) + (double)resample_phase_ns(&r) * (double)in_rate / SPA_NSEC_PER_SEC;
	out = feed_sine(&r, 256, &in, &in_phase, true);
	got = find_delay(samp_in, in, samp_out, out, ((double)out_rate)/in_rate, 100, tol);

	fprintf(stderr, "delay: expect = %g, got = %g\n", expect, got);
	if (!(expect - 4*tol < got && got < expect + 4*tol))
		fprintf(stderr, "FAIL\n\n");

	resample_free(&r);
}

int main(int argc, char *argv[])
{
        static const struct option long_options[] = {
		{ "in-rate", required_argument, NULL, 'i' },
		{ "out-rate", required_argument, NULL, 'o' },
		{ "end-full", no_argument, NULL, 'f' },
		{ "end-interp", no_argument, NULL, 'p' },
		{ "mid-rate", required_argument, NULL, 'm' },
		{ "prefill", no_argument, NULL, 'r' },
		{ "print", no_argument, NULL, 'P' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0}
        };
	const char *help = "%s [options]\n"
		"\n"
		"Check resampler delay. If no arguments, run tests.\n"
		"\n"
		"-i | --in-rate INRATE      input rate\n"
		"-o | --out-rate OUTRATE    output rate\n"
		"-f | --end-full            force full (or copy) resampler\n"
		"-p | --end-interp          force interp resampler\n"
		"-m | --mid-rate RELRATE    force rate adjustment in the middle\n"
		"-r | --prefill             enable prefill\n"
		"-P | --print               force printing\n"
		"\n";
	uint32_t in_rate = 0, out_rate = 0;
	double end_rate = 1, mid_rate = 0;
	uint32_t options = 0;
	int c;

	logger.log.level = SPA_LOG_LEVEL_TRACE;

	while ((c = getopt_long(argc, argv, "i:o:fpm:rPh", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			fprintf(stderr, help, argv[0]);
			return 0;
		case 'i':
			if (!spa_atou32(optarg, &in_rate, 0))
				goto error_arg;
			break;
		case 'o':
			if (!spa_atou32(optarg, &out_rate, 0))
				goto error_arg;
			break;
		case 'f':
			end_rate = 1;
			break;
		case 'p':
			end_rate = 1 + 1e-12;
			break;
		case 'm':
			if (!spa_atod(optarg, &mid_rate))
				goto error_arg;
			break;
		case 'r':
			options = RESAMPLE_OPTION_PREFILL;
			break;
		case 'P':
			force_print = true;
			break;
		default:
			goto error_arg;
		}
	}

	if (in_rate && out_rate) {
		run(in_rate, out_rate, end_rate, mid_rate, options);
		return 0;
	}

	test_find_delay();
	test_delay_copy();
	test_delay_full();
	test_delay_interp();
	test_delay_interp_vary_rate();

	return 0;

error_arg:
	fprintf(stderr, "Invalid arguments\n");
	return 1;
}
