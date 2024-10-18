/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <spa/utils/defs.h>

#include "config.h"
#include "dsp-ops.h"

#include <immintrin.h>

void dsp_mix_gain_avx(struct dsp_ops *ops,
		void * SPA_RESTRICT dst,
		const void * SPA_RESTRICT src[],
		float gain[], uint32_t n_src, uint32_t n_samples)
{
	if (n_src == 0) {
		memset(dst, 0, n_samples * sizeof(float));
	} else if (n_src == 1 && gain[0] == 1.0f) {
		if (dst != src[0])
			spa_memcpy(dst, src[0], n_samples * sizeof(float));
	} else {
		uint32_t n, i, unrolled;
		__m256 in[4], g;
		const float **s = (const float **)src;
		float *d = dst;

		if (SPA_LIKELY(SPA_IS_ALIGNED(dst, 32))) {
			unrolled = n_samples & ~31;
			for (i = 0; i < n_src; i++) {
				if (SPA_UNLIKELY(!SPA_IS_ALIGNED(src[i], 32))) {
					unrolled = 0;
					break;
				}
			}
		} else
			unrolled = 0;

		for (n = 0; n < unrolled; n += 32) {
			g = _mm256_set1_ps(gain[0]);
			in[0] = _mm256_mul_ps(g, _mm256_load_ps(&s[0][n+ 0]));
			in[1] = _mm256_mul_ps(g, _mm256_load_ps(&s[0][n+ 8]));
			in[2] = _mm256_mul_ps(g, _mm256_load_ps(&s[0][n+16]));
			in[3] = _mm256_mul_ps(g, _mm256_load_ps(&s[0][n+24]));

			for (i = 1; i < n_src; i++) {
				g = _mm256_set1_ps(gain[i]);
				in[0] = _mm256_add_ps(in[0], _mm256_mul_ps(g, _mm256_load_ps(&s[i][n+ 0])));
				in[1] = _mm256_add_ps(in[1], _mm256_mul_ps(g, _mm256_load_ps(&s[i][n+ 8])));
				in[2] = _mm256_add_ps(in[2], _mm256_mul_ps(g, _mm256_load_ps(&s[i][n+16])));
				in[3] = _mm256_add_ps(in[3], _mm256_mul_ps(g, _mm256_load_ps(&s[i][n+24])));
			}
			_mm256_store_ps(&d[n+ 0], in[0]);
			_mm256_store_ps(&d[n+ 8], in[1]);
			_mm256_store_ps(&d[n+16], in[2]);
			_mm256_store_ps(&d[n+24], in[3]);
		}
		for (; n < n_samples; n++) {
			__m128 in[1], g;
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

void dsp_sum_avx(struct dsp_ops *ops, float *r, const float *a, const float *b, uint32_t n_samples)
{
	uint32_t n, unrolled;
	__m256 in[4];

	unrolled = n_samples & ~31;

	if (SPA_LIKELY(SPA_IS_ALIGNED(r, 32)) &&
	    SPA_LIKELY(SPA_IS_ALIGNED(a, 32)) &&
	    SPA_LIKELY(SPA_IS_ALIGNED(b, 32))) {
		for (n = 0; n < unrolled; n += 32) {
			in[0] = _mm256_load_ps(&a[n+ 0]);
			in[1] = _mm256_load_ps(&a[n+ 8]);
			in[2] = _mm256_load_ps(&a[n+16]);
			in[3] = _mm256_load_ps(&a[n+24]);

			in[0] = _mm256_add_ps(in[0], _mm256_load_ps(&b[n+ 0]));
			in[1] = _mm256_add_ps(in[1], _mm256_load_ps(&b[n+ 8]));
			in[2] = _mm256_add_ps(in[2], _mm256_load_ps(&b[n+16]));
			in[3] = _mm256_add_ps(in[3], _mm256_load_ps(&b[n+24]));

			_mm256_store_ps(&r[n+ 0], in[0]);
			_mm256_store_ps(&r[n+ 8], in[1]);
			_mm256_store_ps(&r[n+16], in[2]);
			_mm256_store_ps(&r[n+24], in[3]);
		}
	} else {
		for (n = 0; n < unrolled; n += 32) {
			in[0] = _mm256_loadu_ps(&a[n+ 0]);
			in[1] = _mm256_loadu_ps(&a[n+ 8]);
			in[2] = _mm256_loadu_ps(&a[n+16]);
			in[3] = _mm256_loadu_ps(&a[n+24]);

			in[0] = _mm256_add_ps(in[0], _mm256_loadu_ps(&b[n+ 0]));
			in[1] = _mm256_add_ps(in[1], _mm256_loadu_ps(&b[n+ 8]));
			in[2] = _mm256_add_ps(in[2], _mm256_loadu_ps(&b[n+16]));
			in[3] = _mm256_add_ps(in[3], _mm256_loadu_ps(&b[n+24]));

			_mm256_storeu_ps(&r[n+ 0], in[0]);
			_mm256_storeu_ps(&r[n+ 8], in[1]);
			_mm256_storeu_ps(&r[n+16], in[2]);
			_mm256_storeu_ps(&r[n+24], in[3]);
		}
	}
	for (; n < n_samples; n++) {
		__m128 in[1];
		in[0] = _mm_load_ss(&a[n]);
		in[0] = _mm_add_ss(in[0], _mm_load_ss(&b[n]));
		_mm_store_ss(&r[n], in[0]);
	}
}
