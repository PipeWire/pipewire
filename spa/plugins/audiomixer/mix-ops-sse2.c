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
#include <math.h>

#include <spa/utils/defs.h>

#include "mix-ops.h"

#include <emmintrin.h>

void
mix_f64_sse2(struct mix_ops *ops, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_src, uint32_t n_samples)
{
	n_samples *= ops->n_channels;

	if (n_src == 0) {
		memset(dst, 0, n_samples * sizeof(double));
	} else if (n_src == 1) {
		if (dst != src[0])
			spa_memcpy(dst, src[0], n_samples * sizeof(double));
	} else {
		uint32_t n, i, unrolled;
		__m128d in[4];
		const double **s = (const double **)src;
		double *d = dst;

		if (SPA_LIKELY(SPA_IS_ALIGNED(dst, 16))) {
			unrolled = n_samples & ~15;
			for (i = 0; i < n_src; i++) {
				if (SPA_UNLIKELY(!SPA_IS_ALIGNED(src[i], 16))) {
					unrolled = 0;
					break;
				}
			}
		} else
			unrolled = 0;

		for (n = 0; n < unrolled; n += 8) {
			in[0] = _mm_load_pd(&s[0][n+0]);
			in[1] = _mm_load_pd(&s[0][n+2]);
			in[2] = _mm_load_pd(&s[0][n+4]);
			in[3] = _mm_load_pd(&s[0][n+6]);

			for (i = 1; i < n_src; i++) {
				in[0] = _mm_add_pd(in[0], _mm_load_pd(&s[i][n+0]));
				in[1] = _mm_add_pd(in[1], _mm_load_pd(&s[i][n+2]));
				in[2] = _mm_add_pd(in[2], _mm_load_pd(&s[i][n+4]));
				in[3] = _mm_add_pd(in[3], _mm_load_pd(&s[i][n+6]));
			}
			_mm_store_pd(&d[n+0], in[0]);
			_mm_store_pd(&d[n+2], in[1]);
			_mm_store_pd(&d[n+4], in[2]);
			_mm_store_pd(&d[n+6], in[3]);
		}
		for (; n < n_samples; n++) {
			in[0] = _mm_load_sd(&s[0][n]);
			for (i = 1; i < n_src; i++)
				in[0] = _mm_add_sd(in[0], _mm_load_sd(&s[i][n]));
			_mm_store_sd(&d[n], in[0]);
		}
	}
}
