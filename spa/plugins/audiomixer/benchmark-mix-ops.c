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

#include "test-helper.h"
#include "mix-ops.h"

static uint32_t cpu_flags;

typedef void (*mix_func_t) (struct mix_ops *ops, void * SPA_RESTRICT dst,
		const void * SPA_RESTRICT src[], uint32_t n_src, uint32_t n_samples);
struct stats {
	uint32_t n_samples;
	uint32_t n_src;
	uint64_t perf;
	const char *name;
	const char *impl;
};

#define MAX_SAMPLES	4096
#define MAX_SRC		11

#define MAX_COUNT 100

static uint8_t samp_in[MAX_SAMPLES * MAX_SRC * 8];
static uint8_t samp_out[MAX_SAMPLES * 8];

static const int sample_sizes[] = { 0, 1, 128, 513, 4096 };
static const int src_counts[] = { 1, 2, 4, 6, 8, 11 };

#define MAX_RESULTS	SPA_N_ELEMENTS(sample_sizes) * SPA_N_ELEMENTS(src_counts) * 70

static uint32_t n_results = 0;
static struct stats results[MAX_RESULTS];

static void run_test1(const char *name, const char *impl, mix_func_t func, int n_src, int n_samples)
{
	int i, j;
	const void *ip[n_src];
	void *op;
	struct timespec ts;
	uint64_t count, t1, t2;
	struct mix_ops mix;

	mix.n_channels = 1;

	for (j = 0; j < n_src; j++)
		ip[j] = SPA_PTR_ALIGN(&samp_in[j * n_samples * 4], 32, void);
	op = SPA_PTR_ALIGN(samp_out, 32, void);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	t1 = SPA_TIMESPEC_TO_NSEC(&ts);

	count = 0;
	for (i = 0; i < MAX_COUNT; i++) {
		func(&mix, op, ip, n_src, n_samples);
		count++;
	}
	clock_gettime(CLOCK_MONOTONIC, &ts);
	t2 = SPA_TIMESPEC_TO_NSEC(&ts);

	spa_assert(n_results < MAX_RESULTS);

	results[n_results++] = (struct stats) {
		.n_samples = n_samples,
		.n_src = n_src,
		.perf = count * (uint64_t)SPA_NSEC_PER_SEC / (t2 - t1),
		.name = name,
		.impl = impl
	};
}

static void run_test(const char *name, const char *impl, mix_func_t func)
{
	size_t i, j;

	for (i = 0; i < SPA_N_ELEMENTS(sample_sizes); i++) {
		for (j = 0; j < SPA_N_ELEMENTS(src_counts); j++) {
			run_test1(name, impl, func, src_counts[j],
				(sample_sizes[i] + (src_counts[j] -1)) / src_counts[j]);
		}
	}
}

static void test_s8(void)
{
	run_test("test_s8", "c", mix_s8_c);
}
static void test_u8(void)
{
	run_test("test_u8", "c", mix_u8_c);
}

static void test_s16(void)
{
	run_test("test_s16", "c", mix_s16_c);
}
static void test_u16(void)
{
	run_test("test_u8", "c", mix_u16_c);
}

static void test_s24(void)
{
	run_test("test_s24", "c", mix_s24_c);
}
static void test_u24(void)
{
	run_test("test_u24", "c", mix_u24_c);
}
static void test_s24_32(void)
{
	run_test("test_s24_32", "c", mix_s24_32_c);
}
static void test_u24_32(void)
{
	run_test("test_u24_32", "c", mix_u24_32_c);
}

static void test_s32(void)
{
	run_test("test_s32", "c", mix_s32_c);
}
static void test_u32(void)
{
	run_test("test_u32", "c", mix_u32_c);
}

static void test_f32(void)
{
	run_test("test_f32", "c", mix_f32_c);
#if defined (HAVE_SSE)
	if (cpu_flags & SPA_CPU_FLAG_SSE) {
		run_test("test_f32", "sse", mix_f32_sse);
	}
#endif
#if defined (HAVE_AVX)
	if (cpu_flags & SPA_CPU_FLAG_AVX) {
		run_test("test_f32", "avx", mix_f32_avx);
	}
#endif
}

static void test_f64(void)
{
	run_test("test_f64", "c", mix_f64_c);
#if defined (HAVE_SSE2)
	if (cpu_flags & SPA_CPU_FLAG_SSE2) {
		run_test("test_f64", "sse2", mix_f64_sse2);
	}
#endif
}

static int compare_func(const void *_a, const void *_b)
{
	const struct stats *a = _a, *b = _b;
	int diff;
	if ((diff = strcmp(a->name, b->name)) != 0) return diff;
	if ((diff = a->n_samples - b->n_samples) != 0) return diff;
	if ((diff = a->n_src - b->n_src) != 0) return diff;
	if ((diff = b->perf - a->perf) != 0) return diff;
	return 0;
}

int main(int argc, char *argv[])
{
	uint32_t i;

	cpu_flags = get_cpu_flags();
	printf("got get CPU flags %d\n", cpu_flags);

	test_s8();
	test_u8();
	test_s16();
	test_u16();
	test_s24();
	test_u24();
	test_s32();
	test_u32();
	test_s24_32();
	test_u24_32();
	test_f32();
	test_f64();

	qsort(results, n_results, sizeof(struct stats), compare_func);

	for (i = 0; i < n_results; i++) {
		struct stats *s = &results[i];
		fprintf(stderr, "%-12."PRIu64" \t%-32.32s %s \t samples %d, src %d\n",
				s->perf, s->name, s->impl, s->n_samples, s->n_src);
	}
	return 0;
}
