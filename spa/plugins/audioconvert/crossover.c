/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <float.h>
#include <string.h>

#include "crossover.h"

void lr4_set(struct lr4 *lr4, enum biquad_type type, float freq)
{
	biquad_set(&lr4->bq, type, freq, 0, 0);
	lr4->x1 = 0;
	lr4->x2 = 0;
	lr4->y1 = 0;
	lr4->y2 = 0;
	lr4->active = type != BQ_NONE;
}
