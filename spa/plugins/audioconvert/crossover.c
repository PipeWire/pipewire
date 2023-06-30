/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <float.h>
#include <string.h>

#include "crossover.h"

void lr4_set(struct lr4 *lr4, enum biquad_type type, float freq)
{
	biquad_set(&lr4->bq, type, freq);
	lr4->x1 = 0;
	lr4->x2 = 0;
	lr4->y1 = 0;
	lr4->y2 = 0;
	lr4->z1 = 0;
	lr4->z2 = 0;
	lr4->active = true;
}

void lr4_process(struct lr4 *lr4, float *dst, const float *src, const float vol, int samples)
{
	float x1 = lr4->x1;
	float x2 = lr4->x2;
	float y1 = lr4->y1;
	float y2 = lr4->y2;
	float b0 = lr4->bq.b0;
	float b1 = lr4->bq.b1;
	float b2 = lr4->bq.b2;
	float a1 = lr4->bq.a1;
	float a2 = lr4->bq.a2;
	float x, y, z;
	int i;

	if (vol == 0.0f) {
		memset(dst, 0, samples * sizeof(float));
		return;
	} else if (!lr4->active) {
		if (src != dst || vol != 1.0f) {
			for (i = 0; i < samples; i++)
				dst[i] = src[i] * vol;
		}
		return;
	}

	for (i = 0; i < samples; i++) {
		x  = src[i];
		y  = b0 * x          + x1;
		x1 = b1 * x - a1 * y + x2;
		x2 = b2 * x - a2 * y;
		z  = b0 * y          + y1;
		y1 = b1 * y - a1 * z + y2;
		y2 = b2 * y - a2 * z;
		dst[i] = z * vol;
	}
#define F(x) (-FLT_MIN < (x) && (x) < FLT_MIN ? 0.0f : (x))
	lr4->x1 = F(x1);
	lr4->x2 = F(x2);
	lr4->y1 = F(y1);
	lr4->y2 = F(y2);
#undef F
}
