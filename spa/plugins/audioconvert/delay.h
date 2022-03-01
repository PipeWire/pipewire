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

#ifndef DELAY_H
#define DELAY_H

#ifdef __cplusplus
extern "C" {
#endif

static inline void delay_run(float *buffer, uint32_t *pos,
		uint32_t n_buffer, uint32_t delay,
		float *dst, const float *src, const float vol, uint32_t n_samples)
{
	uint32_t i;
	uint32_t p = *pos;

	for (i = 0; i < n_samples; i++) {
		buffer[p] = src[i];
		dst[i] = buffer[(p - delay) & (n_buffer-1)] * vol;
		p = (p + 1) & (n_buffer-1);
	}
	*pos = p;
}

static inline void delay_convolve_run(float *buffer, uint32_t *pos,
		uint32_t n_buffer, uint32_t delay,
		const float *taps, uint32_t n_taps,
		float *dst, const float *src, const float vol, uint32_t n_samples)
{
	uint32_t i, j;
	uint32_t p = *pos;

	for (i = 0; i < n_samples; i++) {
		float sum = 0.0f;

		buffer[p] = src[i];
		for (j = 0; j < n_taps; j++)
			sum += (taps[j] * buffer[((p - delay) - j) & (n_buffer-1)]);
		dst[i] = sum * vol;

		p = (p + 1) & (n_buffer-1);
	}
	*pos = p;
}

#ifdef __cplusplus
}
#endif

#endif /* DELAY_H */
