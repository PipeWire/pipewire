/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "resample-native-impl.h"

#include <xmmintrin.h>

static inline void inner_product_sse(float *d, const float * SPA_RESTRICT s,
		const float * SPA_RESTRICT taps, uint32_t n_taps)
{
	__m128 sum[2] = { _mm_setzero_ps(), _mm_setzero_ps() };
	uint32_t i;

	for (i = 0; i < n_taps; i += 8) {
		sum[0] = _mm_add_ps(sum[0],
			_mm_mul_ps(
				_mm_loadu_ps(s + i + 0),
				_mm_load_ps(taps + i + 0)));
		sum[1] = _mm_add_ps(sum[1],
			_mm_mul_ps(
				_mm_loadu_ps(s + i + 4),
				_mm_load_ps(taps + i + 4)));
	}
	sum[0] = _mm_add_ps(sum[0], sum[1]);
	sum[0] = _mm_add_ps(sum[0], _mm_movehl_ps(sum[0], sum[0]));
	sum[0] = _mm_add_ss(sum[0], _mm_shuffle_ps(sum[0], sum[0], 0x55));
	_mm_store_ss(d, sum[0]);
}

static inline void inner_product_ip_sse(float *d, const float * SPA_RESTRICT s,
	const float * SPA_RESTRICT t0, const float * SPA_RESTRICT t1, float x,
	uint32_t n_taps)
{
	__m128 sum[4] = { _mm_setzero_ps(), _mm_setzero_ps(),
			  _mm_setzero_ps(), _mm_setzero_ps() }, t;
	uint32_t i;

	for (i = 0; i < n_taps; i += 8) {
		t = _mm_loadu_ps(s + i + 0);
		sum[0] = _mm_add_ps(sum[0], _mm_mul_ps(t, _mm_load_ps(t0 + i + 0)));
		sum[1] = _mm_add_ps(sum[1], _mm_mul_ps(t, _mm_load_ps(t1 + i + 0)));
		t = _mm_loadu_ps(s + i + 4);
		sum[2] = _mm_add_ps(sum[2], _mm_mul_ps(t, _mm_load_ps(t0 + i + 4)));
		sum[3] = _mm_add_ps(sum[3], _mm_mul_ps(t, _mm_load_ps(t1 + i + 4)));
	}
	sum[0] = _mm_add_ps(sum[0], sum[2]);
	sum[1] = _mm_add_ps(sum[1], sum[3]);
	sum[1] = _mm_mul_ps(_mm_sub_ps(sum[1], sum[0]), _mm_load1_ps(&x));
	sum[0] = _mm_add_ps(sum[0], sum[1]);
	sum[0] = _mm_add_ps(sum[0], _mm_movehl_ps(sum[0], sum[0]));
	sum[0] = _mm_add_ss(sum[0], _mm_shuffle_ps(sum[0], sum[0], 0x55));
	_mm_store_ss(d, sum[0]);
}

MAKE_RESAMPLER_FULL(sse);
MAKE_RESAMPLER_INTER(sse);
