/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <spa/debug/mem.h>

#include "test-helper.h"
#include "mix-ops.c"

static uint32_t cpu_flags;

#define N_SAMPLES 1024

static uint8_t samp_out[N_SAMPLES * 8];

static void compare_mem(int i, int j, const void *m1, const void *m2, size_t size)
{
	int res = memcmp(m1, m2, size);
	if (res != 0) {
		fprintf(stderr, "%d %d %zd:\n", i, j, size);
		spa_debug_mem(0, m1, size);
		spa_debug_mem(0, m2, size);
	}
	spa_assert_se(res == 0);
}

static int run_test(const char *name, const void *src[], uint32_t n_src, const void *dst,
		size_t dst_size, uint32_t n_samples, mix_func_t mix)
{
	struct mix_ops ops;

	ops.fmt = SPA_AUDIO_FORMAT_F32;
	ops.n_channels = 1;
	ops.cpu_flags = cpu_flags;
	mix_ops_init(&ops);

	fprintf(stderr, "%s\n", name);

	mix(&ops, (void *)samp_out, src, n_src, n_samples);
	compare_mem(0, 0, samp_out, dst, dst_size);
	return 0;
}

static void test_s8(void)
{
	int8_t out[] = { 0x00, 0x00, 0x00, 0x00 };
	int8_t in_1[] = { 0x00, 0x00, 0x00, 0x00 };
	int8_t in_2[] = { 0x7f, 0x80, 0x40, 0xc0 };
	int8_t in_3[] = { 0x40, 0xc0, 0xc0, 0x40 };
	int8_t in_4[] = { 0xc0, 0x40, 0x40, 0xc0 };
	int8_t out_4[] = { 0x7f, 0x80, 0x40, 0xc0 };
	const void *src[6] = { in_1, in_2, in_3, in_4 };

	run_test("test_s8_0", NULL, 0, out, sizeof(out), SPA_N_ELEMENTS(out), mix_s8_c);
	run_test("test_s8_1", src, 1, in_1, sizeof(in_1), SPA_N_ELEMENTS(in_1), mix_s8_c);
	run_test("test_s8_4", src, 4, out_4, sizeof(out_4), SPA_N_ELEMENTS(out_4), mix_s8_c);
}

static void test_u8(void)
{
	uint8_t out[] = { 0x80, 0x80, 0x80, 0x80 };
	uint8_t in_1[] = { 0x80, 0x80, 0x80, 0x80 };
	uint8_t in_2[] = { 0xff, 0x00, 0xc0, 0x40 };
	uint8_t in_3[] = { 0xc0, 0x40, 0x40, 0xc0 };
	uint8_t in_4[] = { 0x40, 0xc0, 0xc0, 0x40 };
	uint8_t out_4[] = { 0xff, 0x00, 0xc0, 0x40 };
	const void *src[6] = { in_1, in_2, in_3, in_4 };

	run_test("test_u8_0", NULL, 0, out, sizeof(out), SPA_N_ELEMENTS(out), mix_u8_c);
	run_test("test_u8_1", src, 1, in_1, sizeof(in_1), SPA_N_ELEMENTS(in_1), mix_u8_c);
	run_test("test_u8_4", src, 4, out_4, sizeof(out_4), SPA_N_ELEMENTS(out_4), mix_u8_c);
}

static void test_s16(void)
{
	int16_t out[] = { 0x0000, 0x0000, 0x0000, 0x0000 };
	int16_t in_1[] = { 0x0000, 0x0000, 0x0000, 0x0000 };
	int16_t in_2[] = { 0x7fff, 0x8000, 0x4000, 0xc000 };
	int16_t in_3[] = { 0x4000, 0xc000, 0xc000, 0x4000 };
	int16_t in_4[] = { 0xc000, 0x4000, 0x4000, 0xc000 };
	int16_t out_4[] = { 0x7fff, 0x8000, 0x4000, 0xc000 };
	const void *src[6] = { in_1, in_2, in_3, in_4 };

	run_test("test_s16_0", NULL, 0, out, sizeof(out), SPA_N_ELEMENTS(out), mix_s16_c);
	run_test("test_s16_1", src, 1, in_1, sizeof(in_1), SPA_N_ELEMENTS(in_1), mix_s16_c);
	run_test("test_s16_4", src, 4, out_4, sizeof(out_4), SPA_N_ELEMENTS(out_4), mix_s16_c);
}

