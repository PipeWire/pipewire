/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <spa/utils/defs.h>

#ifdef HAVE_FFTW
#include <fftw3.h>
#else
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

void dsp_mult_avx2(void *obj,
		float * SPA_RESTRICT dst,
		const float * SPA_RESTRICT src[],
		uint32_t n_src, uint32_t n_samples)
{
	uint32_t n, i, unrolled;
	__m256 in[4];

	if (n_src == 0) {
		memset(dst, 0, n_samples * sizeof(float));
		return;
	}

	if (dst != src[0])
		spa_memcpy(dst, src[0], n_samples * sizeof(float));

	if (SPA_LIKELY(SPA_IS_ALIGNED(dst, 32))) {
		unrolled = n_samples & ~31;
		for (i = 1; i < n_src; i++) {
			if (SPA_UNLIKELY(!SPA_IS_ALIGNED(src[i], 32))) {
				unrolled = 0;
				break;
			}
		}
	} else
		unrolled = 0;

	for (i = 1; i < n_src; i++) {
		for (n = 0; n < unrolled; n += 32) {
			in[0] = _mm256_mul_ps(_mm256_load_ps(&dst[n+ 0]), _mm256_load_ps(&src[i][n+ 0]));
			in[1] = _mm256_mul_ps(_mm256_load_ps(&dst[n+ 8]), _mm256_load_ps(&src[i][n+ 8]));
			in[2] = _mm256_mul_ps(_mm256_load_ps(&dst[n+16]), _mm256_load_ps(&src[i][n+16]));
			in[3] = _mm256_mul_ps(_mm256_load_ps(&dst[n+24]), _mm256_load_ps(&src[i][n+24]));
			_mm256_store_ps(&dst[n+ 0], in[0]);
			_mm256_store_ps(&dst[n+ 8], in[1]);
			_mm256_store_ps(&dst[n+16], in[2]);
			_mm256_store_ps(&dst[n+24], in[3]);
		}
		for (; n < n_samples; n++)
			dst[n] *= src[i][n];
	}
}

void dsp_linear_avx2(void *obj, float * dst,
		const float * SPA_RESTRICT src, const float mult,
		const float add, uint32_t n_samples)
{
	uint32_t n, unrolled;
	__m256 m, a;

	if (mult == 0.0f) {
		a = _mm256_set1_ps(add);
		unrolled = n_samples & ~31;
		for (n = 0; n < unrolled; n += 32) {
			_mm256_storeu_ps(&dst[n+ 0], a);
			_mm256_storeu_ps(&dst[n+ 8], a);
			_mm256_storeu_ps(&dst[n+16], a);
			_mm256_storeu_ps(&dst[n+24], a);
		}
		for (; n < n_samples; n++)
			dst[n] = add;
		return;
	}

	if (SPA_LIKELY(SPA_IS_ALIGNED(src, 32) && SPA_IS_ALIGNED(dst, 32)))
		unrolled = n_samples & ~31;
	else
		unrolled = 0;

	m = _mm256_set1_ps(mult);

	if (add == 0.0f) {
		if (mult == 1.0f) {
			if (dst != src)
				spa_memcpy(dst, src, n_samples * sizeof(float));
			return;
		}
		for (n = 0; n < unrolled; n += 32) {
			_mm256_store_ps(&dst[n+ 0], _mm256_mul_ps(m, _mm256_load_ps(&src[n+ 0])));
			_mm256_store_ps(&dst[n+ 8], _mm256_mul_ps(m, _mm256_load_ps(&src[n+ 8])));
			_mm256_store_ps(&dst[n+16], _mm256_mul_ps(m, _mm256_load_ps(&src[n+16])));
			_mm256_store_ps(&dst[n+24], _mm256_mul_ps(m, _mm256_load_ps(&src[n+24])));
		}
		for (; n < n_samples; n++)
			dst[n] = mult * src[n];
	} else {
		a = _mm256_set1_ps(add);
		for (n = 0; n < unrolled; n += 32) {
			_mm256_store_ps(&dst[n+ 0], _mm256_fmadd_ps(m, _mm256_load_ps(&src[n+ 0]), a));
			_mm256_store_ps(&dst[n+ 8], _mm256_fmadd_ps(m, _mm256_load_ps(&src[n+ 8]), a));
			_mm256_store_ps(&dst[n+16], _mm256_fmadd_ps(m, _mm256_load_ps(&src[n+16]), a));
			_mm256_store_ps(&dst[n+24], _mm256_fmadd_ps(m, _mm256_load_ps(&src[n+24]), a));
		}
		for (; n < n_samples; n++)
			dst[n] = mult * src[n] + add;
	}
}

#define FFT_BLOCK	8

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

