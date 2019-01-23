/* Spa
 *
 * Copyright © 2018 Wim Taymans
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

#include <spa/utils/defs.h>

#include <emmintrin.h>

static void
conv_s16_to_f32d_1_sse2(void *data, int n_dst, void *dst[n_dst], const void *src, int n_samples)
{
	const int16_t *s = src;
	float **d = (float **) dst;
	float *d0 = d[0];
	int n = 0, unrolled;
	__m128i in;
	__m128 out, factor = _mm_set1_ps(1.0f / S16_SCALE);

	unrolled = n_samples / 4;
	n_samples = n_samples & 3;

	for(; unrolled--; n += 4) {
		in = _mm_insert_epi16(in, s[0*n_dst], 1);
		in = _mm_insert_epi16(in, s[1*n_dst], 3);
		in = _mm_insert_epi16(in, s[2*n_dst], 5);
		in = _mm_insert_epi16(in, s[3*n_dst], 7);
		in = _mm_srai_epi32(in, 16);
		out = _mm_cvtepi32_ps(in);
		out = _mm_mul_ps(out, factor);
		_mm_storeu_ps(&d0[n], out);
		s += 4*n_dst;
	}
	for(; n_samples--; n++) {
		out = _mm_cvtsi32_ss(out, s[0]);
		out = _mm_mul_ss(out, factor);
		_mm_store_ss(&d0[n], out);
		s += n_dst;
	}
}

static void
conv_s16_to_f32d_2_sse2(void *data, int n_dst, void *dst[n_dst], const void *src, int n_samples)
{
	const int16_t *s = src;
	float **d = (float **) dst;
	float *d0 = d[0], *d1 = d[1];
	int n = 0, unrolled;
	__m128i in, t[2];
	__m128 out[2], factor = _mm_set1_ps(1.0f / S16_SCALE);

	if (n_dst == 2) {
		unrolled = n_samples / 4;
		n_samples = n_samples & 3;

		for(; unrolled--; n += 4) {
			in = _mm_loadu_si128((__m128i*)s);

			t[0] = _mm_slli_epi32(in, 16);
			t[0] = _mm_srai_epi32(t[0], 16);
			t[1] = _mm_srai_epi32(in, 16);

			out[0] = _mm_cvtepi32_ps(t[0]);
			out[0] = _mm_mul_ps(out[0], factor);
			out[1] = _mm_cvtepi32_ps(t[1]);
			out[1] = _mm_mul_ps(out[1], factor);

			_mm_storeu_ps(&d0[n], out[0]);
			_mm_storeu_ps(&d1[n], out[1]);

			s += 4*n_dst;
		}
	}
	for(; n_samples--; n++) {
		out[0] = _mm_cvtsi32_ss(out[0], s[0]);
		out[0] = _mm_mul_ss(out[0], factor);
		out[1] = _mm_cvtsi32_ss(out[1], s[1]);
		out[1] = _mm_mul_ss(out[1], factor);
		_mm_store_ss(&d0[n], out[0]);
		_mm_store_ss(&d1[n], out[1]);
		s += n_dst;
	}
}

static void
conv_s16_to_f32d_sse2(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const int16_t *s = src[0];
	int i = 0;

	for(; i + 1 < n_dst; i += 2)
		conv_s16_to_f32d_2_sse2(data, n_dst, &dst[i], &s[i], n_samples);
	for(; i < n_dst; i++)
		conv_s16_to_f32d_1_sse2(data, n_dst, &dst[i], &s[i], n_samples);
}

static void
conv_s24_to_f32d_1_sse2(void *data, int n_dst, void *dst[n_dst], const void *src, int n_samples)
{
	const uint8_t *s = src;
	float **d = (float **) dst;
	float *d0 = d[0];
	int n = 0, unrolled;
	__m128i in;
	__m128 out, factor = _mm_set1_ps(1.0f / S24_SCALE);

	unrolled = n_samples / 4;
	n_samples = n_samples & 3;
	if (n_samples == 0) {
		n_samples += 4;
		unrolled--;
	}

	for(; unrolled--; n += 4) {
		in = _mm_setr_epi32(
			*((uint32_t*)&s[0 * n_dst]),
			*((uint32_t*)&s[3 * n_dst]),
			*((uint32_t*)&s[6 * n_dst]),
			*((uint32_t*)&s[9 * n_dst]));
		in = _mm_slli_epi32(in, 8);
		in = _mm_srai_epi32(in, 8);
		out = _mm_cvtepi32_ps(in);
		out = _mm_mul_ps(out, factor);
		_mm_storeu_ps(&d0[n], out);
		s += 12 * n_dst;
	}
	for(; n_samples--; n++) {
		out = _mm_cvtsi32_ss(out, read_s24(s));
		out = _mm_mul_ss(out, factor);
		_mm_store_ss(&d0[n], out);
		s += 3 * n_dst;
	}
}

static void
conv_s24_to_f32d_sse2(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const int8_t *s = src[0];
	int i = 0;

	for(; i < n_dst; i++)
		conv_s24_to_f32d_1_sse2(data, n_dst, &dst[i], &s[3*i], n_samples);
}

static void
conv_f32d_to_s32_1_sse2(void *data, void *dst, int n_src, const void *src[n_src], int n_samples)
{
	const float **s = (const float **) src;
	const float *s0 = s[0];
	int32_t *d = dst;
	int n, unrolled;
	__m128 in[1];
	__m128i out[4];
	__m128 int_max = _mm_set1_ps(S24_MAX_F);
        __m128 int_min = _mm_sub_ps(_mm_setzero_ps(), int_max);

	unrolled = n_samples / 4;
	n_samples = n_samples & 3;

	for(n = 0; unrolled--; n += 4) {
		in[0] = _mm_mul_ps(_mm_loadu_ps(&s0[n]), int_max);
		in[0] = _mm_min_ps(int_max, _mm_max_ps(in[0], int_min));

		out[0] = _mm_slli_epi32(_mm_cvtps_epi32(in[0]), 8);
		out[1] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(0, 3, 2, 1));
		out[2] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(1, 0, 3, 2));
		out[3] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(2, 1, 0, 3));

		d[0*n_src] = _mm_cvtsi128_si32(out[0]);
		d[1*n_src] = _mm_cvtsi128_si32(out[1]);
		d[2*n_src] = _mm_cvtsi128_si32(out[2]);
		d[3*n_src] = _mm_cvtsi128_si32(out[3]);
		d += 4*n_src;
	}
	for(; n_samples--; n++) {
		in[0] = _mm_load_ss(&s0[n]);
		in[0] = _mm_mul_ss(in[0], int_max);
		in[0] = _mm_min_ss(int_max, _mm_max_ss(in[0], int_min));
		*d = _mm_cvtss_si32(in[0]) << 8;
		d += n_src;
	}
}

static void
conv_f32d_to_s32_2_sse2(void *data, void *dst, int n_src, const void *src[n_src], int n_samples)
{
	const float **s = (const float **) src;
	const float *s0 = s[0], *s1 = s[1];
	int32_t *d = dst;
	int n, unrolled;
	__m128 in[2];
	__m128i out[2], t[2];
	__m128 int_max = _mm_set1_ps(S24_MAX_F);
        __m128 int_min = _mm_sub_ps(_mm_setzero_ps(), int_max);

	unrolled = n_samples / 4;
	n_samples = n_samples & 3;

	for(n = 0; unrolled--; n += 4) {
		in[0] = _mm_mul_ps(_mm_loadu_ps(&s0[n]), int_max);
		in[1] = _mm_mul_ps(_mm_loadu_ps(&s1[n]), int_max);

		in[0] = _mm_min_ps(int_max, _mm_max_ps(in[0], int_min));
		in[1] = _mm_min_ps(int_max, _mm_max_ps(in[1], int_min));

		out[0] = _mm_slli_epi32(_mm_cvtps_epi32(in[0]), 8);
		out[1] = _mm_slli_epi32(_mm_cvtps_epi32(in[1]), 8);

		t[0] = _mm_unpacklo_epi32(out[0], out[1]);
		t[1] = _mm_shuffle_epi32(t[0], _MM_SHUFFLE(0, 0, 2, 2));
		t[2] = _mm_unpackhi_epi32(out[0], out[1]);
		t[3] = _mm_shuffle_epi32(t[2], _MM_SHUFFLE(0, 0, 2, 2));

		_mm_storel_epi64((__m128i*)(d + 0*n_src), t[0]);
		_mm_storel_epi64((__m128i*)(d + 1*n_src), t[1]);
		_mm_storel_epi64((__m128i*)(d + 2*n_src), t[2]);
		_mm_storel_epi64((__m128i*)(d + 3*n_src), t[3]);
		d += 4*n_src;
	}
	for(; n_samples--; n++) {
		in[0] = _mm_load_ss(&s0[n]);
		in[1] = _mm_load_ss(&s1[n]);

		in[0] = _mm_unpacklo_ps(in[0], in[1]);

		in[0] = _mm_mul_ps(in[0], int_max);
		in[0] = _mm_min_ps(int_max, _mm_max_ps(in[0], int_min));
		out[0] = _mm_slli_epi32(_mm_cvtps_epi32(in[0]), 8);
		_mm_storel_epi64((__m128i*)d, out[0]);
		d += n_src;
	}
}

static void
conv_f32d_to_s32_4_sse2(void *data, void *dst, int n_src, const void *src[n_src], int n_samples)
{
	const float **s = (const float **) src;
	const float *s0 = s[0], *s1 = s[1], *s2 = s[2], *s3 = s[3];
	int32_t *d = dst;
	int n, unrolled;
	__m128 in[4];
	__m128i out[4], t[4];
	__m128 int_max = _mm_set1_ps(S24_MAX_F);
        __m128 int_min = _mm_sub_ps(_mm_setzero_ps(), int_max);

	unrolled = n_samples / 4;
	n_samples = n_samples & 3;

	for(n = 0; unrolled--; n += 4) {
		in[0] = _mm_mul_ps(_mm_loadu_ps(&s0[n]), int_max);
		in[1] = _mm_mul_ps(_mm_loadu_ps(&s1[n]), int_max);
		in[2] = _mm_mul_ps(_mm_loadu_ps(&s2[n]), int_max);
		in[3] = _mm_mul_ps(_mm_loadu_ps(&s3[n]), int_max);

		in[0] = _mm_min_ps(int_max, _mm_max_ps(in[0], int_min));
		in[1] = _mm_min_ps(int_max, _mm_max_ps(in[1], int_min));
		in[2] = _mm_min_ps(int_max, _mm_max_ps(in[2], int_min));
		in[3] = _mm_min_ps(int_max, _mm_max_ps(in[3], int_min));

		out[0] = _mm_slli_epi32(_mm_cvtps_epi32(in[0]), 8);
		out[1] = _mm_slli_epi32(_mm_cvtps_epi32(in[1]), 8);
		out[2] = _mm_slli_epi32(_mm_cvtps_epi32(in[2]), 8);
		out[3] = _mm_slli_epi32(_mm_cvtps_epi32(in[3]), 8);

		/* transpose */
		t[0] = _mm_unpacklo_epi32(out[0], out[1]);
		t[1] = _mm_unpacklo_epi32(out[2], out[3]);
		t[2] = _mm_unpackhi_epi32(out[0], out[1]);
		t[3] = _mm_unpackhi_epi32(out[2], out[3]);
		out[0] = _mm_unpacklo_epi64(t[0], t[1]);
		out[1] = _mm_unpackhi_epi64(t[0], t[1]);
		out[2] = _mm_unpacklo_epi64(t[2], t[3]);
		out[3] = _mm_unpackhi_epi64(t[2], t[3]);

		_mm_storeu_si128((__m128i*)(d + 0*n_src), out[0]);
		_mm_storeu_si128((__m128i*)(d + 1*n_src), out[1]);
		_mm_storeu_si128((__m128i*)(d + 2*n_src), out[2]);
		_mm_storeu_si128((__m128i*)(d + 3*n_src), out[3]);
		d += 4*n_src;
	}
	for(; n_samples--; n++) {
		in[0] = _mm_load_ss(&s0[n]);
		in[1] = _mm_load_ss(&s1[n]);
		in[2] = _mm_load_ss(&s2[n]);
		in[3] = _mm_load_ss(&s3[n]);

		in[0] = _mm_unpacklo_ps(in[0], in[2]);
		in[1] = _mm_unpacklo_ps(in[1], in[3]);
		in[0] = _mm_unpacklo_ps(in[0], in[1]);

		in[0] = _mm_mul_ps(in[0], int_max);
		in[0] = _mm_min_ps(int_max, _mm_max_ps(in[0], int_min));
		out[0] = _mm_slli_epi32(_mm_cvtps_epi32(in[0]), 8);
		_mm_storeu_si128((__m128i*)d, out[0]);
		d += n_src;
	}
}

