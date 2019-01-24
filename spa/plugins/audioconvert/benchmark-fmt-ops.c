/* Spa
 *
 * Copyright © 2019 Wim Taymans
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
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include "fmt-ops.c"

#define N_SAMPLES	4096
#define N_CHANNELS	5

#define MAX_COUNT 1000

static uint8_t samp_in[N_SAMPLES * N_CHANNELS * 4];
static uint8_t samp_out[N_SAMPLES * N_CHANNELS * 4];

static void run_test(const char *name, bool in_packed, bool out_packed, convert_func_t func)
{
	const void *ip[N_CHANNELS];
	void *op[N_CHANNELS];
	int i, j, ic, oc, ns;
	struct timespec ts;
	uint64_t t1, t2;
	uint64_t count = 0;

	for (j = 0; j < N_CHANNELS; j++) {
		ip[j] = &samp_in[j * N_SAMPLES * 4];
		op[j] = &samp_out[j * N_SAMPLES * 4];
	}

	ic = in_packed ? 1 : N_CHANNELS;
	oc = out_packed ? 1 : N_CHANNELS;
	ns = (in_packed && out_packed) ? N_SAMPLES * N_CHANNELS : N_SAMPLES;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	t1 = SPA_TIMESPEC_TO_NSEC(&ts);

	for (i = 0; i < MAX_COUNT; i++) {
		func(NULL, oc, op, ic, ip, ns);
		count++;
	}
	count *= N_SAMPLES;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	t2 = SPA_TIMESPEC_TO_NSEC(&ts);

	fprintf(stderr, "%s: elapsed %"PRIu64" count %"PRIu64" = %"PRIu64"/sec\n", name,
			t2 - t1, count, count * (uint64_t)SPA_NSEC_PER_SEC / (t2 - t1));
}

static void test_f32_u8(void)
{
	run_test("test_f32_u8", true, true, conv_f32_to_u8);
	run_test("test_f32d_u8", false, true, conv_f32d_to_u8);
	run_test("test_f32_u8d", true, false, conv_f32_to_u8d);
}

static void test_u8_f32(void)
{
	run_test("test_u8_f32", true, true, conv_u8_to_f32);
	run_test("test_u8d_f32", false, true, conv_u8d_to_f32);
	run_test("test_u8_f32d", true, false, conv_u8_to_f32d);
}

static void test_f32_s16(void)
{
	run_test("test_f32_s16", true, true, conv_f32_to_s16);
	run_test("test_f32d_s16", false, true, conv_f32d_to_s16);
	run_test("test_f32_s16d", true, false, conv_f32_to_s16d);
}

static void test_s16_f32(void)
{
	run_test("test_s16_f32", true, true, conv_s16_to_f32);
	run_test("test_s16d_f32", false, true, conv_s16d_to_f32);
	run_test("test_s16_f32d", true, false, conv_s16_to_f32d);
}

static void test_f32_s32(void)
{
	run_test("test_f32_s32", true, true, conv_f32_to_s32);
	run_test("test_f32d_s32", false, true, conv_f32d_to_s32);
	run_test("test_f32_s32d", true, false, conv_f32_to_s32d);
}

static void test_s32_f32(void)
{
	run_test("test_s32_f32", true, true, conv_s32_to_f32);
	run_test("test_s32d_f32", false, true, conv_s32d_to_f32);
	run_test("test_s32_f32d", true, false, conv_s32_to_f32d);
}

static void test_f32_s24(void)
{
	run_test("test_f32_s24", true, true, conv_f32_to_s24);
	run_test("test_f32d_s24", false, true, conv_f32d_to_s24);
	run_test("test_f32_s24d", true, false, conv_f32_to_s24d);
}

static void test_s24_f32(void)
{
	run_test("test_s24_f32", true, true, conv_s24_to_f32);
	run_test("test_s24d_f32", false, true, conv_s24d_to_f32);
	run_test("test_s24_f32d", true, false, conv_s24_to_f32d);
}

static void test_f32_s24_32(void)
{
	run_test("test_f32_s24_32", true, true, conv_f32_to_s24_32);
	run_test("test_f32d_s24_32", false, true, conv_f32d_to_s24_32);
	run_test("test_f32_s24_32d", true, false, conv_f32_to_s24_32d);
}

static void test_s24_32_f32(void)
{
	run_test("test_s24_32_f32", true, true, conv_s24_32_to_f32);
	run_test("test_s24_32d_f32", false, true, conv_s24_32d_to_f32);
	run_test("test_s24_32_f32d", true, false, conv_s24_32_to_f32d);
}

static void test_interleave(void)
{
	run_test("test_interleave_8", false, true, interleave_8);
	run_test("test_interleave_16", false, true, interleave_16);
	run_test("test_interleave_24", false, true, interleave_24);
	run_test("test_interleave_32", false, true, interleave_32);
}

static void test_deinterleave(void)
{
	run_test("test_deinterleave_8", true, false, deinterleave_8);
	run_test("test_deinterleave_16", true, false, deinterleave_16);
	run_test("test_deinterleave_24", true, false, deinterleave_24);
	run_test("test_deinterleave_32", true, false, deinterleave_32);
}

int main(int argc, char *argv[])
{
	find_conv_info(0, 0, 0);

	test_f32_u8();
	test_u8_f32();
	test_f32_s16();
	test_s16_f32();
	test_f32_s32();
	test_s32_f32();
	test_f32_s24();
	test_s24_f32();
	test_f32_s24_32();
	test_s24_32_f32();
	test_interleave();
	test_deinterleave();

	return 0;
}
