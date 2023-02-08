/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "volume-ops.h"

void
volume_f32_c(struct volume *vol, void * SPA_RESTRICT dst,
		const void * SPA_RESTRICT src, float volume, uint32_t n_samples)
{
	uint32_t n;
	float *d = (float*)dst;
	const float *s = (const float*)src;

	if (volume == VOLUME_MIN) {
		memset(d, 0, n_samples * sizeof(float));
	}
	else if (volume == VOLUME_NORM) {
		spa_memcpy(d, s, n_samples * sizeof(float));
	}
	else {
		for (n = 0; n < n_samples; n++)
			d[n] = s[n] * volume;
	}
}
