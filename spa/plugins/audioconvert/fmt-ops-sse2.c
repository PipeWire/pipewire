/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "fmt-ops.h"

#include <emmintrin.h>

#define _MM_CLAMP_PS(r,min,max)				\
	_mm_min_ps(_mm_max_ps(r, min), max)

#define _MM_CLAMP_SS(r,min,max)				\
	_mm_min_ss(_mm_max_ss(r, min), max)

static void
conv_s16_to_f32d_1s_sse2(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	const int16_t *s = src;
	float *d0 = dst[0];
	uint32_t n, unrolled;
	__m128i in = _mm_setzero_si128();
	__m128 out, factor = _mm_set1_ps(1.0f / S16_SCALE);

	if (SPA_LIKELY(SPA_IS_ALIGNED(d0, 16)))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		in = _mm_insert_epi16(in, s[0*n_channels], 1);
		in = _mm_insert_epi16(in, s[1*n_channels], 3);
		in = _mm_insert_epi16(in, s[2*n_channels], 5);
		in = _mm_insert_epi16(in, s[3*n_channels], 7);
		in = _mm_srai_epi32(in, 16);
		out = _mm_cvtepi32_ps(in);
		out = _mm_mul_ps(out, factor);
		_mm_store_ps(&d0[n], out);
		s += 4*n_channels;
	}
	for(; n < n_samples; n++) {
		out = _mm_cvtsi32_ss(factor, s[0]);
		out = _mm_mul_ss(out, factor);
		_mm_store_ss(&d0[n], out);
		s += n_channels;
	}
}

void
conv_s16_to_f32d_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int16_t *s = src[0];
	uint32_t i = 0, n_channels = conv->n_channels;

	for(; i < n_channels; i++)
		conv_s16_to_f32d_1s_sse2(conv, &dst[i], &s[i], n_channels, n_samples);
}

void
conv_s16_to_f32d_2_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int16_t *s = src[0];
	float *d0 = dst[0], *d1 = dst[1];
	uint32_t n, unrolled;
	__m128i in[2], t[4];
	__m128 out[4], factor = _mm_set1_ps(1.0f / S16_SCALE);

	if (SPA_IS_ALIGNED(s, 16) &&
	    SPA_IS_ALIGNED(d0, 16) &&
	    SPA_IS_ALIGNED(d1, 16))
		unrolled = n_samples & ~7;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 8) {
		in[0] = _mm_load_si128((__m128i*)(s + 0));
		in[1] = _mm_load_si128((__m128i*)(s + 8));

		t[0] = _mm_slli_epi32(in[0], 16);
		t[0] = _mm_srai_epi32(t[0], 16);
		out[0] = _mm_cvtepi32_ps(t[0]);
		out[0] = _mm_mul_ps(out[0], factor);

		t[1] = _mm_srai_epi32(in[0], 16);
		out[1] = _mm_cvtepi32_ps(t[1]);
		out[1] = _mm_mul_ps(out[1], factor);

		t[2] = _mm_slli_epi32(in[1], 16);
		t[2] = _mm_srai_epi32(t[2], 16);
		out[2] = _mm_cvtepi32_ps(t[2]);
		out[2] = _mm_mul_ps(out[2], factor);

		t[3] = _mm_srai_epi32(in[1], 16);
		out[3] = _mm_cvtepi32_ps(t[3]);
		out[3] = _mm_mul_ps(out[3], factor);

		_mm_store_ps(&d0[n + 0], out[0]);
		_mm_store_ps(&d1[n + 0], out[1]);
		_mm_store_ps(&d0[n + 4], out[2]);
		_mm_store_ps(&d1[n + 4], out[3]);

		s += 16;
	}
	for(; n < n_samples; n++) {
		out[0] = _mm_cvtsi32_ss(factor, s[0]);
		out[0] = _mm_mul_ss(out[0], factor);
		out[1] = _mm_cvtsi32_ss(factor, s[1]);
		out[1] = _mm_mul_ss(out[1], factor);
		_mm_store_ss(&d0[n], out[0]);
		_mm_store_ss(&d1[n], out[1]);
		s += 2;
	}
}

#define spa_read_unaligned(ptr, type) \
__extension__ ({ \
	__typeof__(type) _val; \
	memcpy(&_val, (ptr), sizeof(_val)); \
	_val; \
})

#define spa_write_unaligned(ptr, type, val) \
__extension__ ({ \
	__typeof__(type) _val = (val); \
	memcpy((ptr), &_val, sizeof(_val)); \
})
void
conv_s24_to_f32d_1s_sse2(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	const int24_t *s = src;
	float *d0 = dst[0];
	uint32_t n, unrolled;
	__m128i in;
	__m128 out, factor = _mm_set1_ps(1.0f / S24_SCALE);

	if (SPA_IS_ALIGNED(d0, 16) && n_samples > 0) {
		unrolled = n_samples & ~3;
		if ((n_samples & 3) == 0)
			unrolled -= 4;
	}
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		in = _mm_setr_epi32(
			spa_read_unaligned(&s[0 * n_channels], uint32_t),
			spa_read_unaligned(&s[1 * n_channels], uint32_t),
			spa_read_unaligned(&s[2 * n_channels], uint32_t),
			spa_read_unaligned(&s[3 * n_channels], uint32_t));
		in = _mm_slli_epi32(in, 8);
		in = _mm_srai_epi32(in, 8);
		out = _mm_cvtepi32_ps(in);
		out = _mm_mul_ps(out, factor);
		_mm_store_ps(&d0[n], out);
		s += 4 * n_channels;
	}
	for(; n < n_samples; n++) {
		out = _mm_cvtsi32_ss(factor, s24_to_s32(*s));
		out = _mm_mul_ss(out, factor);
		_mm_store_ss(&d0[n], out);
		s += n_channels;
	}
}

