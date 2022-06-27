/* Spa
 *
 * Copyright © 2022 Wim Taymans
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

#include "dither-ops.h"

#include <emmintrin.h>

static inline void update_dither_sse2(struct dither *dt, uint32_t n_samples)
{
	uint32_t n;
	const uint32_t *r = SPA_PTR_ALIGN(dt->random, 16, uint32_t);
	float *dither = SPA_PTR_ALIGN(dt->dither, 16, float);
	__m128 scale = _mm_set1_ps(dt->scale), out[1];
	__m128i in[1], t[1];

	for (n = 0; n < n_samples; n += 4) {
		/* 32 bit xorshift PRNG, see https://en.wikipedia.org/wiki/Xorshift */
		in[0] = _mm_load_si128((__m128i*)r);
		t[0] = _mm_slli_epi32(in[0], 13);
		in[0] = _mm_xor_si128(in[0], t[0]);
		t[0] = _mm_srli_epi32(in[0], 17);
		in[0] = _mm_xor_si128(in[0], t[0]);
		t[0] = _mm_slli_epi32(in[0], 5);
		in[0] = _mm_xor_si128(in[0], t[0]);
		_mm_store_si128((__m128i*)r, in[0]);

		out[0] = _mm_cvtepi32_ps(in[0]);
		out[0] = _mm_mul_ps(out[0], scale);
		_mm_store_ps(&dither[n], out[0]);
	}
}

void dither_f32_sse2(struct dither *dt, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n, m, chunk, unrolled;
	const float **s = (const float**)src;
	float **d = (float**)dst;
	float *dither = SPA_PTR_ALIGN(dt->dither, 16, float);
	__m128 in[4];

	chunk = SPA_MIN(n_samples, dt->dither_size);
	update_dither_sse2(dt, chunk);

	for (n = 0; n < n_samples; n += chunk) {
		chunk = SPA_MIN(n_samples - n, dt->dither_size);

		for (i = 0; i < dt->n_channels; i++) {
			float *di = &d[i][n];
			const float *si = &s[i][n];

			if (SPA_IS_ALIGNED(di, 16) &&
			    SPA_IS_ALIGNED(si, 16))
				unrolled = chunk & ~15;
			else
				unrolled = 0;

			for (m = 0; m < unrolled; m += 16) {
				in[0] = _mm_load_ps(&si[m     ]);
				in[1] = _mm_load_ps(&si[m +  4]);
				in[2] = _mm_load_ps(&si[m +  8]);
				in[3] = _mm_load_ps(&si[m + 12]);
				in[0] = _mm_add_ps(in[0], _mm_load_ps(&dither[m     ]));
				in[1] = _mm_add_ps(in[1], _mm_load_ps(&dither[m +  4]));
				in[2] = _mm_add_ps(in[2], _mm_load_ps(&dither[m +  8]));
				in[3] = _mm_add_ps(in[3], _mm_load_ps(&dither[m + 12]));
				_mm_store_ps(&di[m     ], in[0]);
				_mm_store_ps(&di[m +  4], in[1]);
				_mm_store_ps(&di[m +  8], in[2]);
				_mm_store_ps(&di[m + 12], in[3]);
			}
			for (; m < chunk; m++)
				di[m] = si[m] + dither[m];
		}
	}
}
