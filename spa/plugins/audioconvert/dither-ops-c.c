/* Spa
 *
 * Copyright © 2022 Wim Taymans
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

#include "dither-ops.h"


/* 32 bit xorshift PRNG, see https://en.wikipedia.org/wiki/Xorshift */
static inline uint32_t
xorshift(uint32_t *state)
{
  uint32_t x = *state;
  x ^= x << 13;
  x ^= x >> 17;
  x ^= x << 5;
  return (*state = x);
}

static inline void update_dither_c(struct dither *dt, uint32_t n_samples)
{
	uint32_t n;
	for (n = 0; n < n_samples; n++)
		dt->dither[n] = ((int32_t)xorshift(&dt->random[0])) * dt->scale;
}

void dither_f32_c(struct dither *dt, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n, m, chunk;
	const float **s = (const float**)src;
	float **d = (float**)dst;
	float *dither = dt->dither;

	chunk = SPA_MIN(n_samples, dt->dither_size);
	update_dither_c(dt, chunk);

	for (n = 0; n < n_samples; n += chunk) {
		chunk = SPA_MIN(n_samples - n, dt->dither_size);

		for (i = 0; i < dt->n_channels; i++) {
			float *di = &d[i][n];
			const float *si = &s[i][n];

			for (m = 0; m < chunk; m++)
				di[m] = si[m] + dither[m];
		}
	}
}
