/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <spa/utils/defs.h>

#ifndef HAVE_FFTW
#include "pffft.h"
#endif
#include "audio-dsp-impl.h"

#include <immintrin.h>

static void dsp_add_avx2(void *obj, float *dst, const float * SPA_RESTRICT src[],
		uint32_t n_src, uint32_t n_samples)
{
	uint32_t n, i, unrolled;
	__m256 in[4];
	const float **s = (const float **)src;
	float *d = dst;

	if (SPA_LIKELY(SPA_IS_ALIGNED(dst, 32))) {
		unrolled = n_samples & ~31;
		for (i = 0; i < n_src; i++) {
			if (SPA_UNLIKELY(!SPA_IS_ALIGNED(src[i], 32))) {
				unrolled = 0;
				break;
			}
		}
	} else
		unrolled = 0;

	for (n = 0; n < unrolled; n += 32) {
		in[0] = _mm256_load_ps(&s[0][n+ 0]);
		in[1] = _mm256_load_ps(&s[0][n+ 8]);
		in[2] = _mm256_load_ps(&s[0][n+16]);
		in[3] = _mm256_load_ps(&s[0][n+24]);

		for (i = 1; i < n_src; i++) {
			in[0] = _mm256_add_ps(in[0], _mm256_load_ps(&s[i][n+ 0]));
			in[1] = _mm256_add_ps(in[1], _mm256_load_ps(&s[i][n+ 8]));
			in[2] = _mm256_add_ps(in[2], _mm256_load_ps(&s[i][n+16]));
			in[3] = _mm256_add_ps(in[3], _mm256_load_ps(&s[i][n+24]));
		}
		_mm256_store_ps(&d[n+ 0], in[0]);
		_mm256_store_ps(&d[n+ 8], in[1]);
		_mm256_store_ps(&d[n+16], in[2]);
		_mm256_store_ps(&d[n+24], in[3]);
	}
	for (; n < n_samples; n++) {
		__m128 in[1];
		in[0] = _mm_load_ss(&s[0][n]);
		for (i = 1; i < n_src; i++)
			in[0] = _mm_add_ss(in[0], _mm_load_ss(&s[i][n]));
		_mm_store_ss(&d[n], in[0]);
	}
}

static void dsp_add_1_gain_avx2(void *obj, float *dst, const float * SPA_RESTRICT src[],
		uint32_t n_src, float gain, uint32_t n_samples)
{
	uint32_t n, i, unrolled;
	__m256 in[4], g;
	const float **s = (const float **)src;
	float *d = dst;
	__m128 g1;

	if (SPA_LIKELY(SPA_IS_ALIGNED(dst, 32))) {
		unrolled = n_samples & ~31;
		for (i = 0; i < n_src; i++) {
			if (SPA_UNLIKELY(!SPA_IS_ALIGNED(src[i], 32))) {
				unrolled = 0;
				break;
			}
		}
	} else
		unrolled = 0;

	g = _mm256_set1_ps(gain);
	g1 = _mm_set_ss(gain);

	for (n = 0; n < unrolled; n += 32) {
		in[0] = _mm256_load_ps(&s[0][n+ 0]);
		in[1] = _mm256_load_ps(&s[0][n+ 8]);
		in[2] = _mm256_load_ps(&s[0][n+16]);
		in[3] = _mm256_load_ps(&s[0][n+24]);

		for (i = 1; i < n_src; i++) {
			in[0] = _mm256_add_ps(in[0], _mm256_load_ps(&s[i][n+ 0]));
			in[1] = _mm256_add_ps(in[1], _mm256_load_ps(&s[i][n+ 8]));
			in[2] = _mm256_add_ps(in[2], _mm256_load_ps(&s[i][n+16]));
			in[3] = _mm256_add_ps(in[3], _mm256_load_ps(&s[i][n+24]));
		}
		_mm256_store_ps(&d[n+ 0], _mm256_mul_ps(g, in[0]));
		_mm256_store_ps(&d[n+ 8], _mm256_mul_ps(g, in[1]));
		_mm256_store_ps(&d[n+16], _mm256_mul_ps(g, in[2]));
		_mm256_store_ps(&d[n+24], _mm256_mul_ps(g, in[3]));
	}
	for (; n < n_samples; n++) {
		__m128 in[1];
		in[0] = _mm_load_ss(&s[0][n]);
		for (i = 1; i < n_src; i++)
			in[0] = _mm_add_ss(in[0], _mm_load_ss(&s[i][n]));
		_mm_store_ss(&d[n], _mm_mul_ss(g1, in[0]));
	}
}