static void test_u16(void)
{
	uint16_t out[] = { 0x8000, 0x8000, 0x8000, 0x8000 };
	uint16_t in_1[] = { 0x8000, 0x8000, 0x8000 , 0x8000};
	uint16_t in_2[] = { 0xffff, 0x0000, 0xc000, 0x4000 };
	uint16_t in_3[] = { 0xc000, 0x4000, 0x4000, 0xc000 };
	uint16_t in_4[] = { 0x4000, 0xc000, 0xc000, 0x4000 };
	uint16_t out_4[] = { 0xffff, 0x0000, 0xc000, 0x4000 };
	const void *src[6] = { in_1, in_2, in_3, in_4 };

	run_test("test_u16_0", NULL, 0, out, sizeof(out), SPA_N_ELEMENTS(out), mix_u16_c);
	run_test("test_u16_1", src, 1, in_1, sizeof(in_1), SPA_N_ELEMENTS(in_1), mix_u16_c);
	run_test("test_u16_4", src, 4, out_4, sizeof(out_4), SPA_N_ELEMENTS(out_4), mix_u16_c);
}

static void test_s24(void)
{
	int24_t out[] = { S32_TO_S24(0x000000), S32_TO_S24(0x000000), S32_TO_S24(0x000000) };
	int24_t in_1[] = { S32_TO_S24(0x000000), S32_TO_S24(0x000000), S32_TO_S24(0x000000) };
	int24_t in_2[] = { S32_TO_S24(0x7fffff), S32_TO_S24(0xff800000), S32_TO_S24(0x400000) };
	int24_t in_3[] = { S32_TO_S24(0x400000), S32_TO_S24(0xffc00000), S32_TO_S24(0xffc00000) };
	int24_t in_4[] = { S32_TO_S24(0xffc00000), S32_TO_S24(0x400000), S32_TO_S24(0x400000) };
	int24_t out_4[] = { S32_TO_S24(0x7fffff), S32_TO_S24(0xff800000), S32_TO_S24(0x400000) };
	const void *src[6] = { in_1, in_2, in_3, in_4 };

	run_test("test_s24_0", NULL, 0, out, sizeof(out), SPA_N_ELEMENTS(out), mix_s24_c);
	run_test("test_s24_1", src, 1, in_1, sizeof(in_1), SPA_N_ELEMENTS(in_1), mix_s24_c);
	run_test("test_s24_4", src, 4, out_4, sizeof(out_4), SPA_N_ELEMENTS(out_4), mix_s24_c);
}

static void test_u24(void)
{
	uint24_t out[] = { U32_TO_U24(0x800000), U32_TO_U24(0x800000), U32_TO_U24(0x800000) };
	uint24_t in_1[] = { U32_TO_U24(0x800000), U32_TO_U24(0x800000), U32_TO_U24(0x800000) };
	uint24_t in_2[] = { U32_TO_U24(0xffffffff), U32_TO_U24(0x000000), U32_TO_U24(0xffc00000) };
	uint24_t in_3[] = { U32_TO_U24(0xffc00000), U32_TO_U24(0x400000), U32_TO_U24(0x400000) };
	uint24_t in_4[] = { U32_TO_U24(0x400000), U32_TO_U24(0xffc00000), U32_TO_U24(0xffc00000) };
	uint24_t out_4[] = { U32_TO_U24(0xffffffff), U32_TO_U24(0x000000), U32_TO_U24(0xffc00000) };
	const void *src[6] = { in_1, in_2, in_3, in_4 };

	run_test("test_u24_0", NULL, 0, out, sizeof(out), SPA_N_ELEMENTS(out), mix_u24_c);
	run_test("test_u24_1", src, 1, in_1, sizeof(in_1), SPA_N_ELEMENTS(in_1), mix_u24_c);
	run_test("test_u24_4", src, 4, out_4, sizeof(out_4), SPA_N_ELEMENTS(out_4), mix_u24_c);
}

