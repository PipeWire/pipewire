/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "channelmix-ops.h"

#include <xmmintrin.h>

static inline void clear_sse(float *d, uint32_t n_samples)
{
	memset(d, 0, n_samples * sizeof(float));
}

static inline void copy_sse(float *d, const float *s, uint32_t n_samples)
{
	spa_memcpy(d, s, n_samples * sizeof(float));
}

static inline void vol_sse(float *d, const float *s, float vol, uint32_t n_samples)
{
	uint32_t n, unrolled;
	if (vol == 0.0f) {
		clear_sse(d, n_samples);
	} else if (vol == 1.0f) {
		copy_sse(d, s, n_samples);
	} else {
		__m128 t[4];
		const __m128 v = _mm_set1_ps(vol);

		if (SPA_IS_ALIGNED(d, 16) &&
		    SPA_IS_ALIGNED(s, 16))
			unrolled = n_samples & ~15;
		else
			unrolled = 0;

		for(n = 0; n < unrolled; n += 16) {
			t[0] = _mm_load_ps(&s[n]);
			t[1] = _mm_load_ps(&s[n+4]);
			t[2] = _mm_load_ps(&s[n+8]);
			t[3] = _mm_load_ps(&s[n+12]);
			_mm_store_ps(&d[n], _mm_mul_ps(t[0], v));
			_mm_store_ps(&d[n+4], _mm_mul_ps(t[1], v));
			_mm_store_ps(&d[n+8], _mm_mul_ps(t[2], v));
			_mm_store_ps(&d[n+12], _mm_mul_ps(t[3], v));
		}
		for(; n < n_samples; n++)
			_mm_store_ss(&d[n], _mm_mul_ss(_mm_load_ss(&s[n]), v));
	}
}

static inline void conv_sse(float *d, const float **s, float *c, uint32_t n_c, uint32_t n_samples)
{
	__m128 mi[n_c], sum[2];
	uint32_t n, j, unrolled;
	bool aligned = true;

	for (j = 0; j < n_c; j++) {
		mi[j] = _mm_set1_ps(c[j]);
		aligned &= SPA_IS_ALIGNED(s[j], 16);
	}

	if (aligned && SPA_IS_ALIGNED(d, 16))
		unrolled = n_samples & ~7;
	else
		unrolled = 0;

	for (n = 0; n < unrolled; n += 8) {
		sum[0] = sum[1] = _mm_setzero_ps();
		for (j = 0; j < n_c; j++) {
			sum[0] = _mm_add_ps(sum[0], _mm_mul_ps(_mm_load_ps(&s[j][n + 0]), mi[j]));
			sum[1] = _mm_add_ps(sum[1], _mm_mul_ps(_mm_load_ps(&s[j][n + 4]), mi[j]));
		}
		_mm_store_ps(&d[n + 0], sum[0]);
		_mm_store_ps(&d[n + 4], sum[1]);
	}
	for (; n < n_samples; n++) {
		sum[0] = _mm_setzero_ps();
		for (j = 0; j < n_c; j++)
			sum[0] = _mm_add_ss(sum[0], _mm_mul_ss(_mm_load_ss(&s[j][n]), mi[j]));
		_mm_store_ss(&d[n], sum[0]);
	}
}

static inline void avg_sse(float *d, const float *s0, const float *s1, uint32_t n_samples)
{
	uint32_t n, unrolled;
	__m128 half = _mm_set1_ps(0.5f);

	if (SPA_IS_ALIGNED(d, 16) &&
	    SPA_IS_ALIGNED(s0, 16) &&
	    SPA_IS_ALIGNED(s1, 16))
		unrolled = n_samples & ~7;
	else
		unrolled = 0;

	for (n = 0; n < unrolled; n += 8) {
		_mm_store_ps(&d[n + 0],
				_mm_mul_ps(
					_mm_add_ps(
						_mm_load_ps(&s0[n + 0]),
						_mm_load_ps(&s1[n + 0])),
					half));
		_mm_store_ps(&d[n + 4],
				_mm_mul_ps(
					_mm_add_ps(
						_mm_load_ps(&s0[n + 4]),
						_mm_load_ps(&s1[n + 4])),
					half));
	}

	for (; n < n_samples; n++)
		_mm_store_ss(&d[n],
				_mm_mul_ss(
					_mm_add_ss(
						_mm_load_ss(&s0[n]),
						_mm_load_ss(&s1[n])),
					half));
}

