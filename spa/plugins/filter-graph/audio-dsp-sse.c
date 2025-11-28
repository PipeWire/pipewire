/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>
#include <complex.h>

#include <spa/utils/defs.h>

#ifndef HAVE_FFTW
#include "pffft.h"
#endif

#include "audio-dsp-impl.h"

#include <xmmintrin.h>

static void dsp_add_sse(void *obj, float *dst, const float * SPA_RESTRICT src[],
		uint32_t n_src, uint32_t n_samples)
{
	uint32_t n, i, unrolled;
	__m128 in[4];
	const float **s = (const float **)src;
	float *d = dst;

	if (SPA_LIKELY(SPA_IS_ALIGNED(dst, 16))) {
		unrolled = n_samples & ~15;
		for (i = 0; i < n_src; i++) {
			if (SPA_UNLIKELY(!SPA_IS_ALIGNED(src[i], 16))) {
				unrolled = 0;
				break;
			}
		}
	} else
		unrolled = 0;

	for (n = 0; n < unrolled; n += 16) {
		in[0] = _mm_load_ps(&s[0][n+ 0]);
		in[1] = _mm_load_ps(&s[0][n+ 4]);
		in[2] = _mm_load_ps(&s[0][n+ 8]);
		in[3] = _mm_load_ps(&s[0][n+12]);

		for (i = 1; i < n_src; i++) {
			in[0] = _mm_add_ps(in[0], _mm_load_ps(&s[i][n+ 0]));
			in[1] = _mm_add_ps(in[1], _mm_load_ps(&s[i][n+ 4]));
			in[2] = _mm_add_ps(in[2], _mm_load_ps(&s[i][n+ 8]));
			in[3] = _mm_add_ps(in[3], _mm_load_ps(&s[i][n+12]));
		}
		_mm_store_ps(&d[n+ 0], in[0]);
		_mm_store_ps(&d[n+ 4], in[1]);
		_mm_store_ps(&d[n+ 8], in[2]);
		_mm_store_ps(&d[n+12], in[3]);
	}
	for (; n < n_samples; n++) {
		in[0] = _mm_load_ss(&s[0][n]);
		for (i = 1; i < n_src; i++)
			in[0] = _mm_add_ss(in[0], _mm_load_ss(&s[i][n]));
		_mm_store_ss(&d[n], in[0]);
	}
}

static void dsp_add_1_gain_sse(void *obj,
		float * SPA_RESTRICT dst,
		const float * SPA_RESTRICT src[], uint32_t n_src,
		float gain, uint32_t n_samples)
{
	uint32_t n, i, unrolled;
	__m128 in[4], g;
	const float **s = (const float **)src;
	float *d = dst;

	if (SPA_LIKELY(SPA_IS_ALIGNED(dst, 16))) {
		unrolled = n_samples & ~15;
		for (i = 0; i < n_src; i++) {
			if (SPA_UNLIKELY(!SPA_IS_ALIGNED(src[i], 16))) {
				unrolled = 0;
				break;
			}
		}
	} else
		unrolled = 0;

	g = _mm_set1_ps(gain);

	for (n = 0; n < unrolled; n += 16) {
		in[0] = _mm_load_ps(&s[0][n+ 0]);
		in[1] = _mm_load_ps(&s[0][n+ 4]);
		in[2] = _mm_load_ps(&s[0][n+ 8]);
		in[3] = _mm_load_ps(&s[0][n+12]);

		for (i = 1; i < n_src; i++) {
			in[0] = _mm_add_ps(in[0], _mm_load_ps(&s[i][n+ 0]));
			in[1] = _mm_add_ps(in[1], _mm_load_ps(&s[i][n+ 4]));
			in[2] = _mm_add_ps(in[2], _mm_load_ps(&s[i][n+ 8]));
			in[3] = _mm_add_ps(in[3], _mm_load_ps(&s[i][n+12]));
		}
		_mm_store_ps(&d[n+ 0], _mm_mul_ps(in[0], g));
		_mm_store_ps(&d[n+ 4], _mm_mul_ps(in[1], g));
		_mm_store_ps(&d[n+ 8], _mm_mul_ps(in[2], g));
		_mm_store_ps(&d[n+12], _mm_mul_ps(in[3], g));
	}
	for (; n < n_samples; n++) {
		in[0] = _mm_load_ss(&s[0][n]);
		for (i = 1; i < n_src; i++)
			in[0] = _mm_add_ss(in[0], _mm_load_ss(&s[i][n]));
		_mm_store_ss(&d[n], _mm_mul_ss(in[0], g));
	}
}

