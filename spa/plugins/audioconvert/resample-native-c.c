/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "resample-native-impl.h"

static inline void inner_product_c(float *d, const float * SPA_RESTRICT s,
		const float * SPA_RESTRICT taps, uint32_t n_taps)
{
	float sum = 0.0f;
#if 1
	uint32_t i, j, nt2 = n_taps/2;
	for (i = 0, j = n_taps-1; i < nt2; i++, j--)
		sum += s[i] * taps[i] + s[j] * taps[j];
#else
	uint32_t i;
	for (i = 0; i < n_taps; i++)
		sum += s[i] * taps[i];
#endif
	*d = sum;
}

static inline void inner_product_ip_c(float *d, const float * SPA_RESTRICT s,
	const float * SPA_RESTRICT t0, const float * SPA_RESTRICT t1, float x,
	uint32_t n_taps)
{
	float sum[2] = { 0.0f, 0.0f };
	uint32_t i;
#if 1
	uint32_t j, nt2 = n_taps/2;
	for (i = 0, j = n_taps-1; i < nt2; i++, j--) {
		sum[0] += s[i] * t0[i] + s[j] * t0[j];
		sum[1] += s[i] * t1[i] + s[j] * t1[j];
	}
#else
	for (i = 0; i < n_taps; i++) {
		sum[0] += s[i] * t0[i];
		sum[1] += s[i] * t1[i];
	}
#endif
	*d = (sum[1] - sum[0]) * x + sum[0];
}

MAKE_RESAMPLER_FULL(c);
MAKE_RESAMPLER_INTER(c);