static void
conv_s24_to_f32d_2s_sse2(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	const int24_t *s = src;
	float *d0 = dst[0], *d1 = dst[1];
	uint32_t n, unrolled;
	__m128i in[2];
	__m128 out[2], factor = _mm_set1_ps(1.0f / S24_SCALE);

	if (SPA_IS_ALIGNED(d0, 16) &&
	    SPA_IS_ALIGNED(d1, 16) &&
	    n_samples > 0) {
		unrolled = n_samples & ~3;
		if ((n_samples & 3) == 0)
			unrolled -= 4;
	}
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		in[0] = _mm_setr_epi32(
			spa_read_unaligned(&s[0 + 0*n_channels], uint32_t),
			spa_read_unaligned(&s[0 + 1*n_channels], uint32_t),
			spa_read_unaligned(&s[0 + 2*n_channels], uint32_t),
			spa_read_unaligned(&s[0 + 3*n_channels], uint32_t));
		in[1] = _mm_setr_epi32(
			spa_read_unaligned(&s[1 + 0*n_channels], uint32_t),
			spa_read_unaligned(&s[1 + 1*n_channels], uint32_t),
			spa_read_unaligned(&s[1 + 2*n_channels], uint32_t),
			spa_read_unaligned(&s[1 + 3*n_channels], uint32_t));

		in[0] = _mm_slli_epi32(in[0], 8);
		in[1] = _mm_slli_epi32(in[1], 8);

		in[0] = _mm_srai_epi32(in[0], 8);
		in[1] = _mm_srai_epi32(in[1], 8);

		out[0] = _mm_cvtepi32_ps(in[0]);
		out[1] = _mm_cvtepi32_ps(in[1]);

		out[0] = _mm_mul_ps(out[0], factor);
		out[1] = _mm_mul_ps(out[1], factor);

		_mm_store_ps(&d0[n], out[0]);
		_mm_store_ps(&d1[n], out[1]);

		s += 4 * n_channels;
	}
	for(; n < n_samples; n++) {
		out[0] = _mm_cvtsi32_ss(factor, s24_to_s32(*s));
		out[1] = _mm_cvtsi32_ss(factor, s24_to_s32(*(s+1)));
		out[0] = _mm_mul_ss(out[0], factor);
		out[1] = _mm_mul_ss(out[1], factor);
		_mm_store_ss(&d0[n], out[0]);
		_mm_store_ss(&d1[n], out[1]);
		s += n_channels;
	}
}
static void
conv_s24_to_f32d_4s_sse2(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	const int24_t *s = src;
	float *d0 = dst[0], *d1 = dst[1], *d2 = dst[2], *d3 = dst[3];
	uint32_t n, unrolled;
	__m128i in[4];
	__m128 out[4], factor = _mm_set1_ps(1.0f / S24_SCALE);

	if (SPA_IS_ALIGNED(d0, 16) &&
	    SPA_IS_ALIGNED(d1, 16) &&
	    SPA_IS_ALIGNED(d2, 16) &&
	    SPA_IS_ALIGNED(d3, 16) &&
	    n_samples > 0) {
		unrolled = n_samples & ~3;
		if ((n_samples & 3) == 0)
			unrolled -= 4;
	}
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		in[0] = _mm_setr_epi32(
			spa_read_unaligned(&s[0 + 0*n_channels], uint32_t),
			spa_read_unaligned(&s[0 + 1*n_channels], uint32_t),
			spa_read_unaligned(&s[0 + 2*n_channels], uint32_t),
			spa_read_unaligned(&s[0 + 3*n_channels], uint32_t));
		in[1] = _mm_setr_epi32(
			spa_read_unaligned(&s[1 + 0*n_channels], uint32_t),
			spa_read_unaligned(&s[1 + 1*n_channels], uint32_t),
			spa_read_unaligned(&s[1 + 2*n_channels], uint32_t),
			spa_read_unaligned(&s[1 + 3*n_channels], uint32_t));
		in[2] = _mm_setr_epi32(
			spa_read_unaligned(&s[2 + 0*n_channels], uint32_t),
			spa_read_unaligned(&s[2 + 1*n_channels], uint32_t),
			spa_read_unaligned(&s[2 + 2*n_channels], uint32_t),
			spa_read_unaligned(&s[2 + 3*n_channels], uint32_t));
		in[3] = _mm_setr_epi32(
			spa_read_unaligned(&s[3 + 0*n_channels], uint32_t),
			spa_read_unaligned(&s[3 + 1*n_channels], uint32_t),
			spa_read_unaligned(&s[3 + 2*n_channels], uint32_t),
			spa_read_unaligned(&s[3 + 3*n_channels], uint32_t));

		in[0] = _mm_slli_epi32(in[0], 8);
		in[1] = _mm_slli_epi32(in[1], 8);
		in[2] = _mm_slli_epi32(in[2], 8);
		in[3] = _mm_slli_epi32(in[3], 8);

		in[0] = _mm_srai_epi32(in[0], 8);
		in[1] = _mm_srai_epi32(in[1], 8);
		in[2] = _mm_srai_epi32(in[2], 8);
		in[3] = _mm_srai_epi32(in[3], 8);

		out[0] = _mm_cvtepi32_ps(in[0]);
		out[1] = _mm_cvtepi32_ps(in[1]);
		out[2] = _mm_cvtepi32_ps(in[2]);
		out[3] = _mm_cvtepi32_ps(in[3]);

		out[0] = _mm_mul_ps(out[0], factor);
		out[1] = _mm_mul_ps(out[1], factor);
		out[2] = _mm_mul_ps(out[2], factor);
		out[3] = _mm_mul_ps(out[3], factor);

		_mm_store_ps(&d0[n], out[0]);
		_mm_store_ps(&d1[n], out[1]);
		_mm_store_ps(&d2[n], out[2]);
		_mm_store_ps(&d3[n], out[3]);

		s += 4 * n_channels;
	}
	for(; n < n_samples; n++) {
		out[0] = _mm_cvtsi32_ss(factor, s24_to_s32(*s));
		out[1] = _mm_cvtsi32_ss(factor, s24_to_s32(*(s+1)));
		out[2] = _mm_cvtsi32_ss(factor, s24_to_s32(*(s+2)));
		out[3] = _mm_cvtsi32_ss(factor, s24_to_s32(*(s+3)));
		out[0] = _mm_mul_ss(out[0], factor);
		out[1] = _mm_mul_ss(out[1], factor);
		out[2] = _mm_mul_ss(out[2], factor);
		out[3] = _mm_mul_ss(out[3], factor);
		_mm_store_ss(&d0[n], out[0]);
		_mm_store_ss(&d1[n], out[1]);
		_mm_store_ss(&d2[n], out[2]);
		_mm_store_ss(&d3[n], out[3]);
		s += n_channels;
	}
}

void
conv_s24_to_f32d_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int8_t *s = src[0];
	uint32_t i = 0, n_channels = conv->n_channels;

	for(; i + 3 < n_channels; i += 4)
		conv_s24_to_f32d_4s_sse2(conv, &dst[i], &s[3*i], n_channels, n_samples);
	for(; i + 1 < n_channels; i += 2)
		conv_s24_to_f32d_2s_sse2(conv, &dst[i], &s[3*i], n_channels, n_samples);
	for(; i < n_channels; i++)
		conv_s24_to_f32d_1s_sse2(conv, &dst[i], &s[3*i], n_channels, n_samples);
}

static void
conv_s32_to_f32d_1s_sse2(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	const int32_t *s = src;
	float *d0 = dst[0];
	uint32_t n, unrolled;
	__m128i in;
	__m128 out, factor = _mm_set1_ps(1.0f / S24_SCALE);

	if (SPA_IS_ALIGNED(d0, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		in = _mm_setr_epi32(s[0*n_channels],
				    s[1*n_channels],
				    s[2*n_channels],
				    s[3*n_channels]);
		in = _mm_srai_epi32(in, 8);
		out = _mm_cvtepi32_ps(in);
		out = _mm_mul_ps(out, factor);
		_mm_store_ps(&d0[n], out);
		s += 4*n_channels;
	}
	for(; n < n_samples; n++) {
		out = _mm_cvtsi32_ss(factor, s[0]>>8);
		out = _mm_mul_ss(out, factor);
		_mm_store_ss(&d0[n], out);
		s += n_channels;
	}
}

void
conv_s32_to_f32d_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int32_t *s = src[0];
	uint32_t i = 0, n_channels = conv->n_channels;

	for(; i < n_channels; i++)
		conv_s32_to_f32d_1s_sse2(conv, &dst[i], &s[i], n_channels, n_samples);
}