static void dsp_add_n_gain_sse(void *obj,
		float * SPA_RESTRICT dst,
		const float * SPA_RESTRICT src[], uint32_t n_src,
		float gain[], uint32_t n_gain, uint32_t n_samples)
{
	uint32_t n, i, unrolled;
	__m128 in[4], g;
	const float **s = (const float **)src;
	float *d = dst;

	if (SPA_LIKELY(SPA_IS_ALIGNED(dst, 16))) {
		unrolled = n_samples & ~15;
		for (i = 0; i < n_src; i++) {
			if (SPA_UNLIKELY(!SPA_IS_ALIGNED(src[i], 16))) {
				unrolled = 0;
				break;
			}
		}
	} else
		unrolled = 0;

	for (n = 0; n < unrolled; n += 16) {
		g = _mm_set1_ps(gain[0]);
		in[0] = _mm_mul_ps(g, _mm_load_ps(&s[0][n+ 0]));
		in[1] = _mm_mul_ps(g, _mm_load_ps(&s[0][n+ 4]));
		in[2] = _mm_mul_ps(g, _mm_load_ps(&s[0][n+ 8]));
		in[3] = _mm_mul_ps(g, _mm_load_ps(&s[0][n+12]));

		for (i = 1; i < n_src; i++) {
			g = _mm_set1_ps(gain[i]);
			in[0] = _mm_add_ps(in[0], _mm_mul_ps(g, _mm_load_ps(&s[i][n+ 0])));
			in[1] = _mm_add_ps(in[1], _mm_mul_ps(g, _mm_load_ps(&s[i][n+ 4])));
			in[2] = _mm_add_ps(in[2], _mm_mul_ps(g, _mm_load_ps(&s[i][n+ 8])));
			in[3] = _mm_add_ps(in[3], _mm_mul_ps(g, _mm_load_ps(&s[i][n+12])));
		}
		_mm_store_ps(&d[n+ 0], in[0]);
		_mm_store_ps(&d[n+ 4], in[1]);
		_mm_store_ps(&d[n+ 8], in[2]);
		_mm_store_ps(&d[n+12], in[3]);
	}
	for (; n < n_samples; n++) {
		g = _mm_set_ss(gain[0]);
		in[0] = _mm_mul_ss(g, _mm_load_ss(&s[0][n]));
		for (i = 1; i < n_src; i++) {
			g = _mm_set_ss(gain[i]);
			in[0] = _mm_add_ss(in[0], _mm_mul_ss(g, _mm_load_ss(&s[i][n])));
		}
		_mm_store_ss(&d[n], in[0]);
	}
}

void dsp_mix_gain_sse(void *obj,
		float * SPA_RESTRICT dst,
		const float * SPA_RESTRICT src[], uint32_t n_src,
		float gain[], uint32_t n_gain, uint32_t n_samples)
{
	if (n_src == 0) {
		memset(dst, 0, n_samples * sizeof(float));
	} else if (n_src == 1 && gain[0] == 1.0f) {
		if (dst != src[0])
			spa_memcpy(dst, src[0], n_samples * sizeof(float));
	} else {
		if (n_gain == 0)
			dsp_add_sse(obj, dst, src, n_src, n_samples);
		else if (n_gain < n_src)
			dsp_add_1_gain_sse(obj, dst, src, n_src, gain[0], n_samples);
		else
			dsp_add_n_gain_sse(obj, dst, src, n_src, gain, n_gain, n_samples);
	}
}

void dsp_sum_sse(void *obj, float *r, const float *a, const float *b, uint32_t n_samples)
{
	uint32_t n, unrolled;
	__m128 in[4];

	unrolled = n_samples & ~15;

	if (SPA_LIKELY(SPA_IS_ALIGNED(r, 16)) &&
	    SPA_LIKELY(SPA_IS_ALIGNED(a, 16)) &&
	    SPA_LIKELY(SPA_IS_ALIGNED(b, 16))) {
		for (n = 0; n < unrolled; n += 16) {
			in[0] = _mm_load_ps(&a[n+ 0]);
			in[1] = _mm_load_ps(&a[n+ 4]);
			in[2] = _mm_load_ps(&a[n+ 8]);
			in[3] = _mm_load_ps(&a[n+12]);

			in[0] = _mm_add_ps(in[0], _mm_load_ps(&b[n+ 0]));
			in[1] = _mm_add_ps(in[1], _mm_load_ps(&b[n+ 4]));
			in[2] = _mm_add_ps(in[2], _mm_load_ps(&b[n+ 8]));
			in[3] = _mm_add_ps(in[3], _mm_load_ps(&b[n+12]));

			_mm_store_ps(&r[n+ 0], in[0]);
			_mm_store_ps(&r[n+ 4], in[1]);
			_mm_store_ps(&r[n+ 8], in[2]);
			_mm_store_ps(&r[n+12], in[3]);
		}
	} else {
		for (n = 0; n < unrolled; n += 16) {
			in[0] = _mm_loadu_ps(&a[n+ 0]);
			in[1] = _mm_loadu_ps(&a[n+ 4]);
			in[2] = _mm_loadu_ps(&a[n+ 8]);
			in[3] = _mm_loadu_ps(&a[n+12]);

			in[0] = _mm_add_ps(in[0], _mm_loadu_ps(&b[n+ 0]));
			in[1] = _mm_add_ps(in[1], _mm_loadu_ps(&b[n+ 4]));
			in[2] = _mm_add_ps(in[2], _mm_loadu_ps(&b[n+ 8]));
			in[3] = _mm_add_ps(in[3], _mm_loadu_ps(&b[n+12]));

			_mm_storeu_ps(&r[n+ 0], in[0]);
			_mm_storeu_ps(&r[n+ 4], in[1]);
			_mm_storeu_ps(&r[n+ 8], in[2]);
			_mm_storeu_ps(&r[n+12], in[3]);
		}
	}
	for (; n < n_samples; n++) {
		in[0] = _mm_load_ss(&a[n]);
		in[0] = _mm_add_ss(in[0], _mm_load_ss(&b[n]));
		_mm_store_ss(&r[n], in[0]);
	}
}