static void
conv_f32d_to_s32_sse2(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int32_t *d = dst[0];
	int i = 0;

	for(; i + 3 < n_src; i += 4)
		conv_f32d_to_s32_4_sse2(data, &d[i], n_src, &src[i], n_samples);
	for(; i + 1 < n_src; i += 2)
		conv_f32d_to_s32_2_sse2(data, &d[i], n_src, &src[i], n_samples);
	for(; i < n_src; i++)
		conv_f32d_to_s32_1_sse2(data, &d[i], n_src, &src[i], n_samples);
}

static void
conv_f32d_to_s16_1_sse2(void *data, void *dst, int n_src, const void *src[n_src], int n_samples)
{
	const float **s = (const float **) src;
	const float *s0 = s[0];
	int16_t *d = dst;
	int n, unrolled;
	__m128 in[2];
	__m128i out[2];
	__m128 int_max = _mm_set1_ps(S16_MAX_F);
        __m128 int_min = _mm_sub_ps(_mm_setzero_ps(), int_max);

	unrolled = n_samples / 8;
	n_samples = n_samples & 7;

	for(n = 0; unrolled--; n += 8) {
		in[0] = _mm_mul_ps(_mm_loadu_ps(&s0[n]), int_max);
		in[1] = _mm_mul_ps(_mm_loadu_ps(&s0[n+4]), int_max);
		out[0] = _mm_cvtps_epi32(in[0]);
		out[1] = _mm_cvtps_epi32(in[1]);
		out[0] = _mm_packs_epi32(out[0], out[1]);

		d[0*n_src] = _mm_extract_epi16(out[0], 0);
		d[1*n_src] = _mm_extract_epi16(out[0], 1);
		d[2*n_src] = _mm_extract_epi16(out[0], 2);
		d[3*n_src] = _mm_extract_epi16(out[0], 3);
		d[4*n_src] = _mm_extract_epi16(out[0], 4);
		d[5*n_src] = _mm_extract_epi16(out[0], 5);
		d[6*n_src] = _mm_extract_epi16(out[0], 6);
		d[7*n_src] = _mm_extract_epi16(out[0], 7);
		d += 8*n_src;
	}
	for(; n_samples--; n++) {
		in[0] = _mm_mul_ss(_mm_load_ss(&s0[n]), int_max);
		in[0] = _mm_min_ss(int_max, _mm_max_ss(in[0], int_min));
		*d = _mm_cvtss_si32(in[0]);
		d += n_src;
	}
}