static void
conv_f32d_to_s32_1s_sse2(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s0 = src[0];
	int32_t *d = dst;
	uint32_t n, unrolled;
	__m128 in[1];
	__m128i out[4];
	__m128 scale = _mm_set1_ps(S24_SCALE);
	__m128 int_min = _mm_set1_ps(S24_MIN);
	__m128 int_max = _mm_set1_ps(S24_MAX);

	if (SPA_IS_ALIGNED(s0, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		in[0] = _mm_mul_ps(_mm_load_ps(&s0[n]), scale);
		in[0] = _MM_CLAMP_PS(in[0], int_min, int_max);
		out[0] = _mm_cvtps_epi32(in[0]);
		out[0] = _mm_slli_epi32(out[0], 8);
		out[1] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(0, 3, 2, 1));
		out[2] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(1, 0, 3, 2));
		out[3] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(2, 1, 0, 3));

		d[0*n_channels] = _mm_cvtsi128_si32(out[0]);
		d[1*n_channels] = _mm_cvtsi128_si32(out[1]);
		d[2*n_channels] = _mm_cvtsi128_si32(out[2]);
		d[3*n_channels] = _mm_cvtsi128_si32(out[3]);
		d += 4*n_channels;
	}
	for(; n < n_samples; n++) {
		in[0] = _mm_load_ss(&s0[n]);
		in[0] = _mm_mul_ss(in[0], scale);
		in[0] = _MM_CLAMP_SS(in[0], int_min, int_max);
		*d = _mm_cvtss_si32(in[0]) << 8;
		d += n_channels;
	}
}