static void dsp_biquad_run1_sse(void *obj, struct biquad *bq,
		float *out, const float *in, uint32_t n_samples)
{
	__m128 x, y, z;
	__m128 b012;
	__m128 a12;
	__m128 x12;
	uint32_t i;

	b012 = _mm_setr_ps(bq->b0, bq->b1, bq->b2, 0.0f);  /* b0  b1  b2  0 */
	a12 = _mm_setr_ps(0.0f, bq->a1, bq->a2, 0.0f);	  /* 0   a1  a2  0 */
	x12 = _mm_setr_ps(bq->x1, bq->x2, 0.0f, 0.0f);	  /* x1  x2  0   0 */

	for (i = 0; i < n_samples; i++) {
		x = _mm_load1_ps(&in[i]);		/*  x         x         x      x */
		z = _mm_mul_ps(x, b012);		/*  b0*x      b1*x      b2*x   0 */
		z = _mm_add_ps(z, x12); 		/*  b0*x+x1   b1*x+x2   b2*x   0 */
		_mm_store_ss(&out[i], z);		/*  out[i] = b0*x+x1 */
		y = _mm_shuffle_ps(z, z, _MM_SHUFFLE(0,0,0,0));	/*  b0*x+x1  b0*x+x1  b0*x+x1  b0*x+x1 = y*/
		y = _mm_mul_ps(y, a12);		        /*  0        a1*y     a2*y     0 */
		y = _mm_sub_ps(z, y);	 		/*  y        x1       x2       0 */
		x12 = _mm_shuffle_ps(y, y, _MM_SHUFFLE(3,3,2,1));    /*  x1  x2  0  0*/
	}
#define F(x) (isnormal(x) ? (x) : 0.0f)
	bq->x1 = F(x12[0]);
	bq->x2 = F(x12[1]);
#undef F
}

static void dsp_biquad2_run_sse(void *obj, struct biquad *bq,
		float *out, const float *in, uint32_t n_samples)
{
	__m128 x, y, z;
	__m128 b0, b1;
	__m128 a0, a1;
	__m128 x0, x1;
	uint32_t i;

	b0 = _mm_setr_ps(bq[0].b0, bq[0].b1, bq[0].b2, 0.0f);  /* b0  b1  b2  0 */
	a0 = _mm_setr_ps(0.0f, bq[0].a1, bq[0].a2, 0.0f);	    /* 0   a1  a2  0 */
	x0 = _mm_setr_ps(bq[0].x1, bq[0].x2, 0.0f, 0.0f);	    /* x1  x2  0   0 */

	b1 = _mm_setr_ps(bq[1].b0, bq[1].b1, bq[1].b2, 0.0f);  /* b0  b1  b2  0 */
	a1 = _mm_setr_ps(0.0f, bq[1].a1, bq[1].a2, 0.0f);	    /* 0   a1  a2  0 */
	x1 = _mm_setr_ps(bq[1].x1, bq[1].x2, 0.0f, 0.0f);	    /* x1  x2  0   0 */

	for (i = 0; i < n_samples; i++) {
		x = _mm_load1_ps(&in[i]);			/*  x         x         x      x */

		z = _mm_mul_ps(x, b0);				/*  b0*x      b1*x      b2*x   0 */
		z = _mm_add_ps(z, x0); 				/*  b0*x+x1   b1*x+x2   b2*x   0 */
		y = _mm_shuffle_ps(z, z, _MM_SHUFFLE(0,0,0,0));	/*  b0*x+x1  b0*x+x1  b0*x+x1  b0*x+x1 = y*/
		x = _mm_mul_ps(y, a0);			        /*  0        a1*y     a2*y     0 */
		x = _mm_sub_ps(z, x);	 			/*  y        x1       x2       0 */
		x0 = _mm_shuffle_ps(x, x, _MM_SHUFFLE(3,3,2,1));    /*  x1  x2  0  0*/

		z = _mm_mul_ps(y, b1);				/*  b0*x      b1*x      b2*x   0 */
		z = _mm_add_ps(z, x1); 				/*  b0*x+x1   b1*x+x2   b2*x   0 */
		x = _mm_shuffle_ps(z, z, _MM_SHUFFLE(0,0,0,0));	/*  b0*x+x1  b0*x+x1  b0*x+x1  b0*x+x1 = y*/
		y = _mm_mul_ps(x, a1);			        /*  0        a1*y     a2*y     0 */
		y = _mm_sub_ps(z, y);	 			/*  y        x1       x2       0 */
		x1 = _mm_shuffle_ps(y, y, _MM_SHUFFLE(3,3,2,1));    /*  x1  x2  0  0*/

		_mm_store_ss(&out[i], x);			/*  out[i] = b0*x+x1 */
	}
#define F(x) (isnormal(x) ? (x) : 0.0f)
	bq[0].x1 = F(x0[0]);
	bq[0].x2 = F(x0[1]);
	bq[1].x1 = F(x1[0]);
	bq[1].x2 = F(x1[1]);
#undef F
}