static void
conv_f32d_to_s16_2_sse2(void *data, void *dst, int n_src, const void *src[n_src], int n_samples)
{
	const float **s = (const float **) src;
	const float *s0 = s[0], *s1 = s[1];
	int16_t *d = dst;
	int n = 0, unrolled;
	__m128 in[2];
	__m128i out[4], t[2];
	__m128 int_max = _mm_set1_ps(S16_MAX_F);
        __m128 int_min = _mm_sub_ps(_mm_setzero_ps(), int_max);

	unrolled = n_samples / 4;
	n_samples = n_samples & 3;

	for(; unrolled--; n += 4) {
		in[0] = _mm_mul_ps(_mm_loadu_ps(&s0[n]), int_max);
		in[1] = _mm_mul_ps(_mm_loadu_ps(&s1[n]), int_max);

		t[0] = _mm_cvtps_epi32(in[0]);
		t[1] = _mm_cvtps_epi32(in[1]);

		t[0] = _mm_packs_epi32(t[0], t[0]);
		t[1] = _mm_packs_epi32(t[1], t[1]);

		out[0] = _mm_unpacklo_epi16(t[0], t[1]);
		out[1] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(0, 3, 2, 1));
		out[2] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(1, 0, 3, 2));
		out[3] = _mm_shuffle_epi32(out[0], _MM_SHUFFLE(2, 1, 0, 3));

		*((uint32_t*)(d + 0*n_src)) = _mm_cvtsi128_si32(out[0]);
		*((uint32_t*)(d + 1*n_src)) = _mm_cvtsi128_si32(out[1]);
		*((uint32_t*)(d + 2*n_src)) = _mm_cvtsi128_si32(out[2]);
		*((uint32_t*)(d + 3*n_src)) = _mm_cvtsi128_si32(out[3]);
		d += 4*n_src;
	}
	for(; n_samples--; n++) {
		in[0] = _mm_mul_ss(_mm_load_ss(&s0[n]), int_max);
		in[1] = _mm_mul_ss(_mm_load_ss(&s1[n]), int_max);
		in[0] = _mm_min_ss(int_max, _mm_max_ss(in[0], int_min));
		in[1] = _mm_min_ss(int_max, _mm_max_ss(in[1], int_min));
		d[0] = _mm_cvtss_si32(in[0]);
		d[1] = _mm_cvtss_si32(in[1]);
		d += n_src;
	}
}

static void
conv_f32d_to_s16_sse2(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int16_t *d = dst[0];
	int i = 0;

	for(; i + 1 < n_src; i += 2)
		conv_f32d_to_s16_2_sse2(data, &d[i], n_src, &src[i], n_samples);
	for(; i < n_src; i++)
		conv_f32d_to_s16_1_sse2(data, &d[i], n_src, &src[i], n_samples);
}
