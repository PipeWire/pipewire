/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>
#include <float.h>

#include <spa/utils/defs.h>

#include "dsp-ops.h"

#include <xmmintrin.h>

void dsp_mix_gain_sse(struct dsp_ops *ops,
		void * SPA_RESTRICT dst,
		const void * SPA_RESTRICT src[],
		float gain[], uint32_t n_src, uint32_t n_samples)
{
	if (n_src == 0) {
		memset(dst, 0, n_samples * sizeof(float));
	} else if (n_src == 1 && gain[0] == 1.0f) {
		if (dst != src[0])
			spa_memcpy(dst, src[0], n_samples * sizeof(float));
	} else {
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
}

void dsp_sum_sse(struct dsp_ops *ops, float *r, const float *a, const float *b, uint32_t n_samples)
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

void dsp_biquad_run_sse(struct dsp_ops *ops, struct biquad *bq,
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
#define F(x) (-FLT_MIN < (x) && (x) < FLT_MIN ? 0.0f : (x))
	bq->x1 = F(x12[0]);
	bq->x2 = F(x12[1]);
#undef F
}

static void dsp_biquad_run2_sse(struct dsp_ops *ops, struct biquad *bq0, struct biquad *bq1,
		float *out, const float *in, uint32_t n_samples)
{
	__m128 x, y, z;
	__m128 b0, b1;
	__m128 a0, a1;
	__m128 x0, x1;
	uint32_t i;

	b0 = _mm_setr_ps(bq0->b0, bq0->b1, bq0->b2, 0.0f);  /* b0  b1  b2  0 */
	a0 = _mm_setr_ps(0.0f, bq0->a1, bq0->a2, 0.0f);	    /* 0   a1  a2  0 */
	x0 = _mm_setr_ps(bq0->x1, bq0->x2, 0.0f, 0.0f);	    /* x1  x2  0   0 */

	b1 = _mm_setr_ps(bq1->b0, bq1->b1, bq1->b2, 0.0f);  /* b0  b1  b2  0 */
	a1 = _mm_setr_ps(0.0f, bq1->a1, bq1->a2, 0.0f);	    /* 0   a1  a2  0 */
	x1 = _mm_setr_ps(bq1->x1, bq1->x2, 0.0f, 0.0f);	    /* x1  x2  0   0 */

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
#define F(x) (-FLT_MIN < (x) && (x) < FLT_MIN ? 0.0f : (x))
	bq0->x1 = F(x0[0]);
	bq0->x2 = F(x0[1]);
	bq1->x1 = F(x1[0]);
	bq1->x2 = F(x1[1]);
#undef F
}

void dsp_biquadn_run_sse(struct dsp_ops *ops, struct biquad *bq, uint32_t n_bq,
		float * SPA_RESTRICT out[], const float * SPA_RESTRICT in[],
		uint32_t n_src, uint32_t n_samples)
{
	uint32_t i, j;
	const float *s;
	float *d;
	uint32_t unrolled = n_bq & ~1;
	struct biquad *b = bq;

	for (i = 0; i < n_src; i++, b+=n_src) {
		s = in[i];
		d = out[i];
		for (j = 0; j < unrolled; j+=2) {
			dsp_biquad_run2_sse(ops, &b[j], &b[j+1], d, s, n_samples);
			s = d;
		}
		for (; j < n_bq; j++) {
			dsp_biquad_run_sse(ops, &b[j], d, s, n_samples);
			s = d;
		}
	}
}