static void
conv_f32d_to_s32_2s_sse2(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s0 = src[0], *s1 = src[1];
	int32_t *d = dst;
	uint32_t n, unrolled;
	__m128 in[2];
	__m128i out[2], t[2];
	__m128 scale = _mm_set1_ps(S24_SCALE);
	__m128 int_min = _mm_set1_ps(S24_MIN);
	__m128 int_max = _mm_set1_ps(S24_MAX);

	if (SPA_IS_ALIGNED(s0, 16) &&
	    SPA_IS_ALIGNED(s1, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		in[0] = _mm_mul_ps(_mm_load_ps(&s0[n]), scale);
		in[1] = _mm_mul_ps(_mm_load_ps(&s1[n]), scale);

		in[0] = _MM_CLAMP_PS(in[0], int_min, int_max);
		in[1] = _MM_CLAMP_PS(in[1], int_min, int_max);

		out[0] = _mm_cvtps_epi32(in[0]);
		out[1] = _mm_cvtps_epi32(in[1]);
		out[0] = _mm_slli_epi32(out[0], 8);
		out[1] = _mm_slli_epi32(out[1], 8);

		t[0] = _mm_unpacklo_epi32(out[0], out[1]);
		t[1] = _mm_unpackhi_epi32(out[0], out[1]);

		_mm_storel_pi((__m64*)(d + 0*n_channels), (__m128)t[0]);
		_mm_storeh_pi((__m64*)(d + 1*n_channels), (__m128)t[0]);
		_mm_storel_pi((__m64*)(d + 2*n_channels), (__m128)t[1]);
		_mm_storeh_pi((__m64*)(d + 3*n_channels), (__m128)t[1]);
		d += 4*n_channels;
	}
	for(; n < n_samples; n++) {
		in[0] = _mm_load_ss(&s0[n]);
		in[1] = _mm_load_ss(&s1[n]);

		in[0] = _mm_unpacklo_ps(in[0], in[1]);

		in[0] = _mm_mul_ps(in[0], scale);
		in[0] = _MM_CLAMP_PS(in[0], int_min, int_max);
		out[0] = _mm_cvtps_epi32(in[0]);
		out[0] = _mm_slli_epi32(out[0], 8);
		_mm_storel_epi64((__m128i*)d, out[0]);
		d += n_channels;
	}
}

static void
conv_f32d_to_s32_4s_sse2(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s0 = src[0], *s1 = src[1], *s2 = src[2], *s3 = src[3];
	int32_t *d = dst;
	uint32_t n, unrolled;
	__m128 in[4];
	__m128i out[4];
	__m128 scale = _mm_set1_ps(S24_SCALE);
	__m128 int_min = _mm_set1_ps(S24_MIN);
	__m128 int_max = _mm_set1_ps(S24_MAX);

	if (SPA_IS_ALIGNED(s0, 16) &&
	    SPA_IS_ALIGNED(s1, 16) &&
	    SPA_IS_ALIGNED(s2, 16) &&
	    SPA_IS_ALIGNED(s3, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		in[0] = _mm_mul_ps(_mm_load_ps(&s0[n]), scale);
		in[1] = _mm_mul_ps(_mm_load_ps(&s1[n]), scale);
		in[2] = _mm_mul_ps(_mm_load_ps(&s2[n]), scale);
		in[3] = _mm_mul_ps(_mm_load_ps(&s3[n]), scale);

		in[0] = _MM_CLAMP_PS(in[0], int_min, int_max);
		in[1] = _MM_CLAMP_PS(in[1], int_min, int_max);
		in[2] = _MM_CLAMP_PS(in[2], int_min, int_max);
		in[3] = _MM_CLAMP_PS(in[3], int_min, int_max);

		_MM_TRANSPOSE4_PS(in[0], in[1], in[2], in[3]);

		out[0] = _mm_cvtps_epi32(in[0]);
		out[1] = _mm_cvtps_epi32(in[1]);
		out[2] = _mm_cvtps_epi32(in[2]);
		out[3] = _mm_cvtps_epi32(in[3]);
		out[0] = _mm_slli_epi32(out[0], 8);
		out[1] = _mm_slli_epi32(out[1], 8);
		out[2] = _mm_slli_epi32(out[2], 8);
		out[3] = _mm_slli_epi32(out[3], 8);

		_mm_storeu_si128((__m128i*)(d + 0*n_channels), out[0]);
		_mm_storeu_si128((__m128i*)(d + 1*n_channels), out[1]);
		_mm_storeu_si128((__m128i*)(d + 2*n_channels), out[2]);
		_mm_storeu_si128((__m128i*)(d + 3*n_channels), out[3]);
		d += 4*n_channels;
	}
	for(; n < n_samples; n++) {
		in[0] = _mm_load_ss(&s0[n]);
		in[1] = _mm_load_ss(&s1[n]);
		in[2] = _mm_load_ss(&s2[n]);
		in[3] = _mm_load_ss(&s3[n]);

		in[0] = _mm_unpacklo_ps(in[0], in[2]);
		in[1] = _mm_unpacklo_ps(in[1], in[3]);
		in[0] = _mm_unpacklo_ps(in[0], in[1]);

		in[0] = _mm_mul_ps(in[0], scale);
		in[0] = _MM_CLAMP_PS(in[0], int_min, int_max);
		out[0] = _mm_cvtps_epi32(in[0]);
		out[0] = _mm_slli_epi32(out[0], 8);
		_mm_storeu_si128((__m128i*)d, out[0]);
		d += n_channels;
	}
}

void
conv_f32d_to_s32_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	int32_t *d = dst[0];
	uint32_t i = 0, n_channels = conv->n_channels;

	for(; i + 3 < n_channels; i += 4)
		conv_f32d_to_s32_4s_sse2(conv, &d[i], &src[i], n_channels, n_samples);
	for(; i + 1 < n_channels; i += 2)
		conv_f32d_to_s32_2s_sse2(conv, &d[i], &src[i], n_channels, n_samples);
	for(; i < n_channels; i++)
		conv_f32d_to_s32_1s_sse2(conv, &d[i], &src[i], n_channels, n_samples);
}

/* 32 bit xorshift PRNG, see https://en.wikipedia.org/wiki/Xorshift */
#define _MM_XORSHIFT_EPI32(r)				\
({							\
	__m128i i, t;					\
	i = _mm_load_si128((__m128i*)r);		\
	t = _mm_slli_epi32(i, 13);			\
	i = _mm_xor_si128(i, t);			\
	t = _mm_srli_epi32(i, 17);			\
	i = _mm_xor_si128(i, t);			\
	t = _mm_slli_epi32(i, 5);			\
	i = _mm_xor_si128(i, t);			\
	_mm_store_si128((__m128i*)r, i);		\
	i;						\
})

void conv_noise_rect_sse2(struct convert *conv, float *noise, uint32_t n_samples)
{
	uint32_t n;
	const uint32_t *r = conv->random;
	__m128 scale = _mm_set1_ps(conv->scale);
	__m128i in[1];
	__m128 out[1];

	for (n = 0; n < n_samples; n += 4) {
		in[0] = _MM_XORSHIFT_EPI32(r);
		out[0] = _mm_cvtepi32_ps(in[0]);
		out[0] = _mm_mul_ps(out[0], scale);
		_mm_store_ps(&noise[n], out[0]);
	}
}

void conv_noise_tri_sse2(struct convert *conv, float *noise, uint32_t n_samples)
{
	uint32_t n;
	const uint32_t *r = conv->random;
	__m128 scale = _mm_set1_ps(conv->scale);
	__m128i in[1];
	__m128 out[1];

	for (n = 0; n < n_samples; n += 4) {
		in[0] = _mm_sub_epi32( _MM_XORSHIFT_EPI32(r), _MM_XORSHIFT_EPI32(r));
		out[0] = _mm_cvtepi32_ps(in[0]);
		out[0] = _mm_mul_ps(out[0], scale);
		_mm_store_ps(&noise[n], out[0]);
	}
}

void conv_noise_tri_hf_sse2(struct convert *conv, float *noise, uint32_t n_samples)
{
	uint32_t n;
	int32_t *p = conv->prev;
	const uint32_t *r = conv->random;
	__m128 scale = _mm_set1_ps(conv->scale);
	__m128i in[1], old[1], new[1];
	__m128 out[1];

	old[0] = _mm_load_si128((__m128i*)p);
	for (n = 0; n < n_samples; n += 4) {
		new[0] = _MM_XORSHIFT_EPI32(r);
		in[0] = _mm_sub_epi32(old[0], new[0]);
		old[0] = new[0];
		out[0] = _mm_cvtepi32_ps(in[0]);
		out[0] = _mm_mul_ps(out[0], scale);
		_mm_store_ps(&noise[n], out[0]);
	}
	_mm_store_si128((__m128i*)p, old[0]);
}

static void
conv_f32d_to_s32_1s_noise_sse2(struct convert *conv, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src,
		float *noise, uint32_t n_channels, uint32_t n_samples)
{
	const float *s = src;
	int32_t *d = dst;
	uint32_t n, unrolled;
	__m128 in[1];
	__m128i out[4];
	__m128 scale = _mm_set1_ps(S24_SCALE);
	__m128 int_min = _mm_set1_ps(S24_MIN);
	__m128 int_max = _mm_set1_ps(S24_MAX);

	if (SPA_IS_ALIGNED(s, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		in[0] = _mm_mul_ps(_mm_load_ps(&s[n]), scale);
		in[0] = _mm_add_ps(in[0], _mm_load_ps(&noise[n]));
		in[0] = _MM_CLAMP_PS(in[0], int_min, int_max);
		out[0] = _mm_cvtps_epi32(in[0]);
		out[0] = _mm_slli_epi32(out[0], 8);
		out[1] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(0, 3, 2, 1));
		out[2] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(1, 0, 3, 2));
		out[3] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(2, 1, 0, 3));

		d[0*n_channels] = _mm_cvtsi128_si32(out[0]);
		d[1*n_channels] = _mm_cvtsi128_si32(out[1]);
		d[2*n_channels] = _mm_cvtsi128_si32(out[2]);
		d[3*n_channels] = _mm_cvtsi128_si32(out[3]);
		d += 4*n_channels;
	}
	for(; n < n_samples; n++) {
		in[0] = _mm_load_ss(&s[n]);
		in[0] = _mm_mul_ss(in[0], scale);
		in[0] = _mm_add_ss(in[0], _mm_load_ss(&noise[n]));
		in[0] = _MM_CLAMP_SS(in[0], int_min, int_max);
		*d = _mm_cvtss_si32(in[0]) << 8;
		d += n_channels;
	}
}

void
conv_f32d_to_s32_noise_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	int32_t *d = dst[0];
	uint32_t i, k, chunk, n_channels = conv->n_channels;
	float *noise = conv->noise;

	convert_update_noise(conv, noise, SPA_MIN(n_samples, conv->noise_size));

	for(i = 0; i < n_channels; i++) {
		const float *s = src[i];
		for(k = 0; k < n_samples; k += chunk) {
			chunk = SPA_MIN(n_samples - k, conv->noise_size);
			conv_f32d_to_s32_1s_noise_sse2(conv, &d[i + k*n_channels],
					&s[k], noise, n_channels, chunk);
		}
	}
}

