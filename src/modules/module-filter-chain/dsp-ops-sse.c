/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <spa/utils/defs.h>

#include "dsp-ops.h"

#include <xmmintrin.h>

void dsp_mix_gain_sse(struct dsp_ops *ops,
		void * SPA_RESTRICT dst,
		const void * SPA_RESTRICT src[],
		float gain[], uint32_t n_src, uint32_t n_samples)
{
	if (n_src == 0) {
		memset(dst, 0, n_samples * sizeof(float));
	} else if (n_src == 1) {
		if (dst != src[0])
			spa_memcpy(dst, src[0], n_samples * sizeof(float));
	} else {
		uint32_t n, i, unrolled;
		__m128 in[4], g;
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
			g = _mm_set1_ps(gain[0]);
			in[0] = _mm_mul_ps(g, _mm_load_ps(&s[0][n+ 0]));
			in[1] = _mm_mul_ps(g, _mm_load_ps(&s[0][n+ 4]));
			in[2] = _mm_mul_ps(g, _mm_load_ps(&s[0][n+ 8]));
			in[3] = _mm_mul_ps(g, _mm_load_ps(&s[0][n+12]));

			for (i = 1; i < n_src; i++) {
				g = _mm_set1_ps(gain[i]);
				in[0] = _mm_add_ps(in[0], _mm_mul_ps(g, _mm_load_ps(&s[i][n+ 0])));
				in[1] = _mm_add_ps(in[1], _mm_mul_ps(g, _mm_load_ps(&s[i][n+ 4])));
				in[2] = _mm_add_ps(in[2], _mm_mul_ps(g, _mm_load_ps(&s[i][n+ 8])));
				in[3] = _mm_add_ps(in[3], _mm_mul_ps(g, _mm_load_ps(&s[i][n+12])));
			}
			_mm_store_ps(&d[n+ 0], in[0]);
			_mm_store_ps(&d[n+ 4], in[1]);
			_mm_store_ps(&d[n+ 8], in[2]);
			_mm_store_ps(&d[n+12], in[3]);
		}
		for (; n < n_samples; n++) {
			g = _mm_set_ss(gain[0]);
			in[0] = _mm_mul_ss(g, _mm_load_ss(&s[0][n]));
			for (i = 1; i < n_src; i++) {
				g = _mm_set_ss(gain[i]);
				in[0] = _mm_add_ss(in[0], _mm_mul_ss(g, _mm_load_ss(&s[i][n])));
			}
			_mm_store_ss(&d[n], in[0]);
		}
	}
}

void dsp_sum_sse(struct dsp_ops *ops, float *r, const float *a, const float *b, uint32_t n_samples)
{
	uint32_t n, unrolled;
	__m128 in[4];

	unrolled = n_samples & ~15;

	if (SPA_LIKELY(SPA_IS_ALIGNED(r, 16)) &&
	    SPA_LIKELY(SPA_IS_ALIGNED(a, 16)) &&
	    SPA_LIKELY(SPA_IS_ALIGNED(b, 16))) {
		for (n = 0; n < unrolled; n += 16) {
			in[0] = _mm_load_ps(&a[n+ 0]);
			in[1] = _mm_load_ps(&a[n+ 4]);
			in[2] = _mm_load_ps(&a[n+ 8]);
			in[3] = _mm_load_ps(&a[n+12]);

			in[0] = _mm_add_ps(in[0], _mm_load_ps(&b[n+ 0]));
			in[1] = _mm_add_ps(in[1], _mm_load_ps(&b[n+ 4]));
			in[2] = _mm_add_ps(in[2], _mm_load_ps(&b[n+ 8]));
			in[3] = _mm_add_ps(in[3], _mm_load_ps(&b[n+12]));

			_mm_store_ps(&r[n+ 0], in[0]);
			_mm_store_ps(&r[n+ 4], in[1]);
			_mm_store_ps(&r[n+ 8], in[2]);
			_mm_store_ps(&r[n+12], in[3]);
		}
	} else {
		for (n = 0; n < unrolled; n += 16) {
			in[0] = _mm_loadu_ps(&a[n+ 0]);
			in[1] = _mm_loadu_ps(&a[n+ 4]);
			in[2] = _mm_loadu_ps(&a[n+ 8]);
			in[3] = _mm_loadu_ps(&a[n+12]);

			in[0] = _mm_add_ps(in[0], _mm_loadu_ps(&b[n+ 0]));
			in[1] = _mm_add_ps(in[1], _mm_loadu_ps(&b[n+ 4]));
			in[2] = _mm_add_ps(in[2], _mm_loadu_ps(&b[n+ 8]));
			in[3] = _mm_add_ps(in[3], _mm_loadu_ps(&b[n+12]));

			_mm_storeu_ps(&r[n+ 0], in[0]);
			_mm_storeu_ps(&r[n+ 4], in[1]);
			_mm_storeu_ps(&r[n+ 8], in[2]);
			_mm_storeu_ps(&r[n+12], in[3]);
		}
	}
	for (; n < n_samples; n++) {
		in[0] = _mm_load_ss(&a[n]);
		in[0] = _mm_add_ss(in[0], _mm_load_ss(&b[n]));
		_mm_store_ss(&r[n], in[0]);
	}
}