static void dsp_add_n_gain_avx2(void *obj, float *dst,
		const float * SPA_RESTRICT src[], uint32_t n_src,
		float gain[], uint32_t n_gain, uint32_t n_samples)
{
	uint32_t n, i, unrolled;
	__m256 in[4], g;
	const float **s = (const float **)src;
	float *d = dst;

	if (SPA_LIKELY(SPA_IS_ALIGNED(dst, 32))) {
		unrolled = n_samples & ~31;
		for (i = 0; i < n_src; i++) {
			if (SPA_UNLIKELY(!SPA_IS_ALIGNED(src[i], 32))) {
				unrolled = 0;
				break;
			}
		}
	} else
		unrolled = 0;

	for (n = 0; n < unrolled; n += 32) {
		g = _mm256_set1_ps(gain[0]);
		in[0] = _mm256_mul_ps(g, _mm256_load_ps(&s[0][n+ 0]));
		in[1] = _mm256_mul_ps(g, _mm256_load_ps(&s[0][n+ 8]));
		in[2] = _mm256_mul_ps(g, _mm256_load_ps(&s[0][n+16]));
		in[3] = _mm256_mul_ps(g, _mm256_load_ps(&s[0][n+24]));

		for (i = 1; i < n_src; i++) {
			g = _mm256_set1_ps(gain[i]);
			in[0] = _mm256_fmadd_ps(g, _mm256_load_ps(&s[i][n+ 0]), in[0]);
			in[1] = _mm256_fmadd_ps(g, _mm256_load_ps(&s[i][n+ 8]), in[1]);
			in[2] = _mm256_fmadd_ps(g, _mm256_load_ps(&s[i][n+16]), in[2]);
			in[3] = _mm256_fmadd_ps(g, _mm256_load_ps(&s[i][n+24]), in[3]);
		}
		_mm256_store_ps(&d[n+ 0], in[0]);
		_mm256_store_ps(&d[n+ 8], in[1]);
		_mm256_store_ps(&d[n+16], in[2]);
		_mm256_store_ps(&d[n+24], in[3]);
	}
	for (; n < n_samples; n++) {
		__m128 in[1], g;
		g = _mm_set_ss(gain[0]);
		in[0] = _mm_mul_ss(g, _mm_load_ss(&s[0][n]));
		for (i = 1; i < n_src; i++) {
			g = _mm_set_ss(gain[i]);
			in[0] = _mm_add_ss(in[0], _mm_mul_ss(g, _mm_load_ss(&s[i][n])));
		}
		_mm_store_ss(&d[n], in[0]);
	}
}


void dsp_mix_gain_avx2(void *obj,
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
			dsp_add_avx2(obj, dst, src, n_src, n_samples);
		else if (n_gain < n_src)
			dsp_add_1_gain_avx2(obj, dst, src, n_src, gain[0], n_samples);
		else
			dsp_add_n_gain_avx2(obj, dst, src, n_src, gain, n_gain, n_samples);
	}
}

