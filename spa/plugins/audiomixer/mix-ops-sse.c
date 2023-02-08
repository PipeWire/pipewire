/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <spa/utils/defs.h>

#include "mix-ops.h"

#include <xmmintrin.h>

void
mix_f32_sse(struct mix_ops *ops, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_src, uint32_t n_samples)
{
	n_samples *= ops->n_channels;

	if (n_src == 0) {
		memset(dst, 0, n_samples * sizeof(float));
	} else if (n_src == 1) {
		if (dst != src[0])
			spa_memcpy(dst, src[0], n_samples * sizeof(float));
	} else {
		uint32_t n, i, unrolled;
		__m128 in[4];
		const float **s = (const float **)src;
		float *d = dst;

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

		for (n = 0; n < unrolled; n += 16) {
			in[0] = _mm_load_ps(&s[0][n+ 0]);
			in[1] = _mm_load_ps(&s[0][n+ 4]);
			in[2] = _mm_load_ps(&s[0][n+ 8]);
			in[3] = _mm_load_ps(&s[0][n+12]);

			for (i = 1; i < n_src; i++) {
				in[0] = _mm_add_ps(in[0], _mm_load_ps(&s[i][n+ 0]));
				in[1] = _mm_add_ps(in[1], _mm_load_ps(&s[i][n+ 4]));
				in[2] = _mm_add_ps(in[2], _mm_load_ps(&s[i][n+ 8]));
				in[3] = _mm_add_ps(in[3], _mm_load_ps(&s[i][n+12]));
			}
			_mm_store_ps(&d[n+ 0], in[0]);
			_mm_store_ps(&d[n+ 4], in[1]);
			_mm_store_ps(&d[n+ 8], in[2]);
			_mm_store_ps(&d[n+12], in[3]);
		}
		for (; n < n_samples; n++) {
			in[0] = _mm_load_ss(&s[0][n]);
			for (i = 1; i < n_src; i++)
				in[0] = _mm_add_ss(in[0], _mm_load_ss(&s[i][n]));
			_mm_store_ss(&d[n], in[0]);
		}
	}
}