static void
conv_interleave_32_1s_sse2(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples)
{
	const int32_t *s0 = src[0];
	int32_t *d = dst;
	uint32_t n, unrolled;
	__m128i out[4];

	if (SPA_IS_ALIGNED(s0, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		out[0] = _mm_load_si128((__m128i*)&s0[n]);
		out[1] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(0, 3, 2, 1));
		out[2] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(1, 0, 3, 2));
		out[3] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(2, 1, 0, 3));

		d[0*n_channels] = _mm_cvtsi128_si32(out[0]);
		d[1*n_channels] = _mm_cvtsi128_si32(out[1]);
		d[2*n_channels] = _mm_cvtsi128_si32(out[2]);
		d[3*n_channels] = _mm_cvtsi128_si32(out[3]);
		d += 4*n_channels;
	}
	for(; n < n_samples; n++) {
		*d = s0[n];
		d += n_channels;
	}
}
static void
conv_interleave_32_4s_sse2(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s0 = src[0], *s1 = src[1], *s2 = src[2], *s3 = src[3];
	float *d = dst;
	uint32_t n, unrolled;
	__m128 out[4];

	if (SPA_IS_ALIGNED(s0, 16) &&
	    SPA_IS_ALIGNED(s1, 16) &&
	    SPA_IS_ALIGNED(s2, 16) &&
	    SPA_IS_ALIGNED(s3, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		out[0] = _mm_load_ps(&s0[n]);
		out[1] = _mm_load_ps(&s1[n]);
		out[2] = _mm_load_ps(&s2[n]);
		out[3] = _mm_load_ps(&s3[n]);

		_MM_TRANSPOSE4_PS(out[0], out[1], out[2], out[3]);

		_mm_storeu_ps((d + 0*n_channels), out[0]);
		_mm_storeu_ps((d + 1*n_channels), out[1]);
		_mm_storeu_ps((d + 2*n_channels), out[2]);
		_mm_storeu_ps((d + 3*n_channels), out[3]);
		d += 4*n_channels;
	}
	for(; n < n_samples; n++) {
		out[0] = _mm_setr_ps(s0[n], s1[n], s2[n], s3[n]);
		_mm_storeu_ps(d, out[0]);
		d += n_channels;
	}
}

void
conv_32d_to_32_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	int32_t *d = dst[0];
	uint32_t i = 0, n_channels = conv->n_channels;

	for(; i + 3 < n_channels; i += 4)
		conv_interleave_32_4s_sse2(conv, &d[i], &src[i], n_channels, n_samples);
	for(; i < n_channels; i++)
		conv_interleave_32_1s_sse2(conv, &d[i], &src[i], n_channels, n_samples);
}

#define _MM_BSWAP_EPI32(x)						\
({									\
	__m128i a = _mm_or_si128(					\
		_mm_slli_epi16(x, 8),					\
		_mm_srli_epi16(x, 8));					\
	a = _mm_shufflelo_epi16(a, _MM_SHUFFLE(2, 3, 0, 1));		\
	a = _mm_shufflehi_epi16(a, _MM_SHUFFLE(2, 3, 0, 1));		\
})

static void
conv_interleave_32s_1s_sse2(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples)
{
	const int32_t *s0 = src[0];
	int32_t *d = dst;
	uint32_t n, unrolled;
	__m128i out[4];

	if (SPA_IS_ALIGNED(s0, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		out[0] = _mm_load_si128((__m128i*)&s0[n]);
		out[0] = _MM_BSWAP_EPI32(out[0]);
		out[1] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(0, 3, 2, 1));
		out[2] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(1, 0, 3, 2));
		out[3] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(2, 1, 0, 3));

		d[0*n_channels] = _mm_cvtsi128_si32(out[0]);
		d[1*n_channels] = _mm_cvtsi128_si32(out[1]);
		d[2*n_channels] = _mm_cvtsi128_si32(out[2]);
		d[3*n_channels] = _mm_cvtsi128_si32(out[3]);
		d += 4*n_channels;
	}
	for(; n < n_samples; n++) {
		*d = bswap_32(s0[n]);
		d += n_channels;
	}
}
static void
conv_interleave_32s_4s_sse2(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s0 = src[0], *s1 = src[1], *s2 = src[2], *s3 = src[3];
	float *d = dst;
	uint32_t n, unrolled;
	__m128 out[4];

	if (SPA_IS_ALIGNED(s0, 16) &&
	    SPA_IS_ALIGNED(s1, 16) &&
	    SPA_IS_ALIGNED(s2, 16) &&
	    SPA_IS_ALIGNED(s3, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		out[0] = _mm_load_ps(&s0[n]);
		out[1] = _mm_load_ps(&s1[n]);
		out[2] = _mm_load_ps(&s2[n]);
		out[3] = _mm_load_ps(&s3[n]);

		_MM_TRANSPOSE4_PS(out[0], out[1], out[2], out[3]);

		out[0] = (__m128) _MM_BSWAP_EPI32((__m128i)out[0]);
		out[1] = (__m128) _MM_BSWAP_EPI32((__m128i)out[1]);
		out[2] = (__m128) _MM_BSWAP_EPI32((__m128i)out[2]);
		out[3] = (__m128) _MM_BSWAP_EPI32((__m128i)out[3]);

		_mm_storeu_ps(&d[0*n_channels], out[0]);
		_mm_storeu_ps(&d[1*n_channels], out[1]);
		_mm_storeu_ps(&d[2*n_channels], out[2]);
		_mm_storeu_ps(&d[3*n_channels], out[3]);
		d += 4*n_channels;
	}
	for(; n < n_samples; n++) {
		out[0] = _mm_setr_ps(s0[n], s1[n], s2[n], s3[n]);
		out[0] = (__m128) _MM_BSWAP_EPI32((__m128i)out[0]);
		_mm_storeu_ps(d, out[0]);
		d += n_channels;
	}
}

void
conv_32d_to_32s_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	int32_t *d = dst[0];
	uint32_t i = 0, n_channels = conv->n_channels;

	for(; i + 3 < n_channels; i += 4)
		conv_interleave_32s_4s_sse2(conv, &d[i], &src[i], n_channels, n_samples);
	for(; i < n_channels; i++)
		conv_interleave_32s_1s_sse2(conv, &d[i], &src[i], n_channels, n_samples);
}

