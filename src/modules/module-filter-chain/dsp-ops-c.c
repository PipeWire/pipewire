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

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>

#include <spa/utils/defs.h>

#include "dsp-ops.h"

void dsp_clear_c(struct dsp_ops *ops, void * SPA_RESTRICT dst, uint32_t n_samples)
{
	memset(dst, 0, sizeof(float) * n_samples);
}

static inline void dsp_add_c(struct dsp_ops *ops, void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src, uint32_t n_samples)
{
	uint32_t i;
	const float *s = src;
	float *d = dst;
	for (i = 0; i < n_samples; i++)
		d[i] += s[i];
}

static inline void dsp_gain_c(struct dsp_ops *ops, void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src, float gain, uint32_t n_samples)
{
	uint32_t i;
	const float *s = src;
	float *d = dst;
	for (i = 0; i < n_samples; i++)
		d[i] = s[i] * gain;
}

static inline void dsp_gain_add_c(struct dsp_ops *ops, void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src, float gain, uint32_t n_samples)
{
	uint32_t i;
	const float *s = src;
	float *d = dst;
	for (i = 0; i < n_samples; i++)
		d[i] += s[i] * gain;
}


void dsp_copy_c(struct dsp_ops *ops, void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src, uint32_t n_samples)
{
	if (dst != src)
		spa_memcpy(dst, src, sizeof(float) * n_samples);
}

void dsp_mix_gain_c(struct dsp_ops *ops,
		void * SPA_RESTRICT dst,
		const void * SPA_RESTRICT src[],
		float gain[], uint32_t n_src, uint32_t n_samples)
{
	uint32_t i;
	if (n_src == 0) {
		dsp_clear_c(ops, dst, n_samples);
	} else if (n_src == 1) {
		if (dst != src[0])
			dsp_copy_c(ops, dst, src[0], n_samples);
	} else {
		if (gain[0] == 1.0f)
			dsp_copy_c(ops, dst, src[0], n_samples);
		else
			dsp_gain_c(ops, dst, src[0], gain[0], n_samples);

		for (i = 1; i < n_src; i++) {
			if (gain[i] == 1.0f)
				dsp_add_c(ops, dst, src[i], n_samples);
			else
				dsp_gain_add_c(ops, dst, src[i], gain[i], n_samples);
		}
	}
}

void dsp_biquad_run_c(struct dsp_ops *ops, struct biquad *bq,
		float *out, const float *in, uint32_t n_samples)
{
	float x1, x2, y1, y2;
	float b0, b1, b2, a1, a2;
	uint32_t i;

	x1 = bq->x1;
	x2 = bq->x2;
	y1 = bq->y1;
	y2 = bq->y2;
	b0 = bq->b0;
	b1 = bq->b1;
	b2 = bq->b2;
	a1 = bq->a1;
	a2 = bq->a2;
	for (i = 0; i < n_samples; i++) {
		float x = in[i];
		float y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
		out[i] = y;
		x2 = x1;
		x1 = x;
		y2 = y1;
		y1 = y;
	}
#define F(x) (-FLT_MIN < (x) && (x) < FLT_MIN ? 0.0f : (x))
	bq->x1 = F(x1);
	bq->x2 = F(x2);
	bq->y1 = F(y1);
	bq->y2 = F(y2);
#undef F
}

void dsp_sum_c(struct dsp_ops *ops, float * dst,
		const float * SPA_RESTRICT a, const float * SPA_RESTRICT b, uint32_t n_samples)
{
	uint32_t i;
	for (i = 0; i < n_samples; i++)
		dst[i] = a[i] + b[i];
}