static void dsp_biquad_run2_sse(void *obj, struct biquad *bq, uint32_t bq_stride,
		float **out, const float **in, uint32_t n_samples)
{
	__m128 x, y, z;
	__m128 b0, b1, b2;
	__m128 a1, a2;
	__m128 x1, x2;
	uint32_t i;

	b0 = _mm_setr_ps(bq[0].b0, bq[bq_stride].b0, 0.0f, 0.0f);  /* b00  b10  0  0 */
	b1 = _mm_setr_ps(bq[0].b1, bq[bq_stride].b1, 0.0f, 0.0f);  /* b01  b11  0  0 */
	b2 = _mm_setr_ps(bq[0].b2, bq[bq_stride].b2, 0.0f, 0.0f);  /* b02  b12  0  0 */
	a1 = _mm_setr_ps(bq[0].a1, bq[bq_stride].a1, 0.0f, 0.0f);  /* b00  b10  0  0 */
	a2 = _mm_setr_ps(bq[0].a2, bq[bq_stride].a2, 0.0f, 0.0f);  /* b01  b11  0  0 */
	x1 = _mm_setr_ps(bq[0].x1, bq[bq_stride].x1, 0.0f, 0.0f);  /* b00  b10  0  0 */
	x2 = _mm_setr_ps(bq[0].x2, bq[bq_stride].x2, 0.0f, 0.0f);  /* b01  b11  0  0 */

	for (i = 0; i < n_samples; i++) {
		x = _mm_setr_ps(in[0][i], in[1][i], 0.0f, 0.0f);

		y = _mm_mul_ps(x, b0);		/* y = x * b0 */
		y = _mm_add_ps(y, x1);		/* y = x * b0 + x1*/
		z = _mm_mul_ps(y, a1);		/* z = a1 * y */
		x1 = _mm_mul_ps(x, b1);		/* x1 = x * b1 */
		x1 = _mm_add_ps(x1, x2);	/* x1 = x * b1 + x2*/
		x1 = _mm_sub_ps(x1, z);		/* x1 = x * b1 + x2 - a1 * y*/
		z = _mm_mul_ps(y, a2);		/* z = a2 * y */
		x2 = _mm_mul_ps(x, b2);		/* x2 = x * b2 */
		x2 = _mm_sub_ps(x2, z);		/* x2 = x * b2 - a2 * y*/

		out[0][i] = y[0];
		out[1][i] = y[1];
	}
#define F(x) (isnormal(x) ? (x) : 0.0f)
	bq[0*bq_stride].x1 = F(x1[0]);
	bq[0*bq_stride].x2 = F(x2[0]);
	bq[1*bq_stride].x1 = F(x1[1]);
	bq[1*bq_stride].x2 = F(x2[1]);
#undef F
}


static void dsp_biquad2_run2_sse(void *obj, struct biquad *bq, uint32_t bq_stride,
		float **out, const float **in, uint32_t n_samples)
{
	__m128 x, y, z;
	__m128 b00, b01, b02, b10, b11, b12;
	__m128 a01, a02, a11, a12;
	__m128 x01, x02, x11, x12;
	uint32_t i;

	b00 = _mm_setr_ps(bq[0].b0, bq[bq_stride].b0, 0.0f, 0.0f);  /* b00  b10  0  0 */
	b01 = _mm_setr_ps(bq[0].b1, bq[bq_stride].b1, 0.0f, 0.0f);  /* b01  b11  0  0 */
	b02 = _mm_setr_ps(bq[0].b2, bq[bq_stride].b2, 0.0f, 0.0f);  /* b02  b12  0  0 */
	a01 = _mm_setr_ps(bq[0].a1, bq[bq_stride].a1, 0.0f, 0.0f);  /* b00  b10  0  0 */
	a02 = _mm_setr_ps(bq[0].a2, bq[bq_stride].a2, 0.0f, 0.0f);  /* b01  b11  0  0 */
	x01 = _mm_setr_ps(bq[0].x1, bq[bq_stride].x1, 0.0f, 0.0f);  /* b00  b10  0  0 */
	x02 = _mm_setr_ps(bq[0].x2, bq[bq_stride].x2, 0.0f, 0.0f);  /* b01  b11  0  0 */

	b10 = _mm_setr_ps(bq[1].b0, bq[bq_stride+1].b0, 0.0f, 0.0f);  /* b00  b10  0  0 */
	b11 = _mm_setr_ps(bq[1].b1, bq[bq_stride+1].b1, 0.0f, 0.0f);  /* b01  b11  0  0 */
	b12 = _mm_setr_ps(bq[1].b2, bq[bq_stride+1].b2, 0.0f, 0.0f);  /* b02  b12  0  0 */
	a11 = _mm_setr_ps(bq[1].a1, bq[bq_stride+1].a1, 0.0f, 0.0f);  /* b00  b10  0  0 */
	a12 = _mm_setr_ps(bq[1].a2, bq[bq_stride+1].a2, 0.0f, 0.0f);  /* b01  b11  0  0 */
	x11 = _mm_setr_ps(bq[1].x1, bq[bq_stride+1].x1, 0.0f, 0.0f);  /* b00  b10  0  0 */
	x12 = _mm_setr_ps(bq[1].x2, bq[bq_stride+1].x2, 0.0f, 0.0f);  /* b01  b11  0  0 */

	for (i = 0; i < n_samples; i++) {
		x = _mm_setr_ps(in[0][i], in[1][i], 0.0f, 0.0f);

		y = _mm_mul_ps(x, b00);		/* y = x * b0 */
		y = _mm_add_ps(y, x01);		/* y = x * b0 + x1*/
		z = _mm_mul_ps(y, a01);		/* z = a1 * y */
		x01 = _mm_mul_ps(x, b01);	/* x1 = x * b1 */
		x01 = _mm_add_ps(x01, x02);	/* x1 = x * b1 + x2*/
		x01 = _mm_sub_ps(x01, z);	/* x1 = x * b1 + x2 - a1 * y*/
		z = _mm_mul_ps(y, a02);		/* z = a2 * y */
		x02 = _mm_mul_ps(x, b02);	/* x2 = x * b2 */
		x02 = _mm_sub_ps(x02, z);	/* x2 = x * b2 - a2 * y*/

		x = y;

		y = _mm_mul_ps(x, b10);		/* y = x * b0 */
		y = _mm_add_ps(y, x11);		/* y = x * b0 + x1*/
		z = _mm_mul_ps(y, a11);		/* z = a1 * y */
		x11 = _mm_mul_ps(x, b11);	/* x1 = x * b1 */
		x11 = _mm_add_ps(x11, x12);	/* x1 = x * b1 + x2*/
		x11 = _mm_sub_ps(x11, z);	/* x1 = x * b1 + x2 - a1 * y*/
		z = _mm_mul_ps(y, a12);		/* z = a2 * y*/
		x12 = _mm_mul_ps(x, b12);	/* x2 = x * b2 */
		x12 = _mm_sub_ps(x12, z);	/* x2 = x * b2 - a2 * y*/

		out[0][i] = y[0];
		out[1][i] = y[1];
	}
#define F(x) (isnormal(x) ? (x) : 0.0f)
	bq[0*bq_stride+0].x1 = F(x01[0]);
	bq[0*bq_stride+0].x2 = F(x02[0]);
	bq[1*bq_stride+0].x1 = F(x01[1]);
	bq[1*bq_stride+0].x2 = F(x02[1]);

	bq[0*bq_stride+1].x1 = F(x11[0]);
	bq[0*bq_stride+1].x2 = F(x12[0]);
	bq[1*bq_stride+1].x1 = F(x11[1]);
	bq[1*bq_stride+1].x2 = F(x12[1]);
#undef F
}

