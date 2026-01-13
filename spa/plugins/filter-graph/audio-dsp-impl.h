/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef DSP_IMPL_H
#define DSP_IMPL_H

#include "audio-dsp.h"

struct spa_fga_dsp * spa_fga_dsp_new(uint32_t cpu_flags);
void spa_fga_dsp_free(struct spa_fga_dsp *dsp);

#define MAKE_CLEAR_FUNC(arch) \
void dsp_clear_##arch(void *obj, float * SPA_RESTRICT dst, uint32_t n_samples)
#define MAKE_COPY_FUNC(arch) \
void dsp_copy_##arch(void *obj, float * SPA_RESTRICT dst, \
	const float * SPA_RESTRICT src, uint32_t n_samples)
#define MAKE_MIX_GAIN_FUNC(arch) \
void dsp_mix_gain_##arch(void *obj, float * SPA_RESTRICT dst,	\
	const float * SPA_RESTRICT src[], uint32_t n_src, float gain[], uint32_t n_gain, uint32_t n_samples)
#define MAKE_SUM_FUNC(arch) \
void dsp_sum_##arch (void *obj, float * SPA_RESTRICT dst, \
	const float * SPA_RESTRICT a, const float * SPA_RESTRICT b, uint32_t n_samples)
#define MAKE_LINEAR_FUNC(arch) \
void dsp_linear_##arch (void *obj, float * SPA_RESTRICT dst, \
	const float * SPA_RESTRICT src, const float mult, const float add, uint32_t n_samples)
#define MAKE_MULT_FUNC(arch) \
void dsp_mult_##arch(void *obj, float * SPA_RESTRICT dst,	\
	const float * SPA_RESTRICT src[], uint32_t n_src, uint32_t n_samples)
#define MAKE_BIQUAD_RUN_FUNC(arch) \
void dsp_biquad_run_##arch (void *obj, struct biquad *bq, uint32_t n_bq, uint32_t bq_stride, \
	float * SPA_RESTRICT out[], const float * SPA_RESTRICT in[], uint32_t n_src, uint32_t n_samples)
#define MAKE_DELAY_FUNC(arch) \
void dsp_delay_##arch (void *obj, float *buffer, uint32_t *pos, uint32_t n_buffer, \
		uint32_t delay, float *dst, const float *src, uint32_t n_samples)

#define MAKE_FFT_NEW_FUNC(arch) \
void *dsp_fft_new_##arch(void *obj, uint32_t size, bool real)
#define MAKE_FFT_FREE_FUNC(arch) \
void dsp_fft_free_##arch(void *obj, void *fft)
#define MAKE_FFT_MEMALLOC_FUNC(arch) \
void *dsp_fft_memalloc_##arch(void *obj, uint32_t size, bool real)
#define MAKE_FFT_MEMFREE_FUNC(arch) \
void dsp_fft_memfree_##arch(void *obj, void *mem)
#define MAKE_FFT_MEMCLEAR_FUNC(arch) \
void dsp_fft_memclear_##arch(void *obj, void *mem, uint32_t size, bool real)
#define MAKE_FFT_RUN_FUNC(arch) \
void dsp_fft_run_##arch(void *obj, void *fft, int direction, \
	const float * SPA_RESTRICT src, float * SPA_RESTRICT dst)
#define MAKE_FFT_CMUL_FUNC(arch) \
void dsp_fft_cmul_##arch(void *obj, void *fft, \
	float * SPA_RESTRICT dst, const float * SPA_RESTRICT a, \
	const float * SPA_RESTRICT b, uint32_t len, const float scale)
#define MAKE_FFT_CMULADD_FUNC(arch) \
void dsp_fft_cmuladd_##arch(void *obj, void *fft,		\
	float * dst, const float * src,					\
	const float * SPA_RESTRICT a, const float * SPA_RESTRICT b,	\
	uint32_t len, const float scale)


MAKE_CLEAR_FUNC(c);
MAKE_COPY_FUNC(c);
MAKE_MIX_GAIN_FUNC(c);
MAKE_SUM_FUNC(c);
MAKE_LINEAR_FUNC(c);
MAKE_MULT_FUNC(c);
MAKE_BIQUAD_RUN_FUNC(c);
MAKE_DELAY_FUNC(c);

MAKE_FFT_NEW_FUNC(c);
MAKE_FFT_FREE_FUNC(c);
MAKE_FFT_MEMALLOC_FUNC(c);
MAKE_FFT_MEMFREE_FUNC(c);
MAKE_FFT_MEMCLEAR_FUNC(c);
MAKE_FFT_RUN_FUNC(c);
MAKE_FFT_CMUL_FUNC(c);
MAKE_FFT_CMULADD_FUNC(c);

#if defined (HAVE_SSE)
MAKE_MIX_GAIN_FUNC(sse);
MAKE_SUM_FUNC(sse);
MAKE_BIQUAD_RUN_FUNC(sse);
MAKE_DELAY_FUNC(sse);
MAKE_FFT_CMUL_FUNC(sse);
MAKE_FFT_CMULADD_FUNC(sse);
#endif
#if defined (HAVE_AVX2)
MAKE_MIX_GAIN_FUNC(avx2);
MAKE_SUM_FUNC(avx2);
MAKE_FFT_CMUL_FUNC(avx2);
MAKE_FFT_CMULADD_FUNC(avx2);
#endif

#endif /* DSP_OPS_IMPL_H */
