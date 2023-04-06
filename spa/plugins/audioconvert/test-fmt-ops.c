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

#include <spa/debug/mem.h>

#include "test-helper.h"
#include "fmt-ops.c"

#define N_SAMPLES	253
#define N_CHANNELS	11

static uint32_t cpu_flags;

static uint8_t samp_in[N_SAMPLES * 8];
static uint8_t samp_out[N_SAMPLES * 8];
static uint8_t temp_in[N_SAMPLES * N_CHANNELS * 8];
static uint8_t temp_out[N_SAMPLES * N_CHANNELS * 8];

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

static void run_test(const char *name,
		const void *in, size_t in_size, const void *out, size_t out_size, size_t n_samples,
		bool in_packed, bool out_packed, convert_func_t func)
{
	const void *ip[N_CHANNELS];
	void *tp[N_CHANNELS];
	int i, j;
	const uint8_t *in8 = in, *out8 = out;
	struct convert conv;

	conv.n_channels = N_CHANNELS;

	for (j = 0; j < N_SAMPLES; j++) {
		memcpy(&samp_in[j * in_size], &in8[(j % n_samples) * in_size], in_size);
		memcpy(&samp_out[j * out_size], &out8[(j % n_samples) * out_size], out_size);
	}

	for (j = 0; j < N_CHANNELS; j++)
		ip[j] = samp_in;

	if (in_packed) {
		tp[0] = temp_in;
		switch(in_size) {
		case 1:
			conv_8d_to_8_c(&conv, tp, ip, N_SAMPLES);
			break;
		case 2:
			conv_16d_to_16_c(&conv, tp, ip, N_SAMPLES);
			break;
		case 3:
			conv_24d_to_24_c(&conv, tp, ip, N_SAMPLES);
			break;
		case 4:
			conv_32d_to_32_c(&conv, tp, ip, N_SAMPLES);
			break;
		case 8:
			conv_64d_to_64_c(&conv, tp, ip, N_SAMPLES);
			break;
		default:
			fprintf(stderr, "unknown size %zd\n", in_size);
			return;
		}
		ip[0] = temp_in;
	}

	spa_zero(temp_out);
	for (j = 0; j < N_CHANNELS; j++)
		tp[j] = &temp_out[j * N_SAMPLES * out_size];

	fprintf(stderr, "test %s:\n", name);
	func(&conv, tp, ip, N_SAMPLES);

	if (out_packed) {
		const uint8_t *d = tp[0], *s = samp_out;
		for (i = 0; i < N_SAMPLES; i++) {
			for (j = 0; j < N_CHANNELS; j++) {
				compare_mem(i, j, d, s, out_size);
				d += out_size;
			}
			s += out_size;
		}
	} else {
		for (j = 0; j < N_CHANNELS; j++) {
			compare_mem(0, j, tp[j], samp_out, N_SAMPLES * out_size);
		}
	}
}

