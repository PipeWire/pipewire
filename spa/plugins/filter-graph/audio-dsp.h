/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_FGA_DSP_H
#define SPA_FGA_DSP_H

#include <spa/utils/defs.h>
#include <spa/utils/hook.h>

#include "biquad.h"

#define SPA_TYPE_INTERFACE_FILTER_GRAPH_AudioDSP	SPA_TYPE_INFO_INTERFACE_BASE "FilterGraph:AudioDSP"

#define SPA_VERSION_FGA_DSP	0
struct spa_fga_dsp {
	struct spa_interface iface;
	uint32_t cpu_flags;
};

struct spa_fga_dsp_methods {
#define SPA_VERSION_FGA_DSP_METHODS		0
	uint32_t version;

	void (*clear) (void *obj, float * SPA_RESTRICT dst, uint32_t n_samples);
	void (*copy) (void *obj,
			float * SPA_RESTRICT dst,
			const float * SPA_RESTRICT src, uint32_t n_samples);
	void (*mix_gain) (void *obj,
			float * SPA_RESTRICT dst,
			const float * SPA_RESTRICT src[], uint32_t n_src,
			float gain[], uint32_t n_gain, uint32_t n_samples);
	void (*sum) (void *obj,
			float * dst, const float * SPA_RESTRICT a,
			const float * SPA_RESTRICT b, uint32_t n_samples);

	void *(*fft_new) (void *obj, uint32_t size, bool real);
	void (*fft_free) (void *obj, void *fft);
	void *(*fft_memalloc) (void *obj, uint32_t size, bool real);
	void (*fft_memfree) (void *obj, void *mem);
	void (*fft_memclear) (void *obj, void *mem, uint32_t size, bool real);
	void (*fft_run) (void *obj, void *fft, int direction,
			const float * SPA_RESTRICT src, float * SPA_RESTRICT dst);
	void (*fft_cmul) (void *obj, void *fft,
			float * SPA_RESTRICT dst, const float * SPA_RESTRICT a,
			const float * SPA_RESTRICT b, uint32_t len, const float scale);
	void (*fft_cmuladd) (void *obj, void *fft,
			float * dst, const float * src,
			const float * SPA_RESTRICT a, const float * SPA_RESTRICT b,
			uint32_t len, const float scale);
	void (*linear) (void *obj,
			float * dst, const float * SPA_RESTRICT src,
			const float mult, const float add, uint32_t n_samples);
	void (*mult) (void *obj,
			float * SPA_RESTRICT dst,
			const float * SPA_RESTRICT src[], uint32_t n_src, uint32_t n_samples);
	void (*biquad_run) (void *obj, struct biquad *bq, uint32_t n_bq, uint32_t bq_stride,
			float * SPA_RESTRICT out[], const float * SPA_RESTRICT in[],
			uint32_t n_src, uint32_t n_samples);
	void (*delay) (void *obj, float *buffer, uint32_t *pos, uint32_t n_buffer, uint32_t delay,
			float *dst, const float *src, uint32_t n_samples,
			float fb, float ff);
};

static inline void spa_fga_dsp_clear(struct spa_fga_dsp *obj, float * SPA_RESTRICT dst, uint32_t n_samples)
{
	spa_api_method_v(spa_fga_dsp, &obj->iface, clear, 0,
			dst, n_samples);
}
static inline void spa_fga_dsp_copy(struct spa_fga_dsp *obj,
		float * SPA_RESTRICT dst,
		const float * SPA_RESTRICT src, uint32_t n_samples)
{
	spa_api_method_v(spa_fga_dsp, &obj->iface, copy, 0,
			dst, src, n_samples);
}
static inline void spa_fga_dsp_mix_gain(struct spa_fga_dsp *obj,
		float * SPA_RESTRICT dst,
		const float * SPA_RESTRICT src[], uint32_t n_src,
		float gain[], uint32_t n_gain, uint32_t n_samples)
{
	spa_api_method_v(spa_fga_dsp, &obj->iface, mix_gain, 0,
			dst, src, n_src, gain, n_gain, n_samples);
}
static inline void spa_fga_dsp_sum(struct spa_fga_dsp *obj,
		float * dst, const float * SPA_RESTRICT a,
		const float * SPA_RESTRICT b, uint32_t n_samples)
{
	spa_api_method_v(spa_fga_dsp, &obj->iface, sum, 0,
			dst, a, b, n_samples);
}

