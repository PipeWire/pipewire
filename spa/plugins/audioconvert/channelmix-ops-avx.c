/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "channelmix-ops.h"

#include <immintrin.h>
#include <float.h>
#include <math.h>

static inline void clear_avx(float *d, uint32_t n_samples)
{
	memset(d, 0, n_samples * sizeof(float));
}

static inline void copy_avx(float *d, const float *s, uint32_t n_samples)
{
	spa_memcpy(d, s, n_samples * sizeof(float));
}

static inline void vol_avx(float *d, const float *s, float vol, uint32_t n_samples)
{
	uint32_t n, unrolled;
	if (vol == 0.0f) {
		clear_avx(d, n_samples);
	} else if (vol == 1.0f) {
		copy_avx(d, s, n_samples);
	} else {
		__m256 t[4];
		const __m256 v = _mm256_set1_ps(vol);

		if (SPA_IS_ALIGNED(d, 32) &&
		    SPA_IS_ALIGNED(s, 32))
			unrolled = n_samples & ~31;
		else
			unrolled = 0;

		for(n = 0; n < unrolled; n += 32) {
			t[0] = _mm256_load_ps(&s[n]);
			t[1] = _mm256_load_ps(&s[n+8]);
			t[2] = _mm256_load_ps(&s[n+16]);
			t[3] = _mm256_load_ps(&s[n+24]);
			_mm256_store_ps(&d[n], _mm256_mul_ps(t[0], v));
			_mm256_store_ps(&d[n+8], _mm256_mul_ps(t[1], v));
			_mm256_store_ps(&d[n+16], _mm256_mul_ps(t[2], v));
			_mm256_store_ps(&d[n+24], _mm256_mul_ps(t[3], v));
		}
		for(; n < n_samples; n++) {
			__m128 v = _mm_set1_ps(vol);
			_mm_store_ss(&d[n], _mm_mul_ss(_mm_load_ss(&s[n]), v));
		}
	}
}

void channelmix_copy_avx(struct channelmix *mix, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n_dst = mix->dst_chan;
	float **d = (float **)dst;
	const float **s = (const float **)src;
	for (i = 0; i < n_dst; i++)
		vol_avx(d[i], s[i], mix->matrix[i][i], n_samples);
}
