/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <math.h>

#include <xmmintrin.h>

#include "peaks-ops.h"

static inline float hmin_ps(__m128 val)
{
	__m128 t = _mm_movehl_ps(val, val);
	t = _mm_min_ps(t, val);
	val = _mm_shuffle_ps(t, t, 0x55);
	val = _mm_min_ss(t, val);
	return _mm_cvtss_f32(val);
}

static inline float hmax_ps(__m128 val)
{
	__m128 t = _mm_movehl_ps(val, val);
	t = _mm_max_ps(t, val);
	val = _mm_shuffle_ps(t, t, 0x55);
	val = _mm_max_ss(t, val);
	return _mm_cvtss_f32(val);
}

void peaks_min_max_sse(struct peaks *peaks, const float * SPA_RESTRICT src,
		uint32_t n_samples, float *min, float *max)
{
	uint32_t n;
	__m128 in;
	__m128 mi = _mm_set1_ps(*min);
	__m128 ma = _mm_set1_ps(*max);

	for (n = 0; n < n_samples; n++) {
		if (SPA_IS_ALIGNED(&src[n], 16))
			break;
		in = _mm_set1_ps(src[n]);
		mi = _mm_min_ps(mi, in);
		ma = _mm_max_ps(ma, in);
	}
	for (; n + 15 < n_samples; n += 16) {
		in = _mm_load_ps(&src[n + 0]);
		mi = _mm_min_ps(mi, in);
		ma = _mm_max_ps(ma, in);
		in = _mm_load_ps(&src[n + 4]);
		mi = _mm_min_ps(mi, in);
		ma = _mm_max_ps(ma, in);
		in = _mm_load_ps(&src[n + 8]);
		mi = _mm_min_ps(mi, in);
		ma = _mm_max_ps(ma, in);
		in = _mm_load_ps(&src[n + 12]);
		mi = _mm_min_ps(mi, in);
		ma = _mm_max_ps(ma, in);
	}
	for (; n < n_samples; n++) {
		in = _mm_set1_ps(src[n]);
		mi = _mm_min_ps(mi, in);
		ma = _mm_max_ps(ma, in);
	}
	*min = hmin_ps(mi);
	*max = hmax_ps(ma);
}

float peaks_abs_max_sse(struct peaks *peaks, const float * SPA_RESTRICT src,
		uint32_t n_samples, float max)
{
	uint32_t n;
	__m128 in;
	__m128 ma = _mm_set1_ps(max);
	const __m128 mask = _mm_set1_ps(-0.0f);

	for (n = 0; n < n_samples; n++) {
		if (SPA_IS_ALIGNED(&src[n], 16))
			break;
		in = _mm_set1_ps(src[n]);
		in = _mm_andnot_ps(mask, in);
		ma = _mm_max_ps(ma, in);
	}
	for (; n + 15 < n_samples; n += 16) {
		in = _mm_load_ps(&src[n + 0]);
		in = _mm_andnot_ps(mask, in);
		ma = _mm_max_ps(ma, in);
		in = _mm_load_ps(&src[n + 4]);
		in = _mm_andnot_ps(mask, in);
		ma = _mm_max_ps(ma, in);
		in = _mm_load_ps(&src[n + 8]);
		in = _mm_andnot_ps(mask, in);
		ma = _mm_max_ps(ma, in);
		in = _mm_load_ps(&src[n + 12]);
		in = _mm_andnot_ps(mask, in);
		ma = _mm_max_ps(ma, in);
	}
	for (; n < n_samples; n++) {
		in = _mm_set1_ps(src[n]);
		in = _mm_andnot_ps(mask, in);
		ma = _mm_max_ps(ma, in);
	}
	return hmax_ps(ma);
}
