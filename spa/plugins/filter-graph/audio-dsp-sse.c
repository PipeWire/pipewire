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

#ifdef HAVE_FFTW
#include <fftw3.h>
#else
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

#define FFT_BLOCK	4

struct fft_info {
#ifdef HAVE_FFTW
	fftwf_plan plan_r2c;
	fftwf_plan plan_c2r;
#else
	void *setup;
#endif
	uint32_t size;
};

#ifdef HAVE_FFTW

/* interleaved [r0,i0,r1,i1,r2,i2,r3,i3] -> blocked [r0,r1,r2,r3,i0,i1,i2,i3] */
static void fft_blocked_sse(float *data, uint32_t len)
{
	uint32_t i;
	for (i = 0; i < len; i += FFT_BLOCK) {
		__m128 v0 = _mm_load_ps(&data[0]);	/* r0 i0 r1 i1 */
		__m128 v1 = _mm_load_ps(&data[4]);	/* r2 i2 r3 i3 */
		_mm_store_ps(&data[0], _mm_shuffle_ps(v0, v1, _MM_SHUFFLE(2,0,2,0)));
		_mm_store_ps(&data[4], _mm_shuffle_ps(v0, v1, _MM_SHUFFLE(3,1,3,1)));
		data += 2 * FFT_BLOCK;
	}
}

/* blocked [r0,r1,r2,r3,i0,i1,i2,i3] -> interleaved [r0,i0,r1,i1,r2,i2,r3,i3] with scaling */
static void fft_interleaved_sse(float *data, uint32_t len, float scale)
{
	uint32_t i;
	__m128 s = _mm_set1_ps(scale);
	for (i = 0; i < len; i += FFT_BLOCK) {
		__m128 r = _mm_mul_ps(_mm_load_ps(&data[0]), s);
		__m128 im = _mm_mul_ps(_mm_load_ps(&data[4]), s);
		_mm_store_ps(&data[0], _mm_unpacklo_ps(r, im));
		_mm_store_ps(&data[4], _mm_unpackhi_ps(r, im));
		data += 2 * FFT_BLOCK;
	}
}
#endif

void *dsp_fft_memalloc_sse(void *obj, uint32_t size, bool real)
{
#ifdef HAVE_FFTW
	return fftwf_alloc_real(real ? size : SPA_ROUND_UP_N(size, FFT_BLOCK) * 2);
#else
	if (real)
		return pffft_aligned_malloc(size * sizeof(float));
	else
		return pffft_aligned_malloc(size * 2 * sizeof(float));
#endif
}

void dsp_fft_memclear_sse(void *obj, void *data, uint32_t size, bool real)
{
#ifdef HAVE_FFTW
	spa_fga_dsp_clear(obj, data, real ? size : SPA_ROUND_UP_N(size, FFT_BLOCK) * 2);
#else
	spa_fga_dsp_clear(obj, data, real ? size : size * 2);
#endif
}

void dsp_fft_run_sse(void *obj, void *fft, int direction,
	const float * SPA_RESTRICT src, float * SPA_RESTRICT dst)
{
	struct fft_info *info = fft;
#ifdef HAVE_FFTW
	uint32_t freq_size = SPA_ROUND_UP_N(info->size / 2 + 1, FFT_BLOCK);
	if (direction > 0) {
		fftwf_execute_dft_r2c(info->plan_r2c, (float*)src, (fftwf_complex*)dst);
		fft_blocked_sse(dst, freq_size);
	} else {
		fft_interleaved_sse((float*)src, freq_size, 1.0f / info->size);
		fftwf_execute_dft_c2r(info->plan_c2r, (fftwf_complex*)src, dst);
	}
#else
	if (direction < 0)
		spa_fga_dsp_linear(obj, (float*)src, (float*)src,
				1.0f / info->size, 0.0f, info->size);
	pffft_transform(info->setup, src, dst, NULL, direction < 0 ? PFFFT_BACKWARD : PFFFT_FORWARD);
#endif
}

void dsp_fft_cmul_sse(void *obj, void *fft,
	float * SPA_RESTRICT dst, const float * SPA_RESTRICT a,
	const float * SPA_RESTRICT b, uint32_t len)
{
#ifdef HAVE_FFTW
	uint32_t i, plen = SPA_ROUND_UP_N(len, FFT_BLOCK) * 2;

	for (i = 0; i < plen; i += 2 * FFT_BLOCK) {
		__m128 ar = _mm_load_ps(&a[i]);
		__m128 ai = _mm_load_ps(&a[i + FFT_BLOCK]);
		__m128 br = _mm_load_ps(&b[i]);
		__m128 bi = _mm_load_ps(&b[i + FFT_BLOCK]);
		_mm_store_ps(&dst[i], _mm_sub_ps(
					_mm_mul_ps(ar, br), _mm_mul_ps(ai, bi)));
		_mm_store_ps(&dst[i + FFT_BLOCK], _mm_add_ps(
					_mm_mul_ps(ar, bi), _mm_mul_ps(ai, br)));
	}
#else
	struct fft_info *info = fft;
	pffft_zconvolve(info->setup, a, b, dst, 1.0f);
#endif
}

void dsp_fft_cmuladd_sse(void *obj, void *fft,
	float * SPA_RESTRICT dst, const float * SPA_RESTRICT src,
	const float * SPA_RESTRICT a, const float * SPA_RESTRICT b,
	uint32_t len)
{
#ifdef HAVE_FFTW
	uint32_t i, plen = SPA_ROUND_UP_N(len, FFT_BLOCK) * 2;

	for (i = 0; i < plen; i += 2 * FFT_BLOCK) {
		__m128 ar = _mm_load_ps(&a[i]);
		__m128 ai = _mm_load_ps(&a[i + FFT_BLOCK]);
		__m128 br = _mm_load_ps(&b[i]);
		__m128 bi = _mm_load_ps(&b[i + FFT_BLOCK]);
		_mm_store_ps(&dst[i], _mm_add_ps(_mm_load_ps(&src[i]),
					_mm_sub_ps(_mm_mul_ps(ar, br), _mm_mul_ps(ai, bi))));
		_mm_store_ps(&dst[i + FFT_BLOCK], _mm_add_ps(_mm_load_ps(&src[i + FFT_BLOCK]),
					_mm_add_ps(_mm_mul_ps(ar, bi), _mm_mul_ps(ai, br))));
	}
#else
	struct fft_info *info = fft;
	pffft_zconvolve_accumulate(info->setup, a, b, src, dst, 1.0f);
#endif
}