static void dsp_biquad_run4_sse(void *obj, struct biquad *bq, uint32_t bq_stride,
		float **out, const float **in, uint32_t n_samples)
{
	__m128 x, y, z;
	__m128 b0, b1, b2;
	__m128 a1, a2;
	__m128 x1, x2;
	uint32_t i;

	b0 = _mm_setr_ps(bq[0].b0, bq[bq_stride].b0, bq[2*bq_stride].b0, bq[3*bq_stride].b0);
	b1 = _mm_setr_ps(bq[0].b1, bq[bq_stride].b1, bq[2*bq_stride].b1, bq[3*bq_stride].b1);
	b2 = _mm_setr_ps(bq[0].b2, bq[bq_stride].b2, bq[2*bq_stride].b2, bq[3*bq_stride].b2);
	a1 = _mm_setr_ps(bq[0].a1, bq[bq_stride].a1, bq[2*bq_stride].a1, bq[3*bq_stride].a1);
	a2 = _mm_setr_ps(bq[0].a2, bq[bq_stride].a2, bq[2*bq_stride].a2, bq[3*bq_stride].a2);
	x1 = _mm_setr_ps(bq[0].x1, bq[bq_stride].x1, bq[2*bq_stride].x1, bq[3*bq_stride].x1);
	x2 = _mm_setr_ps(bq[0].x2, bq[bq_stride].x2, bq[2*bq_stride].x2, bq[3*bq_stride].x2);

	for (i = 0; i < n_samples; i++) {
		x = _mm_setr_ps(in[0][i], in[1][i], in[2][i], in[3][i]);

		y = _mm_mul_ps(x, b0);		/* y = x * b0 */
		y = _mm_add_ps(y, x1);		/* y = x * b0 + x1*/
		z = _mm_mul_ps(y, a1);		/* z = a1 * y */
		x1 = _mm_mul_ps(x, b1);		/* x1 = x * b1 */
		x1 = _mm_add_ps(x1, x2);	/* x1 = x * b1 + x2*/
		x1 = _mm_sub_ps(x1, z);		/* x1 = x * b1 + x2 - a1 * y*/
		z = _mm_mul_ps(y, a2);		/* z = a2 * y */
		x2 = _mm_mul_ps(x, b2);		/* x2 = x * b2 */
		x2 = _mm_sub_ps(x2, z);		/* x2 = x * b2 - a2 * y*/

		out[0][i] = y[0];
		out[1][i] = y[1];
		out[2][i] = y[2];
		out[3][i] = y[3];
	}
#define F(x) (isnormal(x) ? (x) : 0.0f)
	bq[0*bq_stride].x1 = F(x1[0]);
	bq[0*bq_stride].x2 = F(x2[0]);
	bq[1*bq_stride].x1 = F(x1[1]);
	bq[1*bq_stride].x2 = F(x2[1]);
	bq[2*bq_stride].x1 = F(x1[2]);
	bq[2*bq_stride].x2 = F(x2[2]);
	bq[3*bq_stride].x1 = F(x1[3]);
	bq[3*bq_stride].x2 = F(x2[3]);
#undef F
}

