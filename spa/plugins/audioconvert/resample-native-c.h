/* Spa
 *
 * Copyright © 2019 Wim Taymans
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

static void inner_product_c(float *d, const float * SPA_RESTRICT s,
		const float * SPA_RESTRICT taps, uint32_t n_taps)
{
	float sum = 0.0f;
	uint32_t i;

	for (i = 0; i < n_taps; i++)
		sum += s[i] * taps[i];
	*d = sum;
}

static void inner_product_ip_c(float *d, const float * SPA_RESTRICT s,
	const float * SPA_RESTRICT t0, const float * SPA_RESTRICT t1, float x,
	uint32_t n_taps)
{
	float sum[2] = { 0.0f, 0.0f };
	uint32_t i;

	for (i = 0; i < n_taps; i++) {
		sum[0] += s[i] * t0[i];
		sum[1] += s[i] * t1[i];
	}
	*d = (sum[1] - sum[0]) * x + sum[0];
}

MAKE_RESAMPLER_COPY(c);
MAKE_RESAMPLER_FULL(c);
MAKE_RESAMPLER_INTER(c);