/* interleaved [r0,i0,...,r7,i7] -> blocked [r0..r7,i0..i7] */
static void fft_blocked_avx2(float *data, uint32_t len)
{
	const __m256i idx = _mm256_setr_epi32(0,2,4,6,1,3,5,7);
	uint32_t i;
	for (i = 0; i < len; i += FFT_BLOCK) {
		__m256 v0 = _mm256_load_ps(&data[0]);	/* r0 i0 r1 i1 r2 i2 r3 i3 */
		__m256 v1 = _mm256_load_ps(&data[8]);	/* r4 i4 r5 i5 r6 i6 r7 i7 */
		__m256 t0 = _mm256_permutevar8x32_ps(v0, idx); /* r0 r1 r2 r3 i0 i1 i2 i3 */
		__m256 t1 = _mm256_permutevar8x32_ps(v1, idx); /* r4 r5 r6 r7 i4 i5 i6 i7 */
		_mm256_store_ps(&data[0], _mm256_permute2f128_ps(t0, t1, 0x20));
		_mm256_store_ps(&data[8], _mm256_permute2f128_ps(t0, t1, 0x31));
		data += 2 * FFT_BLOCK;
	}
}

/* blocked [r0..r7,i0..i7] -> interleaved [r0,i0,...,r7,i7] with scaling */
static void fft_interleaved_avx2(float *data, uint32_t len, float scale)
{
	const __m256i idx = _mm256_setr_epi32(0,4,1,5,2,6,3,7);
	__m256 s = _mm256_set1_ps(scale);
	uint32_t i;
	for (i = 0; i < len; i += FFT_BLOCK) {
		__m256 r = _mm256_mul_ps(_mm256_load_ps(&data[0]), s);
		__m256 im = _mm256_mul_ps(_mm256_load_ps(&data[8]), s);
		__m256 t0 = _mm256_permute2f128_ps(r, im, 0x20);
		__m256 t1 = _mm256_permute2f128_ps(r, im, 0x31);
		_mm256_store_ps(&data[0], _mm256_permutevar8x32_ps(t0, idx));
		_mm256_store_ps(&data[8], _mm256_permutevar8x32_ps(t1, idx));
		data += 2 * FFT_BLOCK;
	}
}
#endif

void *dsp_fft_memalloc_avx2(void *obj, uint32_t size, bool real)
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

void dsp_fft_memclear_avx2(void *obj, void *data, uint32_t size, bool real)
{
#ifdef HAVE_FFTW
	spa_fga_dsp_clear(obj, data, real ? size : SPA_ROUND_UP_N(size, FFT_BLOCK) * 2);
#else
	spa_fga_dsp_clear(obj, data, real ? size : size * 2);
#endif
}

void dsp_fft_run_avx2(void *obj, void *fft, int direction,
	const float * SPA_RESTRICT src, float * SPA_RESTRICT dst)
{
	struct fft_info *info = fft;
#ifdef HAVE_FFTW
	uint32_t freq_size = SPA_ROUND_UP_N(info->size / 2 + 1, FFT_BLOCK);
	if (direction > 0) {
		fftwf_execute_dft_r2c(info->plan_r2c, (float*)src, (fftwf_complex*)dst);
		fft_blocked_avx2(dst, freq_size);
	} else {
		fft_interleaved_avx2((float*)src, freq_size, 1.0f / info->size);
		fftwf_execute_dft_c2r(info->plan_c2r, (fftwf_complex*)src, dst);
	}
#else
	if (direction < 0)
		spa_fga_dsp_linear(obj, (float*)src, (float*)src,
				1.0f / info->size, 0.0f, info->size);
	pffft_transform(info->setup, src, dst, NULL, direction < 0 ? PFFFT_BACKWARD : PFFFT_FORWARD);
#endif
}

void dsp_fft_cmul_avx2(void *obj, void *fft,
	float * SPA_RESTRICT dst, const float * SPA_RESTRICT a,
	const float * SPA_RESTRICT b, uint32_t len)
{
#ifdef HAVE_FFTW
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
		_mm256_store_ps(&dst[i], dr);
		_mm256_store_ps(&dst[i+8], di);
	}
#else
	struct fft_info *info = fft;
	pffft_zconvolve(info->setup, a, b, dst, 1.0f);
#endif
}

void dsp_fft_cmuladd_avx2(void *obj, void *fft,
	float * SPA_RESTRICT dst, const float * SPA_RESTRICT src,
	const float * SPA_RESTRICT a, const float * SPA_RESTRICT b,
	uint32_t len)
{
#ifdef HAVE_FFTW
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
		_mm256_store_ps(&dst[i], _mm256_add_ps(dr,
					_mm256_load_ps(&src[i])));
		_mm256_store_ps(&dst[i+8], _mm256_add_ps(di,
					_mm256_load_ps(&src[i+8])));
	}
#else
	struct fft_info *info = fft;
	pffft_zconvolve_accumulate(info->setup, a, b, src, dst, 1.0f);
#endif
}