static void test_s32(void)
{
	int32_t out[] = { 0x00000000, 0x00000000, 0x00000000, 0x00000000 };
	int32_t in_1[] = { 0x00000000, 0x00000000, 0x00000000, 0x00000000 };
	int32_t in_2[] = { 0x7fffffff, 0x80000000, 0x40000000, 0xc0000000 };
	int32_t in_3[] = { 0x40000000, 0xc0000000, 0xc0000000, 0x40000000 };
	int32_t in_4[] = { 0xc0000000, 0x40000000, 0x40000000, 0xc0000000 };
	int32_t out_4[] = { 0x7fffffff, 0x80000000, 0x40000000, 0xc0000000 };
	const void *src[6] = { in_1, in_2, in_3, in_4 };

	run_test("test_s32_0", NULL, 0, out, sizeof(out), SPA_N_ELEMENTS(out), mix_s32_c);
	run_test("test_s32_1", src, 1, in_1, sizeof(in_1), SPA_N_ELEMENTS(in_1), mix_s32_c);
	run_test("test_s32_4", src, 4, out_4, sizeof(out_4), SPA_N_ELEMENTS(out_4), mix_s32_c);
}

static void test_u32(void)
{
	uint32_t out[] = { 0x80000000, 0x80000000, 0x80000000, 0x80000000 };
	uint32_t in_1[] = { 0x80000000, 0x80000000, 0x80000000, 0x80000000 };
	uint32_t in_2[] = { 0xffffffff, 0x00000000, 0xc0000000, 0x40000000 };
	uint32_t in_3[] = { 0xc0000000, 0x40000000, 0x40000000, 0xc0000000 };
	uint32_t in_4[] = { 0x40000000, 0xc0000000, 0xc0000000, 0x40000000 };
	uint32_t out_4[] = { 0xffffffff, 0x00000000, 0xc0000000, 0x40000000 };
	const void *src[6] = { in_1, in_2, in_3, in_4 };

	run_test("test_u32_0", NULL, 0, out, sizeof(out), SPA_N_ELEMENTS(out), mix_u32_c);
	run_test("test_u32_1", src, 1, in_1, sizeof(in_1), SPA_N_ELEMENTS(in_1), mix_u32_c);
	run_test("test_u32_4", src, 4, out_4, sizeof(out_4), SPA_N_ELEMENTS(out_4), mix_u32_c);
}

static void test_s24_32(void)
{
	int32_t out[] = { 0x000000, 0x000000, 0x000000, 0x000000 };
	int32_t in_1[] = { 0x000000, 0x000000, 0x000000, 0x000000 };
	int32_t in_2[] = { 0x7fffff, 0xff800000, 0x400000, 0xffc00000 };
	int32_t in_3[] = { 0x400000, 0xffc00000, 0xffc00000, 0x400000 };
	int32_t in_4[] = { 0xffc00000, 0x400000, 0x400000, 0xffc00000 };
	int32_t out_4[] = { 0x7fffff, 0xff800000, 0x400000, 0xffc00000 };
	const void *src[6] = { in_1, in_2, in_3, in_4 };

	run_test("test_s24_32_0", NULL, 0, out, sizeof(out), SPA_N_ELEMENTS(out), mix_s24_32_c);
	run_test("test_s24_32_1", src, 1, in_1, sizeof(in_1), SPA_N_ELEMENTS(in_1), mix_s24_32_c);
	run_test("test_s24_32_4", src, 4, out_4, sizeof(out_4), SPA_N_ELEMENTS(out_4), mix_s24_32_c);
}

static void test_u24_32(void)
{
	uint32_t out[] = { 0x800000, 0x800000, 0x800000, 0x800000 };
	uint32_t in_1[] = { 0x800000, 0x800000, 0x800000, 0x800000 };
	uint32_t in_2[] = { 0xffffff, 0x000000, 0xc00000, 0x400000 };
	uint32_t in_3[] = { 0xc00000, 0x400000, 0x400000, 0xc00000 };
	uint32_t in_4[] = { 0x400000, 0xc00000, 0xc00000, 0x400000 };
	uint32_t out_4[] = { 0xffffff, 0x000000, 0xc00000, 0x400000 };
	const void *src[6] = { in_1, in_2, in_3, in_4 };

	run_test("test_u24_32_0", NULL, 0, out, sizeof(out), SPA_N_ELEMENTS(out), mix_u24_32_c);
	run_test("test_u24_32_1", src, 1, in_1, sizeof(in_1), SPA_N_ELEMENTS(in_1), mix_u24_32_c);
	run_test("test_u24_32_4", src, 4, out_4, sizeof(out_4), SPA_N_ELEMENTS(out_4), mix_u24_32_c);
}

