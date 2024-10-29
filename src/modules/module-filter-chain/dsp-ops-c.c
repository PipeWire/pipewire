/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <errno.h>

#include <spa/utils/defs.h>

#include "config.h"
#ifdef HAVE_FFTW
#include <fftw3.h>
#else
#include "pffft.h"
#endif
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
	if (gain == 0.0f)
		dsp_clear_c(ops, dst, n_samples);
	else if (gain == 1.0f)
		dsp_copy_c(ops, dst, src, n_samples);
	else  {
		for (i = 0; i < n_samples; i++)
			d[i] = s[i] * gain;
	}
}

static inline void dsp_gain_add_c(struct dsp_ops *ops, void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src, float gain, uint32_t n_samples)
{
	uint32_t i;
	const float *s = src;
	float *d = dst;

	if (gain == 0.0f)
		return;
	else if (gain == 1.0f)
		dsp_add_c(ops, dst, src, n_samples);
	else {
		for (i = 0; i < n_samples; i++)
			d[i] += s[i] * gain;
	}
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
	} else {
		dsp_gain_c(ops, dst, src[0], gain[0], n_samples);
		for (i = 1; i < n_src; i++)
			dsp_gain_add_c(ops, dst, src[i], gain[i], n_samples);
	}
}

static inline void dsp_mult1_c(struct dsp_ops *ops, void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src, uint32_t n_samples)
{
	uint32_t i;
	const float *s = src;
	float *d = dst;
	for (i = 0; i < n_samples; i++)
		d[i] *= s[i];
}

void dsp_mult_c(struct dsp_ops *ops,
		void * SPA_RESTRICT dst,
		const void * SPA_RESTRICT src[],
		uint32_t n_src, uint32_t n_samples)
{
	uint32_t i;
	if (n_src == 0) {
		dsp_clear_c(ops, dst, n_samples);
	} else {
		dsp_copy_c(ops, dst, src[0], n_samples);
		for (i = 1; i < n_src; i++)
			dsp_mult1_c(ops, dst, src[i], n_samples);
	}
}