static void
conv_deinterleave_32_1s_sse2(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s = src;
	float *d0 = dst[0];
	uint32_t n, unrolled;
	__m128 out;

	if (SPA_IS_ALIGNED(d0, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		out = _mm_setr_ps(s[0*n_channels],
				  s[1*n_channels],
				  s[2*n_channels],
				  s[3*n_channels]);
		_mm_store_ps(&d0[n], out);
		s += 4*n_channels;
	}
	for(; n < n_samples; n++) {
		d0[n] = *s;
		s += n_channels;
	}
}

static void
conv_deinterleave_32_4s_sse2(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s = src;
	float *d0 = dst[0], *d1 = dst[1], *d2 = dst[2], *d3 = dst[3];
	uint32_t n, unrolled;
	__m128 out[4];

	if (SPA_IS_ALIGNED(d0, 16) &&
	    SPA_IS_ALIGNED(d1, 16) &&
	    SPA_IS_ALIGNED(d2, 16) &&
	    SPA_IS_ALIGNED(d3, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		out[0] = _mm_loadu_ps(&s[0 * n_channels]);
		out[1] = _mm_loadu_ps(&s[1 * n_channels]);
		out[2] = _mm_loadu_ps(&s[2 * n_channels]);
		out[3] = _mm_loadu_ps(&s[3 * n_channels]);

		_MM_TRANSPOSE4_PS(out[0], out[1], out[2], out[3]);

		_mm_store_ps(&d0[n], out[0]);
		_mm_store_ps(&d1[n], out[1]);
		_mm_store_ps(&d2[n], out[2]);
		_mm_store_ps(&d3[n], out[3]);
		s += 4 * n_channels;
	}
	for(; n < n_samples; n++) {
		d0[n] = s[0];
		d1[n] = s[1];
		d2[n] = s[2];
		d3[n] = s[3];
		s += n_channels;
	}
}

void
conv_32_to_32d_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float *s = src[0];
	uint32_t i = 0, n_channels = conv->n_channels;

	for(; i + 3 < n_channels; i += 4)
		conv_deinterleave_32_4s_sse2(conv, &dst[i], &s[i], n_channels, n_samples);
	for(; i < n_channels; i++)
		conv_deinterleave_32_1s_sse2(conv, &dst[i], &s[i], n_channels, n_samples);
}

static void
conv_deinterleave_32s_1s_sse2(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s = src;
	float *d0 = dst[0];
	uint32_t n, unrolled;
	__m128 out;

	if (SPA_IS_ALIGNED(d0, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		out = _mm_setr_ps(s[0*n_channels],
				  s[1*n_channels],
				  s[2*n_channels],
				  s[3*n_channels]);
		out = (__m128) _MM_BSWAP_EPI32((__m128i)out);
		_mm_store_ps(&d0[n], out);
		s += 4*n_channels;
	}
	for(; n < n_samples; n++) {
		d0[n] = bswap_32(*s);
		s += n_channels;
	}
}

static void
conv_deinterleave_32s_4s_sse2(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s = src;
	float *d0 = dst[0], *d1 = dst[1], *d2 = dst[2], *d3 = dst[3];
	uint32_t n, unrolled;
	__m128 out[4];

	if (SPA_IS_ALIGNED(d0, 16) &&
	    SPA_IS_ALIGNED(d1, 16) &&
	    SPA_IS_ALIGNED(d2, 16) &&
	    SPA_IS_ALIGNED(d3, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		out[0] = _mm_loadu_ps(&s[0 * n_channels]);
		out[1] = _mm_loadu_ps(&s[1 * n_channels]);
		out[2] = _mm_loadu_ps(&s[2 * n_channels]);
		out[3] = _mm_loadu_ps(&s[3 * n_channels]);

		_MM_TRANSPOSE4_PS(out[0], out[1], out[2], out[3]);

		out[0] = (__m128) _MM_BSWAP_EPI32((__m128i)out[0]);
		out[1] = (__m128) _MM_BSWAP_EPI32((__m128i)out[1]);
		out[2] = (__m128) _MM_BSWAP_EPI32((__m128i)out[2]);
		out[3] = (__m128) _MM_BSWAP_EPI32((__m128i)out[3]);

		_mm_store_ps(&d0[n], out[0]);
		_mm_store_ps(&d1[n], out[1]);
		_mm_store_ps(&d2[n], out[2]);
		_mm_store_ps(&d3[n], out[3]);
		s += 4 * n_channels;
	}
	for(; n < n_samples; n++) {
		d0[n] = bswap_32(s[0]);
		d1[n] = bswap_32(s[1]);
		d2[n] = bswap_32(s[2]);
		d3[n] = bswap_32(s[3]);
		s += n_channels;
	}
}

void
conv_32s_to_32d_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float *s = src[0];
	uint32_t i = 0, n_channels = conv->n_channels;

	for(; i + 3 < n_channels; i += 4)
		conv_deinterleave_32s_4s_sse2(conv, &dst[i], &s[i], n_channels, n_samples);
	for(; i < n_channels; i++)
		conv_deinterleave_32s_1s_sse2(conv, &dst[i], &s[i], n_channels, n_samples);
}

static void
conv_f32_to_s16_1_sse2(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src,
		uint32_t n_samples)
{
	const float *s = src;
	int16_t *d = dst;
	uint32_t n, unrolled;
	__m128 in[2];
	__m128i out[2];
	__m128 int_scale = _mm_set1_ps(S16_SCALE);
	__m128 int_max = _mm_set1_ps(S16_MAX);
        __m128 int_min = _mm_set1_ps(S16_MIN);

	if (SPA_IS_ALIGNED(s, 16))
		unrolled = n_samples & ~7;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 8) {
		in[0] = _mm_mul_ps(_mm_load_ps(&s[n]), int_scale);
		in[1] = _mm_mul_ps(_mm_load_ps(&s[n+4]), int_scale);
		out[0] = _mm_cvtps_epi32(in[0]);
		out[1] = _mm_cvtps_epi32(in[1]);
		out[0] = _mm_packs_epi32(out[0], out[1]);
		_mm_storeu_si128((__m128i*)(d+0), out[0]);
		d += 8;
	}
	for(; n < n_samples; n++) {
		in[0] = _mm_mul_ss(_mm_load_ss(&s[n]), int_scale);
		in[0] = _MM_CLAMP_SS(in[0], int_min, int_max);
		*d++ = _mm_cvtss_si32(in[0]);
	}
}

void
conv_f32d_to_s16d_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	for(i = 0; i < n_channels; i++)
		conv_f32_to_s16_1_sse2(conv, dst[i], src[i], n_samples);
}

void
conv_f32_to_s16_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	conv_f32_to_s16_1_sse2(conv, dst[0], src[0], n_samples * conv->n_channels);
}

static void
conv_f32d_to_s16_1s_sse2(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s0 = src[0];
	int16_t *d = dst;
	uint32_t n, unrolled;
	__m128 in[2];
	__m128i out[2];
	__m128 int_scale = _mm_set1_ps(S16_SCALE);
	__m128 int_max = _mm_set1_ps(S16_MAX);
        __m128 int_min = _mm_set1_ps(S16_MIN);

	if (SPA_IS_ALIGNED(s0, 16))
		unrolled = n_samples & ~7;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 8) {
		in[0] = _mm_mul_ps(_mm_load_ps(&s0[n]), int_scale);
		in[1] = _mm_mul_ps(_mm_load_ps(&s0[n+4]), int_scale);
		out[0] = _mm_cvtps_epi32(in[0]);
		out[1] = _mm_cvtps_epi32(in[1]);
		out[0] = _mm_packs_epi32(out[0], out[1]);

		d[0*n_channels] = _mm_extract_epi16(out[0], 0);
		d[1*n_channels] = _mm_extract_epi16(out[0], 1);
		d[2*n_channels] = _mm_extract_epi16(out[0], 2);
		d[3*n_channels] = _mm_extract_epi16(out[0], 3);
		d[4*n_channels] = _mm_extract_epi16(out[0], 4);
		d[5*n_channels] = _mm_extract_epi16(out[0], 5);
		d[6*n_channels] = _mm_extract_epi16(out[0], 6);
		d[7*n_channels] = _mm_extract_epi16(out[0], 7);
		d += 8*n_channels;
	}
	for(; n < n_samples; n++) {
		in[0] = _mm_mul_ss(_mm_load_ss(&s0[n]), int_scale);
		in[0] = _MM_CLAMP_SS(in[0], int_min, int_max);
		*d = _mm_cvtss_si32(in[0]);
		d += n_channels;
	}
}