void dsp_sum_avx2(void *obj, float *r, const float *a, const float *b, uint32_t n_samples)
{
	uint32_t n, unrolled;
	__m256 in[4];

	unrolled = n_samples & ~31;

	if (SPA_LIKELY(SPA_IS_ALIGNED(r, 32)) &&
	    SPA_LIKELY(SPA_IS_ALIGNED(a, 32)) &&
	    SPA_LIKELY(SPA_IS_ALIGNED(b, 32))) {
		for (n = 0; n < unrolled; n += 32) {
			in[0] = _mm256_load_ps(&a[n+ 0]);
			in[1] = _mm256_load_ps(&a[n+ 8]);
			in[2] = _mm256_load_ps(&a[n+16]);
			in[3] = _mm256_load_ps(&a[n+24]);

			in[0] = _mm256_add_ps(in[0], _mm256_load_ps(&b[n+ 0]));
			in[1] = _mm256_add_ps(in[1], _mm256_load_ps(&b[n+ 8]));
			in[2] = _mm256_add_ps(in[2], _mm256_load_ps(&b[n+16]));
			in[3] = _mm256_add_ps(in[3], _mm256_load_ps(&b[n+24]));

			_mm256_store_ps(&r[n+ 0], in[0]);
			_mm256_store_ps(&r[n+ 8], in[1]);
			_mm256_store_ps(&r[n+16], in[2]);
			_mm256_store_ps(&r[n+24], in[3]);
		}
	} else {
		for (n = 0; n < unrolled; n += 32) {
			in[0] = _mm256_loadu_ps(&a[n+ 0]);
			in[1] = _mm256_loadu_ps(&a[n+ 8]);
			in[2] = _mm256_loadu_ps(&a[n+16]);
			in[3] = _mm256_loadu_ps(&a[n+24]);

			in[0] = _mm256_add_ps(in[0], _mm256_loadu_ps(&b[n+ 0]));
			in[1] = _mm256_add_ps(in[1], _mm256_loadu_ps(&b[n+ 8]));
			in[2] = _mm256_add_ps(in[2], _mm256_loadu_ps(&b[n+16]));
			in[3] = _mm256_add_ps(in[3], _mm256_loadu_ps(&b[n+24]));

			_mm256_storeu_ps(&r[n+ 0], in[0]);
			_mm256_storeu_ps(&r[n+ 8], in[1]);
			_mm256_storeu_ps(&r[n+16], in[2]);
			_mm256_storeu_ps(&r[n+24], in[3]);
		}
	}
	for (; n < n_samples; n++) {
		__m128 in[1];
		in[0] = _mm_load_ss(&a[n]);
		in[0] = _mm_add_ss(in[0], _mm_load_ss(&b[n]));
		_mm_store_ss(&r[n], in[0]);
	}
}

void dsp_fft_cmul_avx2(void *obj, void *fft,
	float * SPA_RESTRICT dst, const float * SPA_RESTRICT a,
	const float * SPA_RESTRICT b, uint32_t len, const float scale)
{
#ifdef HAVE_FFTW
	__m256 s = _mm256_set1_ps(scale);
	uint32_t i, plen = SPA_ROUND_UP_N(len, 8) * 2;

	for (i = 0; i < plen; i += 16) {
		__m256 ar = _mm256_load_ps(&a[i]);
		__m256 ai = _mm256_load_ps(&a[i+8]);
		__m256 br = _mm256_load_ps(&b[i]);
		__m256 bi = _mm256_load_ps(&b[i+8]);
		__m256 dr = _mm256_mul_ps(ar, br);
		__m256 di = _mm256_mul_ps(ar, bi);
		dr = _mm256_fnmadd_ps(ai, bi, dr);	/* ar*br - ai*bi */
		di = _mm256_fmadd_ps(ai, br, di);	/* ar*bi + ai*br */
		_mm256_store_ps(&dst[i], _mm256_mul_ps(dr, s));
		_mm256_store_ps(&dst[i+8], _mm256_mul_ps(di, s));
	}
#else
	pffft_zconvolve(fft, a, b, dst, scale);
#endif
}

void dsp_fft_cmuladd_avx2(void *obj, void *fft,
	float * SPA_RESTRICT dst, const float * SPA_RESTRICT src,
	const float * SPA_RESTRICT a, const float * SPA_RESTRICT b,
	uint32_t len, const float scale)
{
#ifdef HAVE_FFTW
	__m256 s = _mm256_set1_ps(scale);
	uint32_t i, plen = SPA_ROUND_UP_N(len, 8) * 2;

	for (i = 0; i < plen; i += 16) {
		__m256 ar = _mm256_load_ps(&a[i]);
		__m256 ai = _mm256_load_ps(&a[i+8]);
		__m256 br = _mm256_load_ps(&b[i]);
		__m256 bi = _mm256_load_ps(&b[i+8]);
		__m256 dr = _mm256_mul_ps(ar, br);
		__m256 di = _mm256_mul_ps(ar, bi);
		dr = _mm256_fnmadd_ps(ai, bi, dr);	/* ar*br - ai*bi */
		di = _mm256_fmadd_ps(ai, br, di);	/* ar*bi + ai*br */
		_mm256_store_ps(&dst[i], _mm256_fmadd_ps(dr, s,
					_mm256_load_ps(&src[i])));
		_mm256_store_ps(&dst[i+8], _mm256_fmadd_ps(di, s,
					_mm256_load_ps(&src[i+8])));
	}
#else
	pffft_zconvolve_accumulate(fft, a, b, src, dst, scale);
#endif
}