static inline void sub_sse(float *d, const float *s0, const float *s1, uint32_t n_samples)
{
	uint32_t n, unrolled;

	if (SPA_IS_ALIGNED(d, 16) &&
	    SPA_IS_ALIGNED(s0, 16) &&
	    SPA_IS_ALIGNED(s1, 16))
		unrolled = n_samples & ~7;
	else
		unrolled = 0;

	for (n = 0; n < unrolled; n += 8) {
		_mm_store_ps(&d[n + 0],
			_mm_sub_ps(_mm_load_ps(&s0[n + 0]), _mm_load_ps(&s1[n + 0])));
		_mm_store_ps(&d[n + 4],
			_mm_sub_ps(_mm_load_ps(&s0[n + 4]), _mm_load_ps(&s1[n + 4])));
	}
	for (; n < n_samples; n++)
		_mm_store_ss(&d[n],
			_mm_sub_ss(_mm_load_ss(&s0[n]), _mm_load_ss(&s1[n])));
}

void channelmix_copy_sse(struct channelmix *mix, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n_dst = mix->dst_chan;
	float **d = (float **)dst;
	const float **s = (const float **)src;
	for (i = 0; i < n_dst; i++)
		vol_sse(d[i], s[i], mix->matrix[i][i], n_samples);
}

void
channelmix_f32_n_m_sse(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	float **d = (float **) dst;
	const float **s = (const float **) src;
	uint32_t i, j, n_dst = mix->dst_chan, n_src = mix->src_chan;

	for (i = 0; i < n_dst; i++) {
		float *di = d[i];
		float mj[n_src];
		const float *sj[n_src];
		uint32_t n_j = 0;

		for (j = 0; j < n_src; j++) {
			if (mix->matrix[i][j] == 0.0f)
				continue;
			mj[n_j] = mix->matrix[i][j];
			sj[n_j++] = s[j];
		}
		if (n_j == 0) {
			clear_sse(di, n_samples);
		} else if (n_j == 1) {
			if (mix->lr4[i].active)
				lr4_process(&mix->lr4[i], di, sj[0], mj[0], n_samples);
			else
				vol_sse(di, sj[0], mj[0], n_samples);
		} else {
			conv_sse(di, sj, mj, n_j, n_samples);
			lr4_process(&mix->lr4[i], di, di, 1.0f, n_samples);
		}
	}
}

void
channelmix_f32_2_3p1_sse(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n, unrolled, n_dst = mix->dst_chan;
	float **d = (float **)dst;
	const float **s = (const float **)src;
	const float v0 = mix->matrix[0][0];
	const float v1 = mix->matrix[1][1];
	const float v2 = (mix->matrix[2][0] + mix->matrix[2][1]) * 0.5f;
	const float v3 = (mix->matrix[3][0] + mix->matrix[3][1]) * 0.5f;

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_sse(d[i], n_samples);
	}
	else {
		if (mix->widen == 0.0f) {
			vol_sse(d[0], s[0], v0, n_samples);
			vol_sse(d[1], s[1], v1, n_samples);
			avg_sse(d[2], s[0], s[1], n_samples);
		} else {
			const __m128 mv0 = _mm_set1_ps(mix->matrix[0][0]);
			const __m128 mv1 = _mm_set1_ps(mix->matrix[1][1]);
			const __m128 mw = _mm_set1_ps(mix->widen);
			const __m128 mh = _mm_set1_ps(0.5f);
			__m128 t0[1], t1[1], w[1], c[1];

			if (SPA_IS_ALIGNED(s[0], 16) &&
			    SPA_IS_ALIGNED(s[1], 16) &&
			    SPA_IS_ALIGNED(d[0], 16) &&
			    SPA_IS_ALIGNED(d[1], 16) &&
			    SPA_IS_ALIGNED(d[2], 16))
				unrolled = n_samples & ~3;
			else
				unrolled = 0;

			for(n = 0; n < unrolled; n += 4) {
				t0[0] = _mm_load_ps(&s[0][n]);
				t1[0] = _mm_load_ps(&s[1][n]);
				c[0] = _mm_add_ps(t0[0], t1[0]);
				w[0] = _mm_mul_ps(c[0], mw);
				_mm_store_ps(&d[0][n], _mm_mul_ps(_mm_sub_ps(t0[0], w[0]), mv0));
				_mm_store_ps(&d[1][n], _mm_mul_ps(_mm_sub_ps(t1[0], w[0]), mv1));
				_mm_store_ps(&d[2][n], _mm_mul_ps(c[0], mh));
			}
			for (; n < n_samples; n++) {
				t0[0] = _mm_load_ss(&s[0][n]);
				t1[0] = _mm_load_ss(&s[1][n]);
				c[0] = _mm_add_ss(t0[0], t1[0]);
				w[0] = _mm_mul_ss(c[0], mw);
				_mm_store_ss(&d[0][n], _mm_mul_ss(_mm_sub_ss(t0[0], w[0]), mv0));
				_mm_store_ss(&d[1][n], _mm_mul_ss(_mm_sub_ss(t1[0], w[0]), mv1));
				_mm_store_ss(&d[2][n], _mm_mul_ss(c[0], mh));
			}
		}
		lr4_process(&mix->lr4[3], d[3], d[2], v3, n_samples);
		lr4_process(&mix->lr4[2], d[2], d[2], v2, n_samples);
	}
}