static void
conv_f32d_to_s16_2s_sse2(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s0 = src[0], *s1 = src[1];
	int16_t *d = dst;
	uint32_t n, unrolled;
	__m128 in[2];
	__m128i out[4], t[2];
	__m128 int_scale = _mm_set1_ps(S16_SCALE);
	__m128 int_max = _mm_set1_ps(S16_MAX);
        __m128 int_min = _mm_set1_ps(S16_MIN);

	if (SPA_IS_ALIGNED(s0, 16) &&
	    SPA_IS_ALIGNED(s1, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		in[0] = _mm_mul_ps(_mm_load_ps(&s0[n]), int_scale);
		in[1] = _mm_mul_ps(_mm_load_ps(&s1[n]), int_scale);

		t[0] = _mm_cvtps_epi32(in[0]);
		t[1] = _mm_cvtps_epi32(in[1]);

		t[0] = _mm_packs_epi32(t[0], t[0]);
		t[1] = _mm_packs_epi32(t[1], t[1]);

		out[0] = _mm_unpacklo_epi16(t[0], t[1]);
		out[1] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(0, 3, 2, 1));
		out[2] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(1, 0, 3, 2));
		out[3] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(2, 1, 0, 3));

		spa_write_unaligned(d + 0*n_channels, uint32_t, _mm_cvtsi128_si32(out[0]));
		spa_write_unaligned(d + 1*n_channels, uint32_t, _mm_cvtsi128_si32(out[1]));
		spa_write_unaligned(d + 2*n_channels, uint32_t, _mm_cvtsi128_si32(out[2]));
		spa_write_unaligned(d + 3*n_channels, uint32_t, _mm_cvtsi128_si32(out[3]));
		d += 4*n_channels;
	}
	for(; n < n_samples; n++) {
		in[0] = _mm_mul_ss(_mm_load_ss(&s0[n]), int_scale);
		in[1] = _mm_mul_ss(_mm_load_ss(&s1[n]), int_scale);
		in[0] = _MM_CLAMP_SS(in[0], int_min, int_max);
		in[1] = _MM_CLAMP_SS(in[1], int_min, int_max);
		d[0] = _mm_cvtss_si32(in[0]);
		d[1] = _mm_cvtss_si32(in[1]);
		d += n_channels;
	}
}

static void
conv_f32d_to_s16_4s_sse2(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s0 = src[0], *s1 = src[1], *s2 = src[2], *s3 = src[3];
	int16_t *d = dst;
	uint32_t n, unrolled;
	__m128 in[4];
	__m128i out[4], t[4];
	__m128 int_scale = _mm_set1_ps(S16_SCALE);
	__m128 int_max = _mm_set1_ps(S16_MAX);
        __m128 int_min = _mm_set1_ps(S16_MIN);

	if (SPA_IS_ALIGNED(s0, 16) &&
	    SPA_IS_ALIGNED(s1, 16) &&
	    SPA_IS_ALIGNED(s2, 16) &&
	    SPA_IS_ALIGNED(s3, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		in[0] = _mm_mul_ps(_mm_load_ps(&s0[n]), int_scale);
		in[1] = _mm_mul_ps(_mm_load_ps(&s1[n]), int_scale);
		in[2] = _mm_mul_ps(_mm_load_ps(&s2[n]), int_scale);
		in[3] = _mm_mul_ps(_mm_load_ps(&s3[n]), int_scale);

		t[0] = _mm_cvtps_epi32(in[0]);
		t[1] = _mm_cvtps_epi32(in[1]);
		t[2] = _mm_cvtps_epi32(in[2]);
		t[3] = _mm_cvtps_epi32(in[3]);

		t[0] = _mm_packs_epi32(t[0], t[2]);
		t[1] = _mm_packs_epi32(t[1], t[3]);

		out[0] = _mm_unpacklo_epi16(t[0], t[1]);
		out[1] = _mm_unpackhi_epi16(t[0], t[1]);
		out[2] = _mm_unpacklo_epi32(out[0], out[1]);
		out[3] = _mm_unpackhi_epi32(out[0], out[1]);

		_mm_storel_pi((__m64*)(d + 0*n_channels), (__m128)out[2]);
		_mm_storeh_pi((__m64*)(d + 1*n_channels), (__m128)out[2]);
		_mm_storel_pi((__m64*)(d + 2*n_channels), (__m128)out[3]);
		_mm_storeh_pi((__m64*)(d + 3*n_channels), (__m128)out[3]);

		d += 4*n_channels;
	}
	for(; n < n_samples; n++) {
		in[0] = _mm_mul_ss(_mm_load_ss(&s0[n]), int_scale);
		in[1] = _mm_mul_ss(_mm_load_ss(&s1[n]), int_scale);
		in[2] = _mm_mul_ss(_mm_load_ss(&s2[n]), int_scale);
		in[3] = _mm_mul_ss(_mm_load_ss(&s3[n]), int_scale);
		in[0] = _MM_CLAMP_SS(in[0], int_min, int_max);
		in[1] = _MM_CLAMP_SS(in[1], int_min, int_max);
		in[2] = _MM_CLAMP_SS(in[2], int_min, int_max);
		in[3] = _MM_CLAMP_SS(in[3], int_min, int_max);
		d[0] = _mm_cvtss_si32(in[0]);
		d[1] = _mm_cvtss_si32(in[1]);
		d[2] = _mm_cvtss_si32(in[2]);
		d[3] = _mm_cvtss_si32(in[3]);
		d += n_channels;
	}
}

void
conv_f32d_to_s16_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	int16_t *d = dst[0];
	uint32_t i = 0, n_channels = conv->n_channels;

	for(; i + 3 < n_channels; i += 4)
		conv_f32d_to_s16_4s_sse2(conv, &d[i], &src[i], n_channels, n_samples);
	for(; i + 1 < n_channels; i += 2)
		conv_f32d_to_s16_2s_sse2(conv, &d[i], &src[i], n_channels, n_samples);
	for(; i < n_channels; i++)
		conv_f32d_to_s16_1s_sse2(conv, &d[i], &src[i], n_channels, n_samples);
}

