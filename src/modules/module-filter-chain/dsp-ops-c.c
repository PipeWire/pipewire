/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>

#include <spa/utils/defs.h>

#include "pffft.h"
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
	float x, y, x1, x2;
	float b0, b1, b2, a1, a2;
	uint32_t i;

	x1 = bq->x1;
	x2 = bq->x2;
	b0 = bq->b0;
	b1 = bq->b1;
	b2 = bq->b2;
	a1 = bq->a1;
	a2 = bq->a2;
	for (i = 0; i < n_samples; i++) {
		x  = in[i];
		y  = b0 * x          + x1;
		x1 = b1 * x - a1 * y + x2;
		x2 = b2 * x - a2 * y;
		out[i] = y;
	}
#define F(x) (-FLT_MIN < (x) && (x) < FLT_MIN ? 0.0f : (x))
	bq->x1 = F(x1);
	bq->x2 = F(x2);
#undef F
}

void dsp_sum_c(struct dsp_ops *ops, float * dst,
		const float * SPA_RESTRICT a, const float * SPA_RESTRICT b, uint32_t n_samples)
{
	uint32_t i;
	for (i = 0; i < n_samples; i++)
		dst[i] = a[i] + b[i];
}

void *dsp_fft_new_c(struct dsp_ops *ops, int32_t size, bool real)
{
	return pffft_new_setup(size, real ? PFFFT_REAL : PFFFT_COMPLEX);
}

void dsp_fft_free_c(struct dsp_ops *ops, void *fft)
{
	pffft_destroy_setup(fft);
}
void dsp_fft_run_c(struct dsp_ops *ops, void *fft, int direction,
	const float * SPA_RESTRICT src, float * SPA_RESTRICT dst)
{
	pffft_transform(fft, src, dst, NULL, direction < 0 ? PFFFT_BACKWARD : PFFFT_FORWARD);
}

void dsp_fft_cmul_c(struct dsp_ops *ops, void *fft,
	float * SPA_RESTRICT dst, const float * SPA_RESTRICT a,
	const float * SPA_RESTRICT b, uint32_t len, const float scale)
{
	pffft_zconvolve(fft, a, b, dst, scale);
}

void dsp_fft_cmuladd_c(struct dsp_ops *ops, void *fft,
	float * SPA_RESTRICT dst, const float * SPA_RESTRICT src,
	const float * SPA_RESTRICT a, const float * SPA_RESTRICT b,
	uint32_t len, const float scale)
{
	pffft_zconvolve_accumulate(fft, a, b, src, dst, scale);
}

