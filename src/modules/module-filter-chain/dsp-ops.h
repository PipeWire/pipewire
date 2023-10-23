/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef DSP_OPS_H
#define DSP_OPS_H

#include <spa/utils/defs.h>

#include "biquad.h"

struct dsp_ops;

struct dsp_ops_funcs {
	void (*clear) (struct dsp_ops *ops, void * SPA_RESTRICT dst, uint32_t n_samples);
	void (*copy) (struct dsp_ops *ops,
			void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src, uint32_t n_samples);
	void (*mix_gain) (struct dsp_ops *ops,
			void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src[],
			float gain[], uint32_t n_src, uint32_t n_samples);
	void (*biquad_run) (struct dsp_ops *ops, struct biquad *bq,
			float *out, const float *in, uint32_t n_samples);
	void (*sum) (struct dsp_ops *ops,
			float * dst, const float * SPA_RESTRICT a,
			const float * SPA_RESTRICT b, uint32_t n_samples);

	void *(*fft_new) (struct dsp_ops *ops, int32_t size, bool real);
	void (*fft_free) (struct dsp_ops *ops, void *fft);
	void (*fft_run) (struct dsp_ops *ops, void *fft, int direction,
			const float * SPA_RESTRICT src, float * SPA_RESTRICT dst);
	void (*fft_cmul) (struct dsp_ops *ops, void *fft,
			float * SPA_RESTRICT dst, const float * SPA_RESTRICT a,
			const float * SPA_RESTRICT b, uint32_t len, const float scale);
	void (*fft_cmuladd) (struct dsp_ops *ops, void *fft,
			float * dst, const float * src,
			const float * SPA_RESTRICT a, const float * SPA_RESTRICT b,
			uint32_t len, const float scale);
	void (*linear) (struct dsp_ops *ops,
			float * dst, const float * SPA_RESTRICT src,
			const float mult, const float add, uint32_t n_samples);
	void (*mult) (struct dsp_ops *ops,
			void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src[], uint32_t n_src, uint32_t n_samples);
};

struct dsp_ops {
	uint32_t cpu_flags;

	void (*free) (struct dsp_ops *ops);

	struct dsp_ops_funcs funcs;

	const void *priv;
};

int dsp_ops_init(struct dsp_ops *ops);

#define dsp_ops_free(ops)		(ops)->free(ops)

#define dsp_ops_clear(ops,...)		(ops)->funcs.clear(ops, __VA_ARGS__)
#define dsp_ops_copy(ops,...)		(ops)->funcs.copy(ops, __VA_ARGS__)
#define dsp_ops_mix_gain(ops,...)	(ops)->funcs.mix_gain(ops, __VA_ARGS__)
#define dsp_ops_biquad_run(ops,...)	(ops)->funcs.biquad_run(ops, __VA_ARGS__)
#define dsp_ops_sum(ops,...)		(ops)->funcs.sum(ops, __VA_ARGS__)
#define dsp_ops_linear(ops,...)		(ops)->funcs.linear(ops, __VA_ARGS__)
#define dsp_ops_mult(ops,...)		(ops)->funcs.mult(ops, __VA_ARGS__)

#define dsp_ops_fft_new(ops,...)	(ops)->funcs.fft_new(ops, __VA_ARGS__)
#define dsp_ops_fft_free(ops,...)	(ops)->funcs.fft_free(ops, __VA_ARGS__)
#define dsp_ops_fft_run(ops,...)	(ops)->funcs.fft_run(ops, __VA_ARGS__)
#define dsp_ops_fft_cmul(ops,...)	(ops)->funcs.fft_cmul(ops, __VA_ARGS__)
#define dsp_ops_fft_cmuladd(ops,...)	(ops)->funcs.fft_cmuladd(ops, __VA_ARGS__)

#define MAKE_CLEAR_FUNC(arch) \
void dsp_clear_##arch(struct dsp_ops *ops, void * SPA_RESTRICT dst, uint32_t n_samples)
#define MAKE_COPY_FUNC(arch) \
void dsp_copy_##arch(struct dsp_ops *ops, void * SPA_RESTRICT dst, \
	const void * SPA_RESTRICT src, uint32_t n_samples)
#define MAKE_MIX_GAIN_FUNC(arch) \
void dsp_mix_gain_##arch(struct dsp_ops *ops, void * SPA_RESTRICT dst,	\
	const void * SPA_RESTRICT src[], float gain[], uint32_t n_src, uint32_t n_samples)
#define MAKE_BIQUAD_RUN_FUNC(arch) \
void dsp_biquad_run_##arch (struct dsp_ops *ops, struct biquad *bq,	\
	float *out, const float *in, uint32_t n_samples)
#define MAKE_SUM_FUNC(arch) \
void dsp_sum_##arch (struct dsp_ops *ops, float * SPA_RESTRICT dst, \
	const float * SPA_RESTRICT a, const float * SPA_RESTRICT b, uint32_t n_samples)
#define MAKE_LINEAR_FUNC(arch) \
void dsp_linear_##arch (struct dsp_ops *ops, float * SPA_RESTRICT dst, \
	const float * SPA_RESTRICT src, const float mult, const float add, uint32_t n_samples)
#define MAKE_MULT_FUNC(arch) \
void dsp_mult_##arch(struct dsp_ops *ops, void * SPA_RESTRICT dst,	\
	const void * SPA_RESTRICT src[], uint32_t n_src, uint32_t n_samples)

#define MAKE_FFT_NEW_FUNC(arch) \
void *dsp_fft_new_##arch(struct dsp_ops *ops, int32_t size, bool real)
#define MAKE_FFT_FREE_FUNC(arch) \
void dsp_fft_free_##arch(struct dsp_ops *ops, void *fft)
#define MAKE_FFT_RUN_FUNC(arch) \
void dsp_fft_run_##arch(struct dsp_ops *ops, void *fft, int direction, \
	const float * SPA_RESTRICT src, float * SPA_RESTRICT dst)
#define MAKE_FFT_CMUL_FUNC(arch) \
void dsp_fft_cmul_##arch(struct dsp_ops *ops, void *fft, \
	float * SPA_RESTRICT dst, const float * SPA_RESTRICT a, \
	const float * SPA_RESTRICT b, uint32_t len, const float scale)
#define MAKE_FFT_CMULADD_FUNC(arch) \
void dsp_fft_cmuladd_##arch(struct dsp_ops *ops, void *fft,		\
	float * dst, const float * src,					\
	const float * SPA_RESTRICT a, const float * SPA_RESTRICT b,	\
	uint32_t len, const float scale)

MAKE_CLEAR_FUNC(c);
MAKE_COPY_FUNC(c);
MAKE_MIX_GAIN_FUNC(c);
MAKE_BIQUAD_RUN_FUNC(c);
MAKE_SUM_FUNC(c);
MAKE_LINEAR_FUNC(c);
MAKE_MULT_FUNC(c);

MAKE_FFT_NEW_FUNC(c);
MAKE_FFT_FREE_FUNC(c);
MAKE_FFT_RUN_FUNC(c);
MAKE_FFT_CMUL_FUNC(c);
MAKE_FFT_CMULADD_FUNC(c);

#if defined (HAVE_SSE)
MAKE_MIX_GAIN_FUNC(sse);
MAKE_SUM_FUNC(sse);
#endif
#if defined (HAVE_AVX)
MAKE_SUM_FUNC(avx);
#endif

#endif /* DSP_OPS_H */