static void
conv_f32d_to_s16_1s_noise_sse2(struct convert *conv, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src,
		const float *noise, uint32_t n_channels, uint32_t n_samples)
{
	const float *s0 = src;
	int16_t *d = dst;
	uint32_t n, unrolled;
	__m128 in[2];
	__m128i out[2];
	__m128 int_scale = _mm_set1_ps(S16_SCALE);
	__m128 int_max = _mm_set1_ps(S16_MAX);
        __m128 int_min = _mm_set1_ps(S16_MIN);

	if (SPA_IS_ALIGNED(s0, 16))
		unrolled = n_samples & ~7;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 8) {
		in[0] = _mm_mul_ps(_mm_load_ps(&s0[n]), int_scale);
		in[1] = _mm_mul_ps(_mm_load_ps(&s0[n+4]), int_scale);
		in[0] = _mm_add_ps(in[0], _mm_load_ps(&noise[n]));
		in[1] = _mm_add_ps(in[1], _mm_load_ps(&noise[n+4]));
		out[0] = _mm_cvtps_epi32(in[0]);
		out[1] = _mm_cvtps_epi32(in[1]);
		out[0] = _mm_packs_epi32(out[0], out[1]);

		d[0*n_channels] = _mm_extract_epi16(out[0], 0);
		d[1*n_channels] = _mm_extract_epi16(out[0], 1);
		d[2*n_channels] = _mm_extract_epi16(out[0], 2);
		d[3*n_channels] = _mm_extract_epi16(out[0], 3);
		d[4*n_channels] = _mm_extract_epi16(out[0], 4);
		d[5*n_channels] = _mm_extract_epi16(out[0], 5);
		d[6*n_channels] = _mm_extract_epi16(out[0], 6);
		d[7*n_channels] = _mm_extract_epi16(out[0], 7);
		d += 8*n_channels;
	}
	for(; n < n_samples; n++) {
		in[0] = _mm_mul_ss(_mm_load_ss(&s0[n]), int_scale);
		in[0] = _mm_add_ss(in[0], _mm_load_ss(&noise[n]));
		in[0] = _MM_CLAMP_SS(in[0], int_min, int_max);
		*d = _mm_cvtss_si32(in[0]);
		d += n_channels;
	}
}

void
conv_f32d_to_s16_noise_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	int16_t *d = dst[0];
	uint32_t i, k, chunk, n_channels = conv->n_channels;
	float *noise = conv->noise;

	convert_update_noise(conv, noise, SPA_MIN(n_samples, conv->noise_size));

	for(i = 0; i < n_channels; i++) {
		const float *s = src[i];
		for(k = 0; k < n_samples; k += chunk) {
			chunk = SPA_MIN(n_samples - k, conv->noise_size);
			conv_f32d_to_s16_1s_noise_sse2(conv, &d[i + k*n_channels],
					&s[k], noise, n_channels, chunk);
		}
	}
}

static void
conv_f32_to_s16_1_noise_sse2(struct convert *conv, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src,
		const float *noise, uint32_t n_samples)
{
	const float *s = src;
	int16_t *d = dst;
	uint32_t n, unrolled;
	__m128 in[2];
	__m128i out[2];
	__m128 int_scale = _mm_set1_ps(S16_SCALE);
	__m128 int_max = _mm_set1_ps(S16_MAX);
        __m128 int_min = _mm_set1_ps(S16_MIN);

	if (SPA_IS_ALIGNED(s, 16))
		unrolled = n_samples & ~7;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 8) {
		in[0] = _mm_mul_ps(_mm_load_ps(&s[n]), int_scale);
		in[1] = _mm_mul_ps(_mm_load_ps(&s[n+4]), int_scale);
		in[0] = _mm_add_ps(in[0], _mm_load_ps(&noise[n]));
		in[1] = _mm_add_ps(in[1], _mm_load_ps(&noise[n+4]));
		out[0] = _mm_cvtps_epi32(in[0]);
		out[1] = _mm_cvtps_epi32(in[1]);
		out[0] = _mm_packs_epi32(out[0], out[1]);
		_mm_storeu_si128((__m128i*)(&d[n]), out[0]);
	}
	for(; n < n_samples; n++) {
		in[0] = _mm_mul_ss(_mm_load_ss(&s[n]), int_scale);
		in[0] = _mm_add_ss(in[0], _mm_load_ss(&noise[n]));
		in[0] = _MM_CLAMP_SS(in[0], int_min, int_max);
		d[n] = _mm_cvtss_si32(in[0]);
	}
}

void
conv_f32d_to_s16d_noise_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, k, chunk, n_channels = conv->n_channels;
	float *noise = conv->noise;

	convert_update_noise(conv, noise, SPA_MIN(n_samples, conv->noise_size));

	for(i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int16_t *d = dst[i];
		for(k = 0; k < n_samples; k += chunk) {
			chunk = SPA_MIN(n_samples - k, conv->noise_size);
			conv_f32_to_s16_1_noise_sse2(conv, &d[k], &s[k], noise, chunk);
		}
	}
}

void
conv_f32d_to_s16_2_sse2(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float *s0 = src[0], *s1 = src[1];
	int16_t *d = dst[0];
	uint32_t n, unrolled;
	__m128 in[4];
	__m128i out[4];
	__m128 int_scale = _mm_set1_ps(S16_SCALE);
	__m128 int_max = _mm_set1_ps(S16_MAX);
        __m128 int_min = _mm_set1_ps(S16_MIN);

	if (SPA_IS_ALIGNED(s0, 16) &&
	    SPA_IS_ALIGNED(s1, 16))
		unrolled = n_samples & ~7;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 8) {
		in[0] = _mm_mul_ps(_mm_load_ps(&s0[n+0]), int_scale);
		in[1] = _mm_mul_ps(_mm_load_ps(&s1[n+0]), int_scale);
		in[2] = _mm_mul_ps(_mm_load_ps(&s0[n+4]), int_scale);
		in[3] = _mm_mul_ps(_mm_load_ps(&s1[n+4]), int_scale);

		out[0] = _mm_cvtps_epi32(in[0]);
		out[1] = _mm_cvtps_epi32(in[1]);
		out[2] = _mm_cvtps_epi32(in[2]);
		out[3] = _mm_cvtps_epi32(in[3]);

		out[0] = _mm_packs_epi32(out[0], out[2]);
		out[1] = _mm_packs_epi32(out[1], out[3]);

		out[2] = _mm_unpacklo_epi16(out[0], out[1]);
		out[3] = _mm_unpackhi_epi16(out[0], out[1]);

		_mm_storeu_si128((__m128i*)(d+0), out[2]);
		_mm_storeu_si128((__m128i*)(d+8), out[3]);

		d += 16;
	}
	for(; n < n_samples; n++) {
		in[0] = _mm_mul_ss(_mm_load_ss(&s0[n]), int_scale);
		in[1] = _mm_mul_ss(_mm_load_ss(&s1[n]), int_scale);
		in[0] = _MM_CLAMP_SS(in[0], int_min, int_max);
		in[1] = _MM_CLAMP_SS(in[1], int_min, int_max);
		d[0] = _mm_cvtss_si32(in[0]);
		d[1] = _mm_cvtss_si32(in[1]);
		d += 2;
	}
}