void dsp_biquad_run_c(struct dsp_ops *ops, struct biquad *bq,
		float *out, const float *in, uint32_t n_samples)
{
	float x, y, x1, x2;
	float b0, b1, b2, a1, a2;
	uint32_t i;

	if (bq->type == BQ_NONE) {
		dsp_copy_c(ops, out, in, n_samples);
		return;
	}

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

void dsp_biquadn_run_c(struct dsp_ops *ops, struct biquad *bq, uint32_t n_bq, uint32_t bq_stride,
		float * SPA_RESTRICT out[], const float * SPA_RESTRICT in[],
		uint32_t n_src, uint32_t n_samples)
{
	uint32_t i, j;
	const float *s;
	float *d;
	for (i = 0; i < n_src; i++, bq+=bq_stride) {
		s = in[i];
		d = out[i];
		if (s == NULL || d == NULL)
			continue;
		for (j = 0; j < n_bq; j++) {
			dsp_biquad_run_c(ops, &bq[j], d, s, n_samples);
			s = d;
		}
	}
}

void dsp_sum_c(struct dsp_ops *ops, float * dst,
		const float * SPA_RESTRICT a, const float * SPA_RESTRICT b, uint32_t n_samples)
{
	uint32_t i;
	for (i = 0; i < n_samples; i++)
		dst[i] = a[i] + b[i];
}

void dsp_linear_c(struct dsp_ops *ops, float * dst,
		const float * SPA_RESTRICT src, const float mult,
		const float add, uint32_t n_samples)
{
	uint32_t i;
	if (add == 0.0f) {
		dsp_gain_c(ops, dst, src, mult, n_samples);
	} else {
		if (mult == 0.0f) {
			for (i = 0; i < n_samples; i++)
				dst[i] = add;
		} else if (mult == 1.0f) {
			for (i = 0; i < n_samples; i++)
				dst[i] = src[i] + add;
		} else {
			for (i = 0; i < n_samples; i++)
				dst[i] = mult * src[i] + add;
		}
	}
}


void dsp_delay_c(struct dsp_ops *ops, float *buffer, uint32_t *pos, uint32_t n_buffer,
		uint32_t delay, float *dst, const float *src, uint32_t n_samples)
{
	if (delay == 0) {
		dsp_copy_c(ops, dst, src, n_samples);
	} else {
		uint32_t w, o, i;

		w = *pos;
		o = n_buffer - delay;

		for (i = 0; i < n_samples; i++) {
			buffer[w] = buffer[w + n_buffer] = src[i];
			dst[i] = buffer[w + o];
			w = w + 1 > n_buffer ? 0 : w + 1;
		}
		*pos = w;
	}
}

#ifdef HAVE_FFTW
struct fft_info {
	fftwf_plan plan_r2c;
	fftwf_plan plan_c2r;
};
#endif

void *dsp_fft_new_c(struct dsp_ops *ops, int32_t size, bool real)
{
#ifdef HAVE_FFTW
	struct fft_info *info = calloc(1, sizeof(struct fft_info));
	float *rdata;
	fftwf_complex *cdata;

	if (info == NULL)
		return NULL;

	rdata = fftwf_alloc_real (size * 2);
	cdata = fftwf_alloc_complex (size + 1);

	info->plan_r2c = fftwf_plan_dft_r2c_1d(size, rdata, cdata, FFTW_ESTIMATE);
	info->plan_c2r = fftwf_plan_dft_c2r_1d(size, cdata, rdata, FFTW_ESTIMATE);

	fftwf_free(rdata);
	fftwf_free(cdata);

	return info;
#else
	return pffft_new_setup(size, real ? PFFFT_REAL : PFFFT_COMPLEX);
#endif
}

void dsp_fft_free_c(struct dsp_ops *ops, void *fft)
{
#ifdef HAVE_FFTW
	struct fft_info *info = fft;
	fftwf_destroy_plan(info->plan_r2c);
	fftwf_destroy_plan(info->plan_c2r);
	free(info);
#else
	pffft_destroy_setup(fft);
#endif
}
void dsp_fft_run_c(struct dsp_ops *ops, void *fft, int direction,
	const float * SPA_RESTRICT src, float * SPA_RESTRICT dst)
{
#ifdef HAVE_FFTW
	struct fft_info *info = fft;
	if (direction > 0)
		fftwf_execute_dft_r2c (info->plan_r2c, (float*)src, (fftwf_complex*)dst);
	else
		fftwf_execute_dft_c2r (info->plan_c2r, (fftwf_complex*)src, dst);
#else
	pffft_transform(fft, src, dst, NULL, direction < 0 ? PFFFT_BACKWARD : PFFFT_FORWARD);
#endif
}

void dsp_fft_cmul_c(struct dsp_ops *ops, void *fft,
	float * SPA_RESTRICT dst, const float * SPA_RESTRICT a,
	const float * SPA_RESTRICT b, uint32_t len, const float scale)
{
#ifdef HAVE_FFTW
	for (uint32_t i = 0; i < len; i++) {
		dst[2*i  ] = (a[2*i] * b[2*i  ] - a[2*i+1] * b[2*i+1]) * scale;
		dst[2*i+1] = (a[2*i] * b[2*i+1] + a[2*i+1] * b[2*i  ]) * scale;
	}
#else
	pffft_zconvolve(fft, a, b, dst, scale);
#endif
}

void dsp_fft_cmuladd_c(struct dsp_ops *ops, void *fft,
	float * SPA_RESTRICT dst, const float * SPA_RESTRICT src,
	const float * SPA_RESTRICT a, const float * SPA_RESTRICT b,
	uint32_t len, const float scale)
{
#ifdef HAVE_FFTW
	for (uint32_t i = 0; i < len; i++) {
		dst[2*i  ] = src[2*i  ] + (a[2*i] * b[2*i  ] - a[2*i+1] * b[2*i+1]) * scale;
		dst[2*i+1] = src[2*i+1] + (a[2*i] * b[2*i+1] + a[2*i+1] * b[2*i  ]) * scale;
	}
#else
	pffft_zconvolve_accumulate(fft, a, b, src, dst, scale);
#endif
}

