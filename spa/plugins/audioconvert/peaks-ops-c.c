/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <math.h>

#include "peaks-ops.h"

void peaks_min_max_c(struct peaks *peaks, const float * SPA_RESTRICT src,
		uint32_t n_samples, float *min, float *max)
{
	uint32_t n;
	float t, mi = *min, ma = *max;
	for (n = 0; n < n_samples; n++) {
		t = src[n];
		mi = fminf(mi, t);
		ma = fmaxf(ma, t);
	}
	*min = mi;
	*max = ma;
}

float peaks_abs_max_c(struct peaks *peaks, const float * SPA_RESTRICT src,
		uint32_t n_samples, float max)
{
	uint32_t n;
	for (n = 0; n < n_samples; n++)
		max = fmaxf(fabsf(src[n]), max);
	return max;
}