static void test_f32_s8(void)
{
	static const float in[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.1f, -1.1f,
		1.0f/160.f, 1.0f/256.f, -1.0f/160.f, -1.0f/256.f };
	static const int8_t out[] = { 0, 127, -128, 64, 192, 127, -128, 1, 0, -1, 0 };

	run_test("test_f32_s8", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_f32_to_s8_c);
	run_test("test_f32d_s8", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_f32d_to_s8_c);
	run_test("test_f32_s8d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_f32_to_s8d_c);
	run_test("test_f32d_s8d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, false, conv_f32d_to_s8d_c);
}

static void test_s8_f32(void)
{
	static const int8_t in[] = { 0, 127, -128, 64, 192, };
	static const float out[] = { 0.0f, 0.9921875f, -1.0f, 0.5f, -0.5f, };

	run_test("test_s8_f32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_s8_to_f32_c);
	run_test("test_s8d_f32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_s8d_to_f32_c);
	run_test("test_s8_f32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_s8_to_f32d_c);
	run_test("test_s8d_f32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, false, conv_s8d_to_f32d_c);
}

static void test_f32_u8(void)
{
	static const float in[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.1f, -1.1f,
		1.0f/160.f, 1.0f/256.f, -1.0f/160.f, -1.0f/256.f };
	static const uint8_t out[] = { 128, 255, 0, 192, 64, 255, 0, 129, 128, 127, 128 };

	run_test("test_f32_u8", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_f32_to_u8_c);
	run_test("test_f32d_u8", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_f32d_to_u8_c);
	run_test("test_f32_u8d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_f32_to_u8d_c);
	run_test("test_f32d_u8d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, false, conv_f32d_to_u8d_c);
}

static void test_u8_f32(void)
{
	static const uint8_t in[] = { 128, 255, 0, 192, 64, };
	static const float out[] = { 0.0f, 0.9921875f, -1.0f, 0.5f, -0.5f, };

	run_test("test_u8_f32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_u8_to_f32_c);
	run_test("test_u8d_f32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_u8d_to_f32_c);
	run_test("test_u8_f32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_u8_to_f32d_c);
	run_test("test_u8d_f32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, false, conv_u8d_to_f32d_c);
}

static void test_f32_u16(void)
{
	static const float in[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.1f, -1.1f,
		1.0f/49152.f, 1.0f/65536.f, -1.0f/49152.f, -1.0f/65536.f };
	static const uint16_t out[] = { 32768, 65535, 0, 49152, 16384, 65535, 0,
		32769, 32768, 32767, 32768 };

	run_test("test_f32_u16", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_f32_to_u16_c);
	run_test("test_f32d_u16", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_f32d_to_u16_c);
}

static void test_u16_f32(void)
{
	static const uint16_t in[] = { 32768, 65535, 0, 49152, 16384, };
	static const float out[] = { 0.0f, 0.999969482422f, -1.0f, 0.5f, -0.5f };

	run_test("test_u16_f32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_u16_to_f32d_c);
	run_test("test_u16_f32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_u16_to_f32_c);
}

static void test_f32_s16(void)
{
	static const float in[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.1f, -1.1f,
		1.0f/49152.f, 1.0f/65536.f, -1.0f/49152.f, -1.0f/65536.f };
	static const int16_t out[] = { 0, 32767, -32768, 16384, -16384, 32767, -32768,
		1, 0, -1, 0 };

	run_test("test_f32_s16", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_f32_to_s16_c);
	run_test("test_f32d_s16", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_f32d_to_s16_c);
	run_test("test_f32_s16d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_f32_to_s16d_c);
	run_test("test_f32d_s16d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, false, conv_f32d_to_s16d_c);
#if defined(HAVE_SSE2)
	if (cpu_flags & SPA_CPU_FLAG_SSE2) {
		run_test("test_f32_s16_sse2", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_f32_to_s16_sse2);
		run_test("test_f32d_s16_sse2", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_f32d_to_s16_sse2);
		run_test("test_f32d_s16d_sse2", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, false, conv_f32d_to_s16d_sse2);
	}
#endif
#if defined(HAVE_AVX2)
	if (cpu_flags & SPA_CPU_FLAG_AVX2) {
		run_test("test_f32d_s16_avx2", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_f32d_to_s16_avx2);
	}
#endif
#if defined(HAVE_NEON)
	if (cpu_flags & SPA_CPU_FLAG_NEON) {
		run_test("test_f32d_s16_neon", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_f32d_to_s16_neon);
	}
#endif
}

static void test_s16_f32(void)
{
	static const int16_t in[] = { 0, 32767, -32768, 16384, -16384, };
	static const float out[] = { 0.0f, 0.999969482422f, -1.0f, 0.5f, -0.5f };

	run_test("test_s16_f32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_s16_to_f32d_c);
	run_test("test_s16d_f32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_s16d_to_f32_c);
	run_test("test_s16_f32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_s16_to_f32_c);
	run_test("test_s16d_f32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, false, conv_s16d_to_f32d_c);
#if defined(HAVE_SSE2)
	if (cpu_flags & SPA_CPU_FLAG_SSE2) {
		run_test("test_s16_f32d_sse2", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_s16_to_f32d_sse2);
	}
#endif
#if defined(HAVE_AVX2)
	if (cpu_flags & SPA_CPU_FLAG_AVX2) {
		run_test("test_s16_f32d_avx2", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_s16_to_f32d_avx2);
	}
#endif
#if defined(HAVE_NEON)
	if (cpu_flags & SPA_CPU_FLAG_NEON) {
		run_test("test_s16_f32d_neon", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_s16_to_f32d_neon);
	}
#endif
}

static void test_f32_u32(void)
{
	static const float in[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.1f, -1.1f,
		1.0f/0xa00000, 1.0f/0x1000000, -1.0f/0xa00000, -1.0f/0x1000000 };
	static const uint32_t out[] = { 0x80000000, 0xffffff00, 0x0, 0xc0000000, 0x40000000,
					0xffffff00, 0x0,
		0x80000100, 0x80000000, 0x7fffff00, 0x80000000 };

	run_test("test_f32_u32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_f32_to_u32_c);
	run_test("test_f32d_u32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_f32d_to_u32_c);
}

static void test_u32_f32(void)
{
	static const uint32_t in[] = { 0x80000000, 0xffffff00, 0x0, 0xc0000000, 0x40000000 };
	static const float out[] = { 0.0f, 0.999999880791f, -1.0f, 0.5f, -0.5f, };

	run_test("test_u32_f32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_u32_to_f32d_c);
	run_test("test_u32_f32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_u32_to_f32_c);
}

static void test_f32_s32(void)
{
	static const float in[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.1f, -1.1f,
		1.0f/0xa00000, 1.0f/0x1000000, -1.0f/0xa00000, -1.0f/0x1000000 };
	static const int32_t out[] = { 0, 0x7fffff00, 0x80000000, 0x40000000, 0xc0000000,
					0x7fffff00, 0x80000000,
		0x00000100, 0x00000000, 0xffffff00, 0x00000000 };

	run_test("test_f32_s32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_f32_to_s32_c);
	run_test("test_f32d_s32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_f32d_to_s32_c);
	run_test("test_f32_s32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_f32_to_s32d_c);
	run_test("test_f32d_s32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, false, conv_f32d_to_s32d_c);
#if defined(HAVE_SSE2)
	if (cpu_flags & SPA_CPU_FLAG_SSE2) {
		run_test("test_f32d_s32_sse2", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_f32d_to_s32_sse2);
	}
#endif
#if defined(HAVE_AVX2)
	if (cpu_flags & SPA_CPU_FLAG_AVX2) {
		run_test("test_f32d_s32_avx2", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_f32d_to_s32_avx2);
	}
#endif
}

static void test_s32_f32(void)
{
	static const int32_t in[] = { 0, 0x7fffff00, 0x80000000, 0x40000000, 0xc0000000 };
	static const float out[] = { 0.0f, 0.999999880791, -1.0f, 0.5, -0.5, };

	run_test("test_s32_f32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_s32_to_f32d_c);
	run_test("test_s32d_f32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_s32d_to_f32_c);
	run_test("test_s32_f32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_s32_to_f32_c);
	run_test("test_s32d_f32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, false, conv_s32d_to_f32d_c);
#if defined(HAVE_SSE2)
	if (cpu_flags & SPA_CPU_FLAG_SSE2) {
		run_test("test_s32_f32d_sse2", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_s32_to_f32d_sse2);
	}
#endif
#if defined(HAVE_AVX2)
	if (cpu_flags & SPA_CPU_FLAG_AVX2) {
		run_test("test_s32_f32d_avx2", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_s32_to_f32d_avx2);
	}
#endif
}

static void test_f32_u24(void)
{
	static const float in[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.1f, -1.1f,
		1.0f/0xa00000, 1.0f/0x1000000, -1.0f/0xa00000, -1.0f/0x1000000 };
	static const uint24_t out[] = { U32_TO_U24(0x00800000), U32_TO_U24(0xffffff),
		U32_TO_U24(0x000000), U32_TO_U24(0xc00000), U32_TO_U24(0x400000),
		U32_TO_U24(0xffffff), U32_TO_U24(0x000000),
		U32_TO_U24(0x800001), U32_TO_U24(0x800000), U32_TO_U24(0x7fffff),
		U32_TO_U24(0x800000) };

	run_test("test_f32_u24", in, sizeof(in[0]), out, 3, SPA_N_ELEMENTS(in),
			true, true, conv_f32_to_u24_c);
	run_test("test_f32d_u24", in, sizeof(in[0]), out, 3, SPA_N_ELEMENTS(in),
			false, true, conv_f32d_to_u24_c);
}

static void test_u24_f32(void)
{
	static const uint24_t in[] = { U32_TO_U24(0x00800000), U32_TO_U24(0xffffff),
		U32_TO_U24(0x000000), U32_TO_U24(0xc00000), U32_TO_U24(0x400000) };
	static const float out[] = { 0.0f, 0.999999880791f, -1.0f, 0.5, -0.5, };

	run_test("test_u24_f32d", in, 3, out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_u24_to_f32d_c);
	run_test("test_u24_f32", in, 3, out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_u24_to_f32_c);
}

static void test_f32_s24(void)
{
	static const float in[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.1f, -1.1f,
		1.0f/0xa00000, 1.0f/0x1000000, -1.0f/0xa00000, -1.0f/0x1000000 };
	static const int24_t out[] = { S32_TO_S24(0), S32_TO_S24(0x7fffff),
		S32_TO_S24(0xff800000), S32_TO_S24(0x400000), S32_TO_S24(0xc00000),
		S32_TO_S24(0x7fffff), S32_TO_S24(0xff800000),
		S32_TO_S24(0x000001), S32_TO_S24(0x000000), S32_TO_S24(0xffffffff),
		S32_TO_S24(0x000000) };

	run_test("test_f32_s24", in, sizeof(in[0]), out, 3, SPA_N_ELEMENTS(in),
			true, true, conv_f32_to_s24_c);
	run_test("test_f32d_s24", in, sizeof(in[0]), out, 3, SPA_N_ELEMENTS(in),
			false, true, conv_f32d_to_s24_c);
	run_test("test_f32_s24d", in, sizeof(in[0]), out, 3, SPA_N_ELEMENTS(in),
			true, false, conv_f32_to_s24d_c);
	run_test("test_f32d_s24d", in, sizeof(in[0]), out, 3, SPA_N_ELEMENTS(in),
			false, false, conv_f32d_to_s24d_c);
}

static void test_s24_f32(void)
{
	static const int24_t in[] = { S32_TO_S24(0), S32_TO_S24(0x7fffff),
		S32_TO_S24(0xff800000), S32_TO_S24(0x400000), S32_TO_S24(0xc00000) };
	static const float out[] = { 0.0f, 0.999999880791f, -1.0f, 0.5f, -0.5f, };

	run_test("test_s24_f32d", in, 3, out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_s24_to_f32d_c);
	run_test("test_s24d_f32", in, 3, out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_s24d_to_f32_c);
	run_test("test_s24_f32", in, 3, out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_s24_to_f32_c);
	run_test("test_s24d_f32d", in, 3, out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, false, conv_s24d_to_f32d_c);
#if defined(HAVE_SSE2)
	if (cpu_flags & SPA_CPU_FLAG_SSE2) {
		run_test("test_s24_f32d_sse2", in, 3, out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_s24_to_f32d_sse2);
	}
#endif
#if defined(HAVE_SSSE3)
	if (cpu_flags & SPA_CPU_FLAG_SSSE3) {
		run_test("test_s24_f32d_ssse3", in, 3, out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_s24_to_f32d_ssse3);
	}
#endif
#if defined(HAVE_SSE41)
	if (cpu_flags & SPA_CPU_FLAG_SSE41) {
		run_test("test_s24_f32d_sse41", in, 3, out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_s24_to_f32d_sse41);
	}
#endif
#if defined(HAVE_AVX2)
	if (cpu_flags & SPA_CPU_FLAG_AVX2) {
		run_test("test_s24_f32d_avx2", in, 3, out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_s24_to_f32d_avx2);
	}
#endif
}

static void test_f32_u24_32(void)
{
	static const float in[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.1f, -1.1f,
		1.0f/0xa00000, 1.0f/0x1000000, -1.0f/0xa00000, -1.0f/0x1000000 };
	static const uint32_t out[] = { 0x800000, 0xffffff, 0x0, 0xc00000, 0x400000,
					0xffffff, 0x000000,
		0x800001, 0x800000, 0x7fffff, 0x800000 };

	run_test("test_f32_u24_32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_f32_to_u24_32_c);
	run_test("test_f32d_u24_32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_f32d_to_u24_32_c);
}

static void test_u24_32_f32(void)
{
	static const uint32_t in[] = { 0x800000, 0xffffff, 0x0, 0xc00000, 0x400000, 0x11000000 };
	static const float out[] = { 0.0f, 0.999999880791f, -1.0f, 0.5f, -0.5f, -1.0f };

	run_test("test_u24_32_f32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_u24_32_to_f32d_c);
	run_test("test_u24_32_f32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_u24_32_to_f32_c);
}

static void test_f32_s24_32(void)
{
	static const float in[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.1f, -1.1f,
		1.0f/0xa00000, 1.0f/0x1000000, -1.0f/0xa00000, -1.0f/0x1000000 };
	static const int32_t out[] = { 0, 0x7fffff, 0xff800000, 0x400000, 0xffc00000,
					0x7fffff, 0xff800000,
		0x000001, 0x000000, 0xffffffff, 0x000000 };

	run_test("test_f32_s24_32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_f32_to_s24_32_c);
	run_test("test_f32d_s24_32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_f32d_to_s24_32_c);
	run_test("test_f32_s24_32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_f32_to_s24_32d_c);
	run_test("test_f32d_s24_32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, false, conv_f32d_to_s24_32d_c);
}

static void test_s24_32_f32(void)
{
	static const int32_t in[] = { 0, 0x7fffff, 0xff800000, 0x400000, 0xffc00000, 0x66800000 };
	static const float out[] = { 0.0f, 0.999999880791f, -1.0f, 0.5f, -0.5f, -1.0f };

	run_test("test_s24_32_f32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_s24_32_to_f32d_c);
	run_test("test_s24_32d_f32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_s24_32d_to_f32_c);
	run_test("test_s24_32_f32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_s24_32_to_f32_c);
	run_test("test_s24_32d_f32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, false, conv_s24_32d_to_f32d_c);
}

static void test_f64_f32(void)
{
	static const double in[] = { 0.0, 1.0, -1.0, 0.5, -0.5, };
	static const float out[] = { 0.0, 1.0, -1.0, 0.5, -0.5, };

	run_test("test_f64_f32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_f64_to_f32d_c);
	run_test("test_f64d_f32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_f64d_to_f32_c);
	run_test("test_f64_f32", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_f64_to_f32_c);
	run_test("test_f64d_f32d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, false, conv_f64d_to_f32d_c);
}

static void test_f32_f64(void)
{
	static const float in[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.1f, -1.1f };
	static const double out[] = { 0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 1.1f, -1.1f };

	run_test("test_f32_f64", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, true, conv_f32_to_f64_c);
	run_test("test_f32d_f64", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, true, conv_f32d_to_f64_c);
	run_test("test_f32_f64d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			true, false, conv_f32_to_f64d_c);
	run_test("test_f32d_f64d", in, sizeof(in[0]), out, sizeof(out[0]), SPA_N_ELEMENTS(out),
			false, false, conv_f32d_to_f64d_c);
}

static void test_lossless_s8(void)
{
	int8_t i;

	fprintf(stderr, "test %s:\n", __func__);
	for (i = S8_MIN; i < S8_MAX; i+=1) {
		float v = S8_TO_F32(i);
		int8_t t = F32_TO_S8(v);
		spa_assert_se(i == t);
	}
}

static void test_lossless_u8(void)
{
	uint8_t i;

	fprintf(stderr, "test %s:\n", __func__);
	for (i = U8_MIN; i < U8_MAX; i+=1) {
		float v = U8_TO_F32(i);
		uint8_t t = F32_TO_U8(v);
		spa_assert_se(i == t);
	}
}
static void test_lossless_s16(void)
{
	int32_t i;

	fprintf(stderr, "test %s:\n", __func__);
	for (i = S16_MIN; i <= S16_MAX; i+=3) {
		float v = S16_TO_F32((int16_t)i);
		int16_t t = F32_TO_S16(v);
		spa_assert_se(i == t);

		int32_t t2 = F32_TO_S32(v);
		spa_assert_se(i<<16 == t2);
		spa_assert_se(i == t2>>16);
	}
}

static void test_lossless_u16(void)
{
	uint32_t i;

	fprintf(stderr, "test %s:\n", __func__);
	for (i = U16_MIN; i <= U16_MAX; i+=3) {
		float v = U16_TO_F32((uint16_t)i);
		uint16_t t = F32_TO_U16(v);
		spa_assert_se(i == t);

		uint32_t t2 = F32_TO_U32(v);
		spa_assert_se(i<<16 == t2);
		spa_assert_se(i == t2>>16);
	}
}

static void test_lossless_s24(void)
{
	int32_t i;

	fprintf(stderr, "test %s:\n", __func__);
	for (i = S24_MIN; i < S24_MAX; i+=13) {
		float v = S24_TO_F32(s32_to_s24(i));
		int32_t t = s24_to_s32(F32_TO_S24(v));
		spa_assert_se(i == t);
	}
}

static void test_lossless_u24(void)
{
	uint32_t i;

	fprintf(stderr, "test %s:\n", __func__);
	for (i = U24_MIN; i < U24_MAX; i+=11) {
		float v = U24_TO_F32(u32_to_u24(i));
		uint32_t t = u24_to_u32(F32_TO_U24(v));
		spa_assert_se(i == t);
	}
}

static void test_lossless_s32(void)
{
	int32_t i;

	fprintf(stderr, "test %s:\n", __func__);
	for (i = S32_MIN; i < S32_MAX; i+=255) {
		float v = S32_TO_F32(i);
		int32_t t = F32_TO_S32(v);
		spa_assert_se(SPA_ABS(i - t) <= 256);
	}
}

static void test_lossless_u32(void)
{
	uint32_t i;

	fprintf(stderr, "test %s:\n", __func__);
	for (i = U32_MIN; i < U32_MAX; i+=255) {
		float v = U32_TO_F32(i);
		uint32_t t = F32_TO_U32(v);
		spa_assert_se(i > t ? (i - t) <= 256 : (t - i) <= 256);
	}
}

static void test_swaps(void)
{
	{
		uint24_t v = U32_TO_U24(0x123456);
		uint24_t t = U32_TO_U24(0x563412);
		uint24_t s = bswap_u24(v);
		spa_assert_se(memcmp(&s, &t, sizeof(t)) == 0);
	}
	{
		int24_t v = S32_TO_S24(0xfffe1dc0);
		int24_t t = S32_TO_S24(0xffc01dfe);
		int24_t s = bswap_s24(v);
		spa_assert_se(memcmp(&s, &t, sizeof(t)) == 0);
	}
	{
		int24_t v = S32_TO_S24(0x123456);
		int24_t t = S32_TO_S24(0x563412);
		int24_t s = bswap_s24(v);
		spa_assert_se(memcmp(&s, &t, sizeof(t)) == 0);
	}
}

static void run_test_noise(uint32_t fmt, uint32_t noise, uint32_t flags)
{
	struct convert conv;
	const void *ip[N_CHANNELS];
	void *op[N_CHANNELS];
	uint32_t i, range;
	bool all_zero;

	spa_zero(conv);

	conv.noise_bits = noise;
	conv.src_fmt = SPA_AUDIO_FORMAT_F32P;
	conv.dst_fmt = fmt;
	conv.n_channels = 2;
	conv.rate = 44100;
	conv.cpu_flags = flags;
	spa_assert_se(convert_init(&conv) == 0);
	fprintf(stderr, "test noise %s:\n", conv.func_name);

	memset(samp_in, 0, sizeof(samp_in));
	for (i = 0; i < conv.n_channels; i++) {
		ip[i] = samp_in;
		op[i] = samp_out;
	}
	convert_process(&conv, op, ip, N_SAMPLES);

	range = 1 << conv.noise_bits;

	all_zero = true;
	for (i = 0; i < conv.n_channels * N_SAMPLES; i++) {
		switch (fmt) {
		case SPA_AUDIO_FORMAT_S8:
		{
			int8_t *d = (int8_t *)samp_out;
			if (d[i] != 0)
				all_zero = false;
			spa_assert_se(SPA_ABS(d[i] - 0) <= (int8_t)range);
			break;
		}
		case SPA_AUDIO_FORMAT_U8:
		{
			uint8_t *d = (uint8_t *)samp_out;
			if (d[i] != 0x80)
				all_zero = false;
			spa_assert_se((int8_t)SPA_ABS(d[i] - 0x80) <= (int8_t)(range<<1));
			break;
		}
		case SPA_AUDIO_FORMAT_S16:
		{
			int16_t *d = (int16_t *)samp_out;
			if (d[i] != 0)
				all_zero = false;
			spa_assert_se(SPA_ABS(d[i] - 0) <= (int16_t)range);
			break;
		}
		case SPA_AUDIO_FORMAT_S24:
		{
			int24_t *d = (int24_t *)samp_out;
			int32_t t = s24_to_s32(d[i]);
			if (t != 0)
				all_zero = false;
			spa_assert_se(SPA_ABS(t - 0) <= (int32_t)range);
			break;
		}
		case SPA_AUDIO_FORMAT_S32:
		{
			int32_t *d = (int32_t *)samp_out;
			if (d[i] != 0)
				all_zero = false;
			spa_assert_se(SPA_ABS(d[i] - 0) <= (int32_t)(range << 8));
			break;
		}
		default:
			spa_assert_not_reached();
			break;
		}
	}
	spa_assert_se(all_zero == false);
	convert_free(&conv);
}

static void test_noise(void)
{
	run_test_noise(SPA_AUDIO_FORMAT_S8, 1, 0);
	run_test_noise(SPA_AUDIO_FORMAT_S8, 2, 0);
	run_test_noise(SPA_AUDIO_FORMAT_U8, 1, 0);
	run_test_noise(SPA_AUDIO_FORMAT_U8, 2, 0);
	run_test_noise(SPA_AUDIO_FORMAT_S16, 1, 0);
	run_test_noise(SPA_AUDIO_FORMAT_S16, 2, 0);
	run_test_noise(SPA_AUDIO_FORMAT_S24, 1, 0);
	run_test_noise(SPA_AUDIO_FORMAT_S24, 2, 0);
	run_test_noise(SPA_AUDIO_FORMAT_S32, 1, 0);
	run_test_noise(SPA_AUDIO_FORMAT_S32, 2, 0);
}

int main(int argc, char *argv[])
{
	cpu_flags = get_cpu_flags();
	printf("got CPU flags %d\n", cpu_flags);

	test_f32_s8();
	test_s8_f32();
	test_f32_u8();
	test_u8_f32();
	test_f32_u16();
	test_u16_f32();
	test_f32_s16();
	test_s16_f32();
	test_f32_u32();
	test_u32_f32();
	test_f32_s32();
	test_s32_f32();
	test_f32_u24();
	test_u24_f32();
	test_f32_s24();
	test_s24_f32();
	test_f32_u24_32();
	test_u24_32_f32();
	test_f32_s24_32();
	test_s24_32_f32();
	test_f32_f64();
	test_f64_f32();

	test_lossless_s8();
	test_lossless_u8();
	test_lossless_s16();
	test_lossless_u16();
	test_lossless_s24();
	test_lossless_u24();
	test_lossless_s32();
	test_lossless_u32();

	test_swaps();

	test_noise();

	return 0;
}