static void test_f32(void)
{
	float out[] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float in_1[] = { 0.0f, 0.0f, 0.0f, 0.0f };
	float in_2[] = { 1.0f, -1.0f, 0.5f, -0.5f };
	float in_3[] = { 0.5f, -0.5f, -0.5f, 0.5f };
	float in_4[] = { -0.5f, 1.0f, 0.5f, -0.5f };
	float out_4[] = { 1.0f, -0.5f, 0.5f, -0.5f };
	const void *src[6] = { in_1, in_2, in_3, in_4 };

	run_test("test_f32_0", NULL, 0, out, sizeof(out), SPA_N_ELEMENTS(out), mix_f32_c);
	run_test("test_f32_1", src, 1, in_1, sizeof(in_1), SPA_N_ELEMENTS(in_1), mix_f32_c);
	run_test("test_f32_4", src, 4, out_4, sizeof(out_4), SPA_N_ELEMENTS(out_4), mix_f32_c);
#if defined(HAVE_SSE)
	if (cpu_flags & SPA_CPU_FLAG_SSE) {
		run_test("test_f32_0_sse", NULL, 0, out, sizeof(out), SPA_N_ELEMENTS(out), mix_f32_sse);
		run_test("test_f32_1_sse", src, 1, in_1, sizeof(in_1), SPA_N_ELEMENTS(in_1), mix_f32_sse);
		run_test("test_f32_4_sse", src, 4, out_4, sizeof(out_4), SPA_N_ELEMENTS(out_4), mix_f32_sse);
	}
#endif
#if defined(HAVE_AVX)
	if (cpu_flags & SPA_CPU_FLAG_AVX) {
		run_test("test_f32_0_avx", NULL, 0, out, sizeof(out), SPA_N_ELEMENTS(out), mix_f32_avx);
		run_test("test_f32_1_avx", src, 1, in_1, sizeof(in_1), SPA_N_ELEMENTS(in_1), mix_f32_avx);
		run_test("test_f32_4_avx", src, 4, out_4, sizeof(out_4), SPA_N_ELEMENTS(out_4), mix_f32_avx);
	}
#endif
}

static void test_f64(void)
{
	double out[] = { 0.0, 0.0, 0.0, 0.0 };
	double in_1[] = { 0.0, 0.0, 0.0, 0.0 };
	double in_2[] = { 1.0, -1.0, 0.5, -0.5 };
	double in_3[] = { 0.5, -0.5, -0.5, 0.5 };
	double in_4[] = { -0.5, 1.0, 0.5, -0.5 };
	double out_4[] = { 1.0, -0.5, 0.5, -0.5 };
	const void *src[6] = { in_1, in_2, in_3, in_4 };

	run_test("test_f64_0", NULL, 0, out, sizeof(out), SPA_N_ELEMENTS(out), mix_f64_c);
	run_test("test_f64_1", src, 1, in_1, sizeof(in_1), SPA_N_ELEMENTS(in_1), mix_f64_c);
	run_test("test_f64_4", src, 4, out_4, sizeof(out_4), SPA_N_ELEMENTS(out_4), mix_f64_c);
#if defined(HAVE_SSE2)
	if (cpu_flags & SPA_CPU_FLAG_SSE2) {
		run_test("test_f64_0_sse2", NULL, 0, out, sizeof(out), SPA_N_ELEMENTS(out), mix_f64_sse2);
		run_test("test_f64_1_sse2", src, 1, in_1, sizeof(in_1), SPA_N_ELEMENTS(in_1), mix_f64_sse2);
		run_test("test_f64_4_sse2", src, 4, out_4, sizeof(out_4), SPA_N_ELEMENTS(out_4), mix_f64_sse2);
	}
#endif
}

int main(int argc, char *argv[])
{
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

	return 0;
}