static void dsp_biquad2_run4_sse(void *obj, struct biquad *bq, uint32_t bq_stride,
		float **out, const float **in, uint32_t n_samples)
{
	__m128 x, y, z;
	__m128 b00, b01, b02, b10, b11, b12;
	__m128 a01, a02, a11, a12;
	__m128 x01, x02, x11, x12;
	uint32_t i;

	b00 = _mm_setr_ps(bq[0].b0, bq[bq_stride].b0, bq[2*bq_stride].b0, bq[3*bq_stride].b0);
	b01 = _mm_setr_ps(bq[0].b1, bq[bq_stride].b1, bq[2*bq_stride].b1, bq[3*bq_stride].b1);
	b02 = _mm_setr_ps(bq[0].b2, bq[bq_stride].b2, bq[2*bq_stride].b2, bq[3*bq_stride].b2);
	a01 = _mm_setr_ps(bq[0].a1, bq[bq_stride].a1, bq[2*bq_stride].a1, bq[3*bq_stride].a1);
	a02 = _mm_setr_ps(bq[0].a2, bq[bq_stride].a2, bq[2*bq_stride].a2, bq[3*bq_stride].a2);
	x01 = _mm_setr_ps(bq[0].x1, bq[bq_stride].x1, bq[2*bq_stride].x1, bq[3*bq_stride].x1);
	x02 = _mm_setr_ps(bq[0].x2, bq[bq_stride].x2, bq[2*bq_stride].x2, bq[3*bq_stride].x2);

	b10 = _mm_setr_ps(bq[1].b0, bq[bq_stride+1].b0, bq[2*bq_stride+1].b0, bq[3*bq_stride+1].b0);
	b11 = _mm_setr_ps(bq[1].b1, bq[bq_stride+1].b1, bq[2*bq_stride+1].b1, bq[3*bq_stride+1].b1);
	b12 = _mm_setr_ps(bq[1].b2, bq[bq_stride+1].b2, bq[2*bq_stride+1].b2, bq[3*bq_stride+1].b2);
	a11 = _mm_setr_ps(bq[1].a1, bq[bq_stride+1].a1, bq[2*bq_stride+1].a1, bq[3*bq_stride+1].a1);
	a12 = _mm_setr_ps(bq[1].a2, bq[bq_stride+1].a2, bq[2*bq_stride+1].a2, bq[3*bq_stride+1].a2);
	x11 = _mm_setr_ps(bq[1].x1, bq[bq_stride+1].x1, bq[2*bq_stride+1].x1, bq[3*bq_stride+1].x1);
	x12 = _mm_setr_ps(bq[1].x2, bq[bq_stride+1].x2, bq[2*bq_stride+1].x2, bq[3*bq_stride+1].x2);

	for (i = 0; i < n_samples; i++) {
		x = _mm_setr_ps(in[0][i], in[1][i], in[2][i], in[3][i]);

		y = _mm_mul_ps(x, b00);		/* y = x * b0 */
		y = _mm_add_ps(y, x01);		/* y = x * b0 + x1*/
		z = _mm_mul_ps(y, a01);		/* z = a1 * y */
		x01 = _mm_mul_ps(x, b01);	/* x1 = x * b1 */
		x01 = _mm_add_ps(x01, x02);	/* x1 = x * b1 + x2*/
		x01 = _mm_sub_ps(x01, z);	/* x1 = x * b1 + x2 - a1 * y*/
		z = _mm_mul_ps(y, a02);		/* z = a2 * y */
		x02 = _mm_mul_ps(x, b02);	/* x2 = x * b2 */
		x02 = _mm_sub_ps(x02, z);	/* x2 = x * b2 - a2 * y*/

		x = y;

		y = _mm_mul_ps(x, b10);		/* y = x * b0 */
		y = _mm_add_ps(y, x11);		/* y = x * b0 + x1*/
		z = _mm_mul_ps(y, a11);		/* z = a1 * y */
		x11 = _mm_mul_ps(x, b11);	/* x1 = x * b1 */
		x11 = _mm_add_ps(x11, x12);	/* x1 = x * b1 + x2*/
		x11 = _mm_sub_ps(x11, z);	/* x1 = x * b1 + x2 - a1 * y*/
		z = _mm_mul_ps(y, a12);		/* z = a2 * y*/
		x12 = _mm_mul_ps(x, b12);	/* x2 = x * b2 */
		x12 = _mm_sub_ps(x12, z);	/* x2 = x * b2 - a2 * y*/

		out[0][i] = y[0];
		out[1][i] = y[1];
		out[2][i] = y[2];
		out[3][i] = y[3];
	}
#define F(x) (isnormal(x) ? (x) : 0.0f)
	bq[0*bq_stride+0].x1 = F(x01[0]);
	bq[0*bq_stride+0].x2 = F(x02[0]);
	bq[1*bq_stride+0].x1 = F(x01[1]);
	bq[1*bq_stride+0].x2 = F(x02[1]);
	bq[2*bq_stride+0].x1 = F(x01[2]);
	bq[2*bq_stride+0].x2 = F(x02[2]);
	bq[3*bq_stride+0].x1 = F(x01[3]);
	bq[3*bq_stride+0].x2 = F(x02[3]);

	bq[0*bq_stride+1].x1 = F(x11[0]);
	bq[0*bq_stride+1].x2 = F(x12[0]);
	bq[1*bq_stride+1].x1 = F(x11[1]);
	bq[1*bq_stride+1].x2 = F(x12[1]);
	bq[2*bq_stride+1].x1 = F(x11[2]);
	bq[2*bq_stride+1].x2 = F(x12[2]);
	bq[3*bq_stride+1].x1 = F(x11[3]);
	bq[3*bq_stride+1].x2 = F(x12[3]);
#undef F
}