void
channelmix_f32_2_5p1_sse(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n_dst = mix->dst_chan;
	float **d = (float **)dst;
	const float **s = (const float **)src;
	const float v4 = mix->matrix[4][0];
	const float v5 = mix->matrix[5][1];

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_sse(d[i], n_samples);
	}
	else {
		channelmix_f32_2_3p1_sse(mix, dst, src, n_samples);

		if (mix->upmix != CHANNELMIX_UPMIX_PSD) {
			vol_sse(d[4], s[0], v4, n_samples);
			vol_sse(d[5], s[1], v5, n_samples);
		} else {
			sub_sse(d[4], s[0], s[1], n_samples);

			delay_convolve_run(mix->buffer[1], &mix->pos[1], BUFFER_SIZE, mix->delay,
					mix->taps, mix->n_taps, d[5], d[4], -v5, n_samples);
			delay_convolve_run(mix->buffer[0], &mix->pos[0], BUFFER_SIZE, mix->delay,
					mix->taps, mix->n_taps, d[4], d[4], v4, n_samples);
		}
	}
}

void
channelmix_f32_2_7p1_sse(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n_dst = mix->dst_chan;
	float **d = (float **)dst;
	const float **s = (const float **)src;
	const float v4 = mix->matrix[4][0];
	const float v5 = mix->matrix[5][1];
	const float v6 = mix->matrix[6][0];
	const float v7 = mix->matrix[7][1];

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_sse(d[i], n_samples);
	}
	else {
		channelmix_f32_2_3p1_sse(mix, dst, src, n_samples);

		vol_sse(d[4], s[0], v4, n_samples);
		vol_sse(d[5], s[1], v5, n_samples);

		if (mix->upmix != CHANNELMIX_UPMIX_PSD) {
			vol_sse(d[6], s[0], v6, n_samples);
			vol_sse(d[7], s[1], v7, n_samples);
		} else {
			sub_sse(d[6], s[0], s[1], n_samples);

			delay_convolve_run(mix->buffer[1], &mix->pos[1], BUFFER_SIZE, mix->delay,
					mix->taps, mix->n_taps, d[7], d[6], -v7, n_samples);
			delay_convolve_run(mix->buffer[0], &mix->pos[0], BUFFER_SIZE, mix->delay,
					mix->taps, mix->n_taps, d[6], d[6], v6, n_samples);
		}
	}
}
/* FL+FR+FC+LFE -> FL+FR */
void
channelmix_f32_3p1_2_sse(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	float **d = (float **) dst;
	const float **s = (const float **) src;
	const float m0 = mix->matrix[0][0];
	const float m1 = mix->matrix[1][1];
	const float m2 = (mix->matrix[0][2] + mix->matrix[1][2]) * 0.5f;
	const float m3 = (mix->matrix[0][3] + mix->matrix[1][3]) * 0.5f;

	if (m0 == 0.0f && m1 == 0.0f && m2 == 0.0f && m3 == 0.0f) {
		clear_sse(d[0], n_samples);
		clear_sse(d[1], n_samples);
	}
	else {
		uint32_t n, unrolled;
		const __m128 v0 = _mm_set1_ps(m0);
		const __m128 v1 = _mm_set1_ps(m1);
		const __m128 clev = _mm_set1_ps(m2);
		const __m128 llev = _mm_set1_ps(m3);
		__m128 ctr;

		if (SPA_IS_ALIGNED(s[0], 16) &&
		    SPA_IS_ALIGNED(s[1], 16) &&
		    SPA_IS_ALIGNED(s[2], 16) &&
		    SPA_IS_ALIGNED(s[3], 16) &&
		    SPA_IS_ALIGNED(d[0], 16) &&
		    SPA_IS_ALIGNED(d[1], 16))
			unrolled = n_samples & ~3;
		else
			unrolled = 0;

		for(n = 0; n < unrolled; n += 4) {
			ctr = _mm_add_ps(
					_mm_mul_ps(_mm_load_ps(&s[2][n]), clev),
					_mm_mul_ps(_mm_load_ps(&s[3][n]), llev));
			_mm_store_ps(&d[0][n], _mm_add_ps(_mm_mul_ps(_mm_load_ps(&s[0][n]), v0), ctr));
			_mm_store_ps(&d[1][n], _mm_add_ps(_mm_mul_ps(_mm_load_ps(&s[1][n]), v1), ctr));
		}
		for(; n < n_samples; n++) {
			ctr = _mm_add_ss(_mm_mul_ss(_mm_load_ss(&s[2][n]), clev),
					_mm_mul_ss(_mm_load_ss(&s[3][n]), llev));
			_mm_store_ss(&d[0][n], _mm_add_ss(_mm_mul_ss(_mm_load_ss(&s[0][n]), v0), ctr));
			_mm_store_ss(&d[1][n], _mm_add_ss(_mm_mul_ss(_mm_load_ss(&s[1][n]), v1), ctr));
		}
	}
}