static inline void *spa_fga_dsp_fft_new(struct spa_fga_dsp *obj, uint32_t size, bool real)
{
	return spa_api_method_r(void *, NULL, spa_fga_dsp, &obj->iface, fft_new, 0,
			size, real);
}
static inline void spa_fga_dsp_fft_free(struct spa_fga_dsp *obj, void *fft)
{
	spa_api_method_v(spa_fga_dsp, &obj->iface, fft_free, 0,
			fft);
}
static inline void *spa_fga_dsp_fft_memalloc(struct spa_fga_dsp *obj, uint32_t size, bool real)
{
	return spa_api_method_r(void *, NULL, spa_fga_dsp, &obj->iface, fft_memalloc, 0,
			size, real);
}
static inline void spa_fga_dsp_fft_memfree(struct spa_fga_dsp *obj, void *mem)
{
	spa_api_method_v(spa_fga_dsp, &obj->iface, fft_memfree, 0,
			mem);
}
static inline void spa_fga_dsp_fft_memclear(struct spa_fga_dsp *obj, void *mem, uint32_t size, bool real)
{
	spa_api_method_v(spa_fga_dsp, &obj->iface, fft_memclear, 0,
			mem, size, real);
}
static inline void spa_fga_dsp_fft_run(struct spa_fga_dsp *obj, void *fft, int direction,
		const float * SPA_RESTRICT src, float * SPA_RESTRICT dst)
{
	spa_api_method_v(spa_fga_dsp, &obj->iface, fft_run, 0,
			fft, direction, src, dst);
}
static inline void spa_fga_dsp_fft_cmul(struct spa_fga_dsp *obj, void *fft,
		float * SPA_RESTRICT dst, const float * SPA_RESTRICT a,
		const float * SPA_RESTRICT b, uint32_t len, const float scale)
{
	spa_api_method_v(spa_fga_dsp, &obj->iface, fft_cmul, 0,
			fft, dst, a, b, len, scale);
}
static inline void spa_fga_dsp_fft_cmuladd(struct spa_fga_dsp *obj, void *fft,
		float * dst, const float * src,
		const float * SPA_RESTRICT a, const float * SPA_RESTRICT b,
		uint32_t len, const float scale)
{
	spa_api_method_v(spa_fga_dsp, &obj->iface, fft_cmuladd, 0,
			fft, dst, src, a, b, len, scale);
}
static inline void spa_fga_dsp_linear(struct spa_fga_dsp *obj,
		float * dst, const float * SPA_RESTRICT src,
		const float mult, const float add, uint32_t n_samples)
{
	spa_api_method_v(spa_fga_dsp, &obj->iface, linear, 0,
			dst, src, mult, add, n_samples);
}
static inline void spa_fga_dsp_mult(struct spa_fga_dsp *obj,
		float * SPA_RESTRICT dst,
		const float * SPA_RESTRICT src[], uint32_t n_src, uint32_t n_samples)
{
	spa_api_method_v(spa_fga_dsp, &obj->iface, mult, 0,
			dst, src, n_src, n_samples);
}
static inline void spa_fga_dsp_biquad_run(struct spa_fga_dsp *obj,
		struct biquad *bq, uint32_t n_bq, uint32_t bq_stride,
		float * SPA_RESTRICT out[], const float * SPA_RESTRICT in[],
		uint32_t n_src, uint32_t n_samples)
{
	spa_api_method_v(spa_fga_dsp, &obj->iface, biquad_run, 0,
			bq, n_bq, bq_stride, out, in, n_src, n_samples);
}
static inline void spa_fga_dsp_delay(struct spa_fga_dsp *obj,
		float *buffer, uint32_t *pos, uint32_t n_buffer, uint32_t delay,
		float *dst, const float *src, uint32_t n_samples,
		float fb, float ff)
{
	spa_api_method_v(spa_fga_dsp, &obj->iface, delay, 0,
			buffer, pos, n_buffer, delay, dst, src, n_samples, fb, ff);
}

#endif /* SPA_FGA_DSP_H */
