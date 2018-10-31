/* Spa
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <string.h>
#include <stdio.h>

#include <spa/utils/defs.h>

#include <xmmintrin.h>

static void
conv_s16_to_f32d_1_sse(void *data, int n_dst, void *dst[n_dst], const void *src, int n_bytes)
{
	const int16_t *s = src;
	float **d = (float **) dst;
	float *d0 = d[0];
	int n, n_samples;
	__m128 out, factor = _mm_set1_ps(1.0f / S16_SCALE);

	n_samples = n_bytes / (sizeof(int16_t) * n_dst);

	for(n = 0; n_samples--; n++) {
		out = _mm_cvtsi32_ss(out, *s);
		out = _mm_mul_ss(out, factor);
		_mm_store_ss(&d0[n], out);
		s += n_dst;
	}
}

static void
conv_s16_to_f32d_2_sse(void *data, int n_dst, void *dst[n_dst], const void *src, int n_bytes)
{
	const int16_t *s = src;
	float **d = (float **) dst;
	float *d0 = d[0], *d1 = d[1];
	int n = 0, n_samples, unrolled;
	__m128i in, t[2];
	__m128 out[2], factor = _mm_set1_ps(1.0f / S16_SCALE);

	n_samples = n_bytes / (sizeof(int16_t) * n_dst);

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
conv_s16_to_f32d_sse(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_bytes)
{
	const int16_t *s = src[0];
	int i = 0;

	for(; i + 1 < n_dst; i += 2)
		conv_s16_to_f32d_2_sse(data, n_dst, &dst[i], &s[i], n_bytes);
	for(; i < n_dst; i++)
		conv_s16_to_f32d_1_sse(data, n_dst, &dst[i], &s[i], n_bytes);
}

static void
conv_f32d_to_s32_1_sse(void *data, void *dst, int n_src, const void *src[n_src], int n_bytes)
{
	const float **s = (const float **) src;
	const float *s0 = s[0];
	int32_t *d = dst;
	int n, n_samples, unrolled;
	__m128 in[1];
	__m128i out[4];
	__m128 int_max = _mm_set1_ps(S24_MAX_F);
        __m128 int_min = _mm_sub_ps(_mm_setzero_ps(), int_max);

	n_samples = n_bytes / sizeof(float);

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
conv_f32d_to_s32_2_sse(void *data, void *dst, int n_src, const void *src[n_src], int n_bytes)
{
	const float **s = (const float **) src;
	const float *s0 = s[0], *s1 = s[1];
	int32_t *d = dst;
	int n, n_samples, unrolled;
	__m128 in[2];
	__m128i out[2], t[2];
	__m128 int_max = _mm_set1_ps(S24_MAX_F);
        __m128 int_min = _mm_sub_ps(_mm_setzero_ps(), int_max);

	n_samples = n_bytes / sizeof(float);

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
conv_f32d_to_s32_4_sse(void *data, void *dst, int n_src, const void *src[n_src], int n_bytes)
{
	const float **s = (const float **) src;
	const float *s0 = s[0], *s1 = s[1], *s2 = s[2], *s3 = s[3];
	int32_t *d = dst;
	int n, n_samples, unrolled;
	__m128 in[4];
	__m128i out[4], t[4];
	__m128 int_max = _mm_set1_ps(S24_MAX_F);
        __m128 int_min = _mm_sub_ps(_mm_setzero_ps(), int_max);

	n_samples = n_bytes / sizeof(float);

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
conv_f32d_to_s32_sse(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_bytes)
{
	int32_t *d = dst[0];
	int i = 0;

	for(; i + 3 < n_src; i += 4)
		conv_f32d_to_s32_4_sse(data, &d[i], n_src, &src[i], n_bytes);
	for(; i + 1 < n_src; i += 2)
		conv_f32d_to_s32_2_sse(data, &d[i], n_src, &src[i], n_bytes);
	for(; i < n_src; i++)
		conv_f32d_to_s32_1_sse(data, &d[i], n_src, &src[i], n_bytes);
}

static void
conv_f32d_to_s16_1_sse(void *data, void *dst, int n_src, const void *src[n_src], int n_bytes)
{
	const float **s = (const float **) src;
	const float *s0 = s[0];
	int16_t *d = dst;
	int n, n_samples, unrolled;
	__m128 in[1];
	__m128i out[4];
	__m128 int_max = _mm_set1_ps(S16_MAX_F);
        __m128 int_min = _mm_sub_ps(_mm_setzero_ps(), int_max);

	n_samples = n_bytes / sizeof(float);

	unrolled = n_samples / 4;
	n_samples = n_samples & 3;

	for(n = 0; unrolled--; n += 4) {
		in[0] = _mm_mul_ps(_mm_loadu_ps(&s0[n]), int_max);
		in[0] = _mm_min_ps(int_max, _mm_max_ps(in[0], int_min));
		out[0] = _mm_cvtps_epi32(in[0]);
		out[0] = _mm_packs_epi32(out[0], out[0]);

		d[0*n_src] = _mm_extract_pi16(*(__m64*)out, 0);
		d[1*n_src] = _mm_extract_pi16(*(__m64*)out, 1);
		d[2*n_src] = _mm_extract_pi16(*(__m64*)out, 2);
		d[3*n_src] = _mm_extract_pi16(*(__m64*)out, 3);
		d += 4*n_src;
	}
	for(; n_samples--; n++) {
		in[0] = _mm_mul_ss(_mm_load_ss(&s0[n]), int_max);
		in[0] = _mm_min_ss(int_max, _mm_max_ss(in[0], int_min));
		*d = _mm_cvtss_si32(in[0]);
		d += n_src;
	}
}

static void
conv_f32d_to_s16_2_sse(void *data, void *dst, int n_src, const void *src[n_src], int n_bytes)
{
	const float **s = (const float **) src;
	const float *s0 = s[0], *s1 = s[1];
	int16_t *d = dst;
	int n = 0, n_samples, unrolled;
	__m128 in[2];
	__m128i out[4], t[2];
	__m128 int_max = _mm_set1_ps(S16_MAX_F);
        __m128 int_min = _mm_sub_ps(_mm_setzero_ps(), int_max);

	n_samples = n_bytes / sizeof(float);

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

		*((uint32_t*)d + 0*n_src) = _mm_cvtsi128_si32(out[0]);
		*((uint32_t*)d + 1*n_src) = _mm_cvtsi128_si32(out[1]);
		*((uint32_t*)d + 2*n_src) = _mm_cvtsi128_si32(out[2]);
		*((uint32_t*)d + 3*n_src) = _mm_cvtsi128_si32(out[3]);
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
conv_f32d_to_s16_sse(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_bytes)
{
	int16_t *d = dst[0];
	int i = 0;

	for(; i + 1 < n_src; i += 2)
		conv_f32d_to_s16_2_sse(data, &d[i], n_src, &src[i], n_bytes);
	for(; i < n_src; i++)
		conv_f32d_to_s16_1_sse(data, &d[i], n_src, &src[i], n_bytes);
}