/* FL+FR+FC+LFE+SL+SR -> FL+FR */
void
channelmix_f32_5p1_2_sse(struct channelmix *mix, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t n, unrolled;
	float **d = (float **) dst;
	const float **s = (const float **) src;
	const float m00 = mix->matrix[0][0];
	const float m11 = mix->matrix[1][1];
	const __m128 clev = _mm_set1_ps((mix->matrix[0][2] + mix->matrix[1][2]) * 0.5f);
	const __m128 llev = _mm_set1_ps((mix->matrix[0][3] + mix->matrix[1][3]) * 0.5f);
	const __m128 slev0 = _mm_set1_ps(mix->matrix[0][4]);
	const __m128 slev1 = _mm_set1_ps(mix->matrix[1][5]);
	__m128 in, ctr;

	if (SPA_IS_ALIGNED(s[0], 16) &&
	    SPA_IS_ALIGNED(s[1], 16) &&
	    SPA_IS_ALIGNED(s[2], 16) &&
	    SPA_IS_ALIGNED(s[3], 16) &&
	    SPA_IS_ALIGNED(s[4], 16) &&
	    SPA_IS_ALIGNED(s[5], 16) &&
	    SPA_IS_ALIGNED(d[0], 16) &&
	    SPA_IS_ALIGNED(d[1], 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		clear_sse(d[0], n_samples);
		clear_sse(d[1], n_samples);
	}
	else {
		const __m128 v0 = _mm_set1_ps(m00);
		const __m128 v1 = _mm_set1_ps(m11);
		for(n = 0; n < unrolled; n += 4) {
			ctr = _mm_add_ps(_mm_mul_ps(_mm_load_ps(&s[2][n]), clev),
					_mm_mul_ps(_mm_load_ps(&s[3][n]), llev));
			in = _mm_mul_ps(_mm_load_ps(&s[4][n]), slev0);
			in = _mm_add_ps(in, ctr);
			in = _mm_add_ps(in, _mm_mul_ps(_mm_load_ps(&s[0][n]), v0));
			_mm_store_ps(&d[0][n], in);
			in = _mm_mul_ps(_mm_load_ps(&s[5][n]), slev1);
			in = _mm_add_ps(in, ctr);
			in = _mm_add_ps(in, _mm_mul_ps(_mm_load_ps(&s[1][n]), v1));
			_mm_store_ps(&d[1][n], in);
		}
		for(; n < n_samples; n++) {
			ctr = _mm_mul_ss(_mm_load_ss(&s[2][n]), clev);
			ctr = _mm_add_ss(ctr, _mm_mul_ss(_mm_load_ss(&s[3][n]), llev));
			in = _mm_mul_ss(_mm_load_ss(&s[4][n]), slev0);
			in = _mm_add_ss(in, ctr);
			in = _mm_add_ss(in, _mm_mul_ss(_mm_load_ss(&s[0][n]), v0));
			_mm_store_ss(&d[0][n], in);
			in = _mm_mul_ss(_mm_load_ss(&s[5][n]), slev1);
			in = _mm_add_ss(in, ctr);
			in = _mm_add_ss(in, _mm_mul_ss(_mm_load_ss(&s[1][n]), v1));
			_mm_store_ss(&d[1][n], in);
		}
	}
}

/* FL+FR+FC+LFE+SL+SR -> FL+FR+FC+LFE*/
void
channelmix_f32_5p1_3p1_sse(struct channelmix *mix, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n, unrolled, n_dst = mix->dst_chan;
	float **d = (float **) dst;
	const float **s = (const float **) src;

	if (SPA_IS_ALIGNED(s[0], 16) &&
	    SPA_IS_ALIGNED(s[1], 16) &&
	    SPA_IS_ALIGNED(s[2], 16) &&
	    SPA_IS_ALIGNED(s[3], 16) &&
	    SPA_IS_ALIGNED(s[4], 16) &&
	    SPA_IS_ALIGNED(s[5], 16) &&
	    SPA_IS_ALIGNED(d[0], 16) &&
	    SPA_IS_ALIGNED(d[1], 16) &&
	    SPA_IS_ALIGNED(d[2], 16) &&
	    SPA_IS_ALIGNED(d[3], 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_sse(d[i], n_samples);
	}
	else {
		const __m128 v0 = _mm_set1_ps(mix->matrix[0][0]);
		const __m128 v1 = _mm_set1_ps(mix->matrix[1][1]);
		const __m128 slev0 = _mm_set1_ps(mix->matrix[0][4]);
		const __m128 slev1 = _mm_set1_ps(mix->matrix[1][5]);

		for(n = 0; n < unrolled; n += 4) {
			_mm_store_ps(&d[0][n], _mm_add_ps(
					_mm_mul_ps(_mm_load_ps(&s[0][n]), v0),
					_mm_mul_ps(_mm_load_ps(&s[4][n]), slev0)));

			_mm_store_ps(&d[1][n], _mm_add_ps(
					_mm_mul_ps(_mm_load_ps(&s[1][n]), v1),
					_mm_mul_ps(_mm_load_ps(&s[5][n]), slev1)));
		}
		for(; n < n_samples; n++) {
			_mm_store_ss(&d[0][n], _mm_add_ss(
					_mm_mul_ss(_mm_load_ss(&s[0][n]), v0),
					_mm_mul_ss(_mm_load_ss(&s[4][n]), slev0)));

			_mm_store_ss(&d[1][n], _mm_add_ss(
					_mm_mul_ss(_mm_load_ss(&s[1][n]), v1),
					_mm_mul_ss(_mm_load_ss(&s[5][n]), slev1)));
		}
		vol_sse(d[2], s[2], mix->matrix[2][2], n_samples);
		vol_sse(d[3], s[3], mix->matrix[3][3], n_samples);
	}
}

/* FL+FR+FC+LFE+SL+SR -> FL+FR+RL+RR*/
void
channelmix_f32_5p1_4_sse(struct channelmix *mix, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n_dst = mix->dst_chan;
	float **d = (float **) dst;
	const float **s = (const float **) src;
	const float v4 = mix->matrix[2][4];
	const float v5 = mix->matrix[3][5];

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_sse(d[i], n_samples);
	}
	else {
		channelmix_f32_3p1_2_sse(mix, dst, src, n_samples);

		vol_sse(d[2], s[4], v4, n_samples);
		vol_sse(d[3], s[5], v5, n_samples);
	}
}
