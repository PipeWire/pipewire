/* Hilbert function
 *
 * Copyright © 2021 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef HILBERT_H
#define HILBERT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <errno.h>
#include <stddef.h>
#include <math.h>

static inline void blackman_window(float *taps, int n_taps)
{
	int n;
	for (n = 0; n < n_taps; n++) {
		float w = 2 * M_PI * n / (n_taps-1);
		taps[n] = 0.3635819 - 0.4891775 * cos(w)
			+ 0.1365995 * cos(2 * w) - 0.0106411 * cos(3 * w);
	}
}

static inline int hilbert_generate(float *taps, int n_taps)
{
	int i;

	if ((n_taps & 1) == 0)
		return -EINVAL;

	for (i = 0; i < n_taps; i++) {
		int k = -(n_taps / 2) + i;
		if (k & 1) {
			float pk = M_PI * k;
			taps[i] *= (1.0f - cosf(pk)) / pk;
		} else {
			taps[i] = 0.0f;
		}
	}
	return 0;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* HILBERT_H */