void dsp_biquad_run_sse(void *obj, struct biquad *bq, uint32_t n_bq, uint32_t bq_stride,
		float * SPA_RESTRICT out[], const float * SPA_RESTRICT in[],
		uint32_t n_src, uint32_t n_samples)
{
	uint32_t i, j, bqs2 = bq_stride*2, bqs4 = bqs2*2;
	uint32_t iunrolled4 = n_src & ~3;
	uint32_t iunrolled2 = n_src & ~1;
	uint32_t junrolled2 = n_bq & ~1;

	for (i = 0; i < iunrolled4; i+=4, bq+=bqs4) {
		const float *s[4] = { in[i], in[i+1], in[i+2], in[i+3] };
		float *d[4] = { out[i], out[i+1], out[i+2], out[i+3] };

		if (s[0] == NULL || s[1] == NULL || s[2] == NULL || s[3] == NULL ||
		    d[0] == NULL || d[1] == NULL || d[2] == NULL || d[3] == NULL)
			break;

		j = 0;
		if (j < junrolled2) {
			dsp_biquad2_run4_sse(obj, &bq[j], bq_stride, d, s, n_samples);
			s[0] = d[0];
			s[1] = d[1];
			s[2] = d[2];
			s[3] = d[3];
			j+=2;
		}
		for (; j < junrolled2; j+=2) {
			dsp_biquad2_run4_sse(obj, &bq[j], bq_stride, d, s, n_samples);
		}
		if (j < n_bq) {
			dsp_biquad_run4_sse(obj, &bq[j], bq_stride, d, s, n_samples);
		}
	}
	for (; i < iunrolled2; i+=2, bq+=bqs2) {
		const float *s[2] = { in[i], in[i+1] };
		float *d[2] = { out[i], out[i+1] };

		if (s[0] == NULL || s[1] == NULL || d[0] == NULL || d[1] == NULL)
			break;

		j = 0;
		if (j < junrolled2) {
			dsp_biquad2_run2_sse(obj, &bq[j], bq_stride, d, s, n_samples);
			s[0] = d[0];
			s[1] = d[1];
			j+=2;
		}
		for (; j < junrolled2; j+=2) {
			dsp_biquad2_run2_sse(obj, &bq[j], bq_stride, d, s, n_samples);
		}
		if (j < n_bq) {
			dsp_biquad_run2_sse(obj, &bq[j], bq_stride, d, s, n_samples);
		}
	}
	for (; i < n_src; i++, bq+=bq_stride) {
		const float *s = in[i];
		float *d = out[i];
		if (s == NULL || d == NULL)
			continue;

		j = 0;
		if (j < junrolled2) {
			dsp_biquad2_run_sse(obj, &bq[j], d, s, n_samples);
			s = d;
			j+=2;
		}
		for (; j < junrolled2; j+=2) {
			dsp_biquad2_run_sse(obj, &bq[j], d, s, n_samples);
		}
		if (j < n_bq) {
			dsp_biquad_run1_sse(obj, &bq[j], d, s, n_samples);
		}
	}
}

void dsp_delay_sse(void *obj, float *buffer, uint32_t *pos, uint32_t n_buffer, uint32_t delay,
		float *dst, const float *src, uint32_t n_samples, float fb, float ff)
{
	__m128 t[4];
	uint32_t w = *pos;
	uint32_t o = n_buffer - delay;
	uint32_t n, unrolled;

	if (fb == 0.0f && ff == 0.0f) {
		if (SPA_IS_ALIGNED(src, 16) &&
		    SPA_IS_ALIGNED(dst, 16) && delay >= 4)
			unrolled = n_samples & ~3;
		else
			unrolled = 0;

		for(n = 0; n < unrolled; n += 4) {
			t[0] = _mm_load_ps(&src[n]);
			_mm_storeu_ps(&buffer[w], t[0]);
			_mm_storeu_ps(&buffer[w+n_buffer], t[0]);
			t[0] = _mm_loadu_ps(&buffer[w+o]);
			_mm_store_ps(&dst[n], t[0]);
			w = w + 4 >= n_buffer ? 0 : w + 4;
		}
		for(; n < n_samples; n++) {
			t[0] = _mm_load_ss(&src[n]);
			_mm_store_ss(&buffer[w], t[0]);
			_mm_store_ss(&buffer[w+n_buffer], t[0]);
			t[0] = _mm_load_ss(&buffer[w+o]);
			_mm_store_ss(&dst[n], t[0]);
			w = w + 1 >= n_buffer ? 0 : w + 1;
		}
	} else {
		__m128 fb0 = _mm_set1_ps(fb);
		__m128 ff0 = _mm_set1_ps(ff);

		if (SPA_IS_ALIGNED(src, 16) &&
		    SPA_IS_ALIGNED(dst, 16) && delay >= 4)
			unrolled = n_samples & ~3;
		else
			unrolled = 0;

		for(n = 0; n < unrolled; n += 4) {
			t[0] = _mm_loadu_ps(&buffer[w+o]);
			t[1] = _mm_load_ps(&src[n]);
			t[2] = _mm_mul_ps(t[0], fb0);
			t[2] = _mm_add_ps(t[2], t[1]);
			_mm_storeu_ps(&buffer[w], t[2]);
			_mm_storeu_ps(&buffer[w+n_buffer], t[2]);
			t[2] = _mm_mul_ps(t[1], ff0);
			t[2] = _mm_add_ps(t[2], t[0]);
			_mm_store_ps(&dst[n], t[2]);
			w = w + 4 >= n_buffer ? 0 : w + 4;
		}
		for(; n < n_samples; n++) {
			t[0] = _mm_load_ss(&buffer[w+o]);
			t[1] = _mm_load_ss(&src[n]);
			t[2] = _mm_mul_ss(t[0], fb0);
			t[2] = _mm_add_ss(t[2], t[1]);
			_mm_store_ss(&buffer[w], t[2]);
			_mm_store_ss(&buffer[w+n_buffer], t[2]);
			t[2] = _mm_mul_ps(t[1], ff0);
			t[2] = _mm_add_ps(t[2], t[0]);
			_mm_store_ss(&dst[n], t[2]);
			w = w + 1 >= n_buffer ? 0 : w + 1;
		}
	}
	*pos = w;
}

