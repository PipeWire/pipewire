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

#include "channelmix-ops.h"

#include <xmmintrin.h>

static inline void clear_sse(float *d, uint32_t n_samples)
{
	memset(d, 0, n_samples * sizeof(float));
}

static inline void copy_sse(float *d, const float *s, uint32_t n_samples)
{
	spa_memcpy(d, s, n_samples * sizeof(float));
}

static inline void vol_sse(float *d, const float *s, float vol, uint32_t n_samples)
{
	uint32_t n, unrolled;
	if (vol == 0.0f) {
		clear_sse(d, n_samples);
	} else if (vol == 1.0f) {
		copy_sse(d, s, n_samples);
	} else {
		__m128 t[4];
		const __m128 v = _mm_set1_ps(vol);

		if (SPA_IS_ALIGNED(d, 16) &&
		    SPA_IS_ALIGNED(s, 16))
			unrolled = n_samples & ~15;
		else
			unrolled = 0;

		for(n = 0; n < unrolled; n += 16) {
			t[0] = _mm_load_ps(&s[n]);
			t[1] = _mm_load_ps(&s[n+4]);
			t[2] = _mm_load_ps(&s[n+8]);
			t[3] = _mm_load_ps(&s[n+12]);
			_mm_store_ps(&d[n], _mm_mul_ps(t[0], v));
			_mm_store_ps(&d[n+4], _mm_mul_ps(t[1], v));
			_mm_store_ps(&d[n+8], _mm_mul_ps(t[2], v));
			_mm_store_ps(&d[n+12], _mm_mul_ps(t[3], v));
		}
		for(; n < n_samples; n++)
			_mm_store_ss(&d[n], _mm_mul_ss(_mm_load_ss(&s[n]), v));
	}
}

void channelmix_copy_sse(struct channelmix *mix, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n_dst = mix->dst_chan;
	float **d = (float **)dst;
	const float **s = (const float **)src;
	for (i = 0; i < n_dst; i++)
		vol_sse(d[i], s[i], mix->matrix[i][i], n_samples);
}

/* FL+FR+FC+LFE -> FL+FR */
void
channelmix_f32_3p1_2_sse(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	float **d = (float **) dst;
	const float **s = (const float **) src;
	const float m0 = mix->matrix[0][0];
	const float m1 = mix->matrix[1][1];
	const float m2 = (mix->matrix[0][2] + mix->matrix[1][2]) * 0.5f;
	const float m3 = (mix->matrix[0][3] + mix->matrix[1][3]) * 0.5f;

	if (m0 == 0.0f && m1 == 0.0f && m2 == 0.0f && m3 == 0.0f) {
		clear_sse(d[0], n_samples);
		clear_sse(d[1], n_samples);
	}
	else {
		uint32_t n, unrolled;
		const __m128 v0 = _mm_set1_ps(m0);
		const __m128 v1 = _mm_set1_ps(m1);
		const __m128 clev = _mm_set1_ps(m2);
		const __m128 llev = _mm_set1_ps(m3);
		__m128 ctr;

		if (SPA_IS_ALIGNED(s[0], 16) &&
		    SPA_IS_ALIGNED(s[1], 16) &&
		    SPA_IS_ALIGNED(s[2], 16) &&
		    SPA_IS_ALIGNED(s[3], 16) &&
		    SPA_IS_ALIGNED(d[0], 16) &&
		    SPA_IS_ALIGNED(d[1], 16))
			unrolled = n_samples & ~3;
		else
			unrolled = 0;

		for(n = 0; n < unrolled; n += 4) {
			ctr = _mm_add_ps(
					_mm_mul_ps(_mm_load_ps(&s[2][n]), clev),
					_mm_mul_ps(_mm_load_ps(&s[3][n]), llev));
			_mm_store_ps(&d[0][n], _mm_add_ps(_mm_mul_ps(_mm_load_ps(&s[0][n]), v0), ctr));
			_mm_store_ps(&d[1][n], _mm_add_ps(_mm_mul_ps(_mm_load_ps(&s[1][n]), v1), ctr));
		}
		for(; n < n_samples; n++) {
			ctr = _mm_add_ss(_mm_mul_ss(_mm_load_ss(&s[2][n]), clev),
					_mm_mul_ss(_mm_load_ss(&s[3][n]), llev));
			_mm_store_ss(&d[0][n], _mm_add_ss(_mm_mul_ss(_mm_load_ss(&s[0][n]), v0), ctr));
			_mm_store_ss(&d[1][n], _mm_add_ss(_mm_mul_ss(_mm_load_ss(&s[1][n]), v1), ctr));
		}
	}
}

