/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <spa/utils/defs.h>

#include "dsp-ops.h"

#include <immintrin.h>

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