inline static void _mm_mul_pz(__m128 *a, __m128 *b, __m128 *d)
{
    __m128 ar, ai, br, bi, arbr, arbi, aibi, aibr, dr, di;
    ar = _mm_shuffle_ps(a[0], a[1], _MM_SHUFFLE(2,0,2,0));	/* ar0 ar1 ar2 ar3 */
    ai = _mm_shuffle_ps(a[0], a[1], _MM_SHUFFLE(3,1,3,1));	/* ai0 ai1 ai2 ai3 */
    br = _mm_shuffle_ps(b[0], b[1], _MM_SHUFFLE(2,0,2,0));	/* br0 br1 br2 br3 */
    bi = _mm_shuffle_ps(b[0], b[1], _MM_SHUFFLE(3,1,3,1))	/* bi0 bi1 bi2 bi3 */;

    arbr = _mm_mul_ps(ar, br);	/* ar * br */
    arbi = _mm_mul_ps(ar, bi);	/* ar * bi */

    aibi = _mm_mul_ps(ai, bi);	/* ai * bi */
    aibr = _mm_mul_ps(ai, br);	/* ai * br */

    dr = _mm_sub_ps(arbr, aibi);	/* ar * br  - ai * bi */
    di = _mm_add_ps(arbi, aibr);	/* ar * bi  + ai * br */
    d[0] = _mm_unpacklo_ps(dr, di);
    d[1] = _mm_unpackhi_ps(dr, di);
}

void dsp_fft_cmul_sse(void *obj, void *fft,
	float * SPA_RESTRICT dst, const float * SPA_RESTRICT a,
	const float * SPA_RESTRICT b, uint32_t len, const float scale)
{
#ifdef HAVE_FFTW
	__m128 s = _mm_set1_ps(scale);
	__m128 aa[2], bb[2], dd[2];
	uint32_t i, unrolled;

	if (SPA_IS_ALIGNED(a, 16) &&
	    SPA_IS_ALIGNED(b, 16) &&
	    SPA_IS_ALIGNED(dst, 16))
		unrolled = len & ~3;
	else
		unrolled = 0;

	for (i = 0; i < unrolled; i+=4) {
		aa[0] = _mm_load_ps(&a[2*i]);	/* ar0 ai0 ar1 ai1 */
		aa[1] = _mm_load_ps(&a[2*i+4]);	/* ar1 ai1 ar2 ai2 */
		bb[0] = _mm_load_ps(&b[2*i]);	/* br0 bi0 br1 bi1 */
		bb[1] = _mm_load_ps(&b[2*i+4]);	/* br2 bi2 br3 bi3 */
		_mm_mul_pz(aa, bb, dd);
		dd[0] = _mm_mul_ps(dd[0], s);
		dd[1] = _mm_mul_ps(dd[1], s);
		_mm_store_ps(&dst[2*i], dd[0]);
		_mm_store_ps(&dst[2*i+4], dd[1]);
	}
	for (; i < len; i++) {
		dst[2*i  ] = (a[2*i] * b[2*i  ] - a[2*i+1] * b[2*i+1]) * scale;
		dst[2*i+1] = (a[2*i] * b[2*i+1] + a[2*i+1] * b[2*i  ]) * scale;
	}
#else
	pffft_zconvolve(fft, a, b, dst, scale);
#endif
}

void dsp_fft_cmuladd_sse(void *obj, void *fft,
	float * SPA_RESTRICT dst, const float * SPA_RESTRICT src,
	const float * SPA_RESTRICT a, const float * SPA_RESTRICT b,
	uint32_t len, const float scale)
{
#ifdef HAVE_FFTW
	__m128 s = _mm_set1_ps(scale);
	__m128 aa[2], bb[2], dd[2], t[2];
	uint32_t i, unrolled;

	if (SPA_IS_ALIGNED(a, 16) &&
	    SPA_IS_ALIGNED(b, 16) &&
	    SPA_IS_ALIGNED(src, 16) &&
	    SPA_IS_ALIGNED(dst, 16))
		unrolled = len & ~3;
	else
		unrolled = 0;

	for (i = 0; i < unrolled; i+=4) {
		aa[0] = _mm_load_ps(&a[2*i]);	/* ar0 ai0 ar1 ai1 */
		aa[1] = _mm_load_ps(&a[2*i+4]);	/* ar1 ai1 ar2 ai2 */
		bb[0] = _mm_load_ps(&b[2*i]);	/* br0 bi0 br1 bi1 */
		bb[1] = _mm_load_ps(&b[2*i+4]);	/* br2 bi2 br3 bi3 */
		_mm_mul_pz(aa, bb, dd);
		dd[0] = _mm_mul_ps(dd[0], s);
		dd[1] = _mm_mul_ps(dd[1], s);
		t[0] = _mm_load_ps(&src[2*i]);
		t[1] = _mm_load_ps(&src[2*i+4]);
		t[0] = _mm_add_ps(t[0], dd[0]);
		t[1] = _mm_add_ps(t[1], dd[1]);
		_mm_store_ps(&dst[2*i], t[0]);
		_mm_store_ps(&dst[2*i+4], t[1]);
	}
	for (; i < len; i++) {
		dst[2*i  ] = src[2*i  ] + (a[2*i] * b[2*i  ] - a[2*i+1] * b[2*i+1]) * scale;
		dst[2*i+1] = src[2*i+1] + (a[2*i] * b[2*i+1] + a[2*i+1] * b[2*i  ]) * scale;
	}
#else
	pffft_zconvolve_accumulate(fft, a, b, src, dst, scale);
#endif
}
