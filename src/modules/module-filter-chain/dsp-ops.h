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
	void (*sum) (struct dsp_ops *ops,
			float * dst, const float * SPA_RESTRICT a,
			const float * SPA_RESTRICT b, uint32_t n_samples);

	void *(*fft_new) (struct dsp_ops *ops, uint32_t size, bool real);
	void (*fft_free) (struct dsp_ops *ops, void *fft);
	void *(*fft_memalloc) (struct dsp_ops *ops, uint32_t size, bool real);
	void (*fft_memfree) (struct dsp_ops *ops, void *mem);
	void (*fft_memclear) (struct dsp_ops *ops, void *mem, uint32_t size, bool real);
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
	void (*biquad_run) (struct dsp_ops *ops, struct biquad *bq, uint32_t n_bq, uint32_t bq_stride,
			float * SPA_RESTRICT out[], const float * SPA_RESTRICT in[],
			uint32_t n_src, uint32_t n_samples);
	void (*delay) (struct dsp_ops *ops, float *buffer, uint32_t *pos, uint32_t n_buffer, uint32_t delay,
			float *dst, const float *src, uint32_t n_samples);
};

struct dsp_ops {
	uint32_t cpu_flags;

	void (*free) (struct dsp_ops *ops);

	struct dsp_ops_funcs funcs;

	const void *priv;
};

#define dsp_ops_free(ops)		(ops)->free(ops)

#define dsp_ops_clear(ops,...)		(ops)->funcs.clear(ops, __VA_ARGS__)
#define dsp_ops_copy(ops,...)		(ops)->funcs.copy(ops, __VA_ARGS__)
#define dsp_ops_mix_gain(ops,...)	(ops)->funcs.mix_gain(ops, __VA_ARGS__)
#define dsp_ops_biquad_run(ops,...)	(ops)->funcs.biquad_run(ops, __VA_ARGS__)
#define dsp_ops_sum(ops,...)		(ops)->funcs.sum(ops, __VA_ARGS__)
#define dsp_ops_linear(ops,...)		(ops)->funcs.linear(ops, __VA_ARGS__)
#define dsp_ops_mult(ops,...)		(ops)->funcs.mult(ops, __VA_ARGS__)
#define dsp_ops_delay(ops,...)		(ops)->funcs.delay(ops, __VA_ARGS__)

#define dsp_ops_fft_new(ops,...)	(ops)->funcs.fft_new(ops, __VA_ARGS__)
#define dsp_ops_fft_free(ops,...)	(ops)->funcs.fft_free(ops, __VA_ARGS__)
#define dsp_ops_fft_memalloc(ops,...)	(ops)->funcs.fft_memalloc(ops, __VA_ARGS__)
#define dsp_ops_fft_memfree(ops,...)	(ops)->funcs.fft_memfree(ops, __VA_ARGS__)
#define dsp_ops_fft_memclear(ops,...)	(ops)->funcs.fft_memclear(ops, __VA_ARGS__)
#define dsp_ops_fft_run(ops,...)	(ops)->funcs.fft_run(ops, __VA_ARGS__)
#define dsp_ops_fft_cmul(ops,...)	(ops)->funcs.fft_cmul(ops, __VA_ARGS__)
#define dsp_ops_fft_cmuladd(ops,...)	(ops)->funcs.fft_cmuladd(ops, __VA_ARGS__)

#endif /* DSP_OPS_H */
