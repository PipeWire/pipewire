/* Spa
 *
 * Copyright © 2018 Wim Taymans
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

#include <math.h>

#include <xmmintrin.h>

#include "resample-peaks-impl.h"

static inline float hmax_ps(__m128 val)
{
	__m128 t = _mm_movehl_ps(val, val);
	t = _mm_max_ps(t, val);
	val = _mm_shuffle_ps(t, t, 0x55);
	val = _mm_max_ss(t, val);
	return _mm_cvtss_f32(val);
}

static inline float find_abs_max_sse(const float *s, uint32_t n_samples, float m)
{
	__m128 in[2], max;
	uint32_t n, unrolled;
	const __m128 mask = _mm_set1_ps(-0.0f);

	max = _mm_set1_ps(m);

	unrolled = n_samples & ~7;

	for (n = 0; n < unrolled; n += 8) {
		in[0] = _mm_loadu_ps(&s[n + 0]);
		in[1] = _mm_loadu_ps(&s[n + 4]);
		in[0] = _mm_andnot_ps(mask, in[0]);
		in[1] = _mm_andnot_ps(mask, in[1]);
		max = _mm_max_ps(max, in[0]);
		max = _mm_max_ps(max, in[1]);
	}
	for (; n < n_samples; n++)
		m = fmaxf(fabsf(s[n]), m);

	return fmaxf(hmax_ps(max), m);
}

MAKE_PEAKS(sse);