/* FL+FR+FC+LFE+SL+SR -> FL+FR */
void
channelmix_f32_5p1_2_sse(struct channelmix *mix, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t n, unrolled;
	float **d = (float **) dst;
	const float **s = (const float **) src;
	const float m00 = mix->matrix[0][0];
	const float m11 = mix->matrix[1][1];
	const __m128 clev = _mm_set1_ps((mix->matrix[0][2] + mix->matrix[1][2]) * 0.5f);
	const __m128 llev = _mm_set1_ps((mix->matrix[0][3] + mix->matrix[1][3]) * 0.5f);
	const __m128 slev0 = _mm_set1_ps(mix->matrix[0][4]);
	const __m128 slev1 = _mm_set1_ps(mix->matrix[1][5]);
	__m128 in, ctr;

	if (SPA_IS_ALIGNED(s[0], 16) &&
	    SPA_IS_ALIGNED(s[1], 16) &&
	    SPA_IS_ALIGNED(s[2], 16) &&
	    SPA_IS_ALIGNED(s[3], 16) &&
	    SPA_IS_ALIGNED(s[4], 16) &&
	    SPA_IS_ALIGNED(s[5], 16) &&
	    SPA_IS_ALIGNED(d[0], 16) &&
	    SPA_IS_ALIGNED(d[1], 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		clear_sse(d[0], n_samples);
		clear_sse(d[1], n_samples);
	}
	else {
		const __m128 v0 = _mm_set1_ps(m00);
		const __m128 v1 = _mm_set1_ps(m11);
		for(n = 0; n < unrolled; n += 4) {
			ctr = _mm_add_ps(_mm_mul_ps(_mm_load_ps(&s[2][n]), clev),
					_mm_mul_ps(_mm_load_ps(&s[3][n]), llev));
			in = _mm_mul_ps(_mm_load_ps(&s[4][n]), slev0);
			in = _mm_add_ps(in, ctr);
			in = _mm_add_ps(in, _mm_mul_ps(_mm_load_ps(&s[0][n]), v0));
			_mm_store_ps(&d[0][n], in);
			in = _mm_mul_ps(_mm_load_ps(&s[5][n]), slev1);
			in = _mm_add_ps(in, ctr);
			in = _mm_add_ps(in, _mm_mul_ps(_mm_load_ps(&s[1][n]), v1));
			_mm_store_ps(&d[1][n], in);
		}
		for(; n < n_samples; n++) {
			ctr = _mm_mul_ss(_mm_load_ss(&s[2][n]), clev);
			ctr = _mm_add_ss(ctr, _mm_mul_ss(_mm_load_ss(&s[3][n]), llev));
			in = _mm_mul_ss(_mm_load_ss(&s[4][n]), slev0);
			in = _mm_add_ss(in, ctr);
			in = _mm_add_ss(in, _mm_mul_ss(_mm_load_ss(&s[0][n]), v0));
			_mm_store_ss(&d[0][n], in);
			in = _mm_mul_ss(_mm_load_ss(&s[5][n]), slev1);
			in = _mm_add_ss(in, ctr);
			in = _mm_add_ss(in, _mm_mul_ss(_mm_load_ss(&s[1][n]), v1));
			_mm_store_ss(&d[1][n], in);
		}
	}
}

/* FL+FR+FC+LFE+SL+SR -> FL+FR+FC+LFE*/
void
channelmix_f32_5p1_3p1_sse(struct channelmix *mix, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n, unrolled, n_dst = mix->dst_chan;
	float **d = (float **) dst;
	const float **s = (const float **) src;

	if (SPA_IS_ALIGNED(s[0], 16) &&
	    SPA_IS_ALIGNED(s[1], 16) &&
	    SPA_IS_ALIGNED(s[2], 16) &&
	    SPA_IS_ALIGNED(s[3], 16) &&
	    SPA_IS_ALIGNED(s[4], 16) &&
	    SPA_IS_ALIGNED(s[5], 16) &&
	    SPA_IS_ALIGNED(d[0], 16) &&
	    SPA_IS_ALIGNED(d[1], 16) &&
	    SPA_IS_ALIGNED(d[2], 16) &&
	    SPA_IS_ALIGNED(d[3], 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_sse(d[i], n_samples);
	}
	else {
		const __m128 v0 = _mm_set1_ps(mix->matrix[0][0]);
		const __m128 v1 = _mm_set1_ps(mix->matrix[1][1]);
		const __m128 slev0 = _mm_set1_ps(mix->matrix[0][4]);
		const __m128 slev1 = _mm_set1_ps(mix->matrix[1][5]);

		for(n = 0; n < unrolled; n += 4) {
			_mm_store_ps(&d[0][n], _mm_add_ps(
					_mm_mul_ps(_mm_load_ps(&s[0][n]), v0),
					_mm_mul_ps(_mm_load_ps(&s[4][n]), slev0)));

			_mm_store_ps(&d[1][n], _mm_add_ps(
					_mm_mul_ps(_mm_load_ps(&s[1][n]), v1),
					_mm_mul_ps(_mm_load_ps(&s[5][n]), slev1)));
		}
		for(; n < n_samples; n++) {
			_mm_store_ss(&d[0][n], _mm_add_ss(
					_mm_mul_ss(_mm_load_ss(&s[0][n]), v0),
					_mm_mul_ss(_mm_load_ss(&s[4][n]), slev0)));

			_mm_store_ss(&d[1][n], _mm_add_ss(
					_mm_mul_ss(_mm_load_ss(&s[1][n]), v1),
					_mm_mul_ss(_mm_load_ss(&s[5][n]), slev1)));
		}
		vol_sse(d[2], s[2], mix->matrix[2][2], n_samples);
		vol_sse(d[3], s[3], mix->matrix[3][3], n_samples);
	}
}

/* FL+FR+FC+LFE+SL+SR -> FL+FR+RL+RR*/
void
channelmix_f32_5p1_4_sse(struct channelmix *mix, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n_dst = mix->dst_chan;
	float **d = (float **) dst;
	const float **s = (const float **) src;
	const float v4 = mix->matrix[2][4];
	const float v5 = mix->matrix[3][5];

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_sse(d[i], n_samples);
	}
	else {
		channelmix_f32_3p1_2_sse(mix, dst, src, n_samples);

		vol_sse(d[2], s[4], v4, n_samples);
		vol_sse(d[3], s[5], v5, n_samples);
	}
}
