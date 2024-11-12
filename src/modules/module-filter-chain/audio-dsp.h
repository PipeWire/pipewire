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

	void (*clear) (void *obj, void * SPA_RESTRICT dst, uint32_t n_samples);
	void (*copy) (void *obj,
			void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src, uint32_t n_samples);
	void (*mix_gain) (void *obj,
			void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src[],
			float gain[], uint32_t n_src, uint32_t n_samples);
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
			void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src[], uint32_t n_src, uint32_t n_samples);
	void (*biquad_run) (void *obj, struct biquad *bq, uint32_t n_bq, uint32_t bq_stride,
			float * SPA_RESTRICT out[], const float * SPA_RESTRICT in[],
			uint32_t n_src, uint32_t n_samples);
	void (*delay) (void *obj, float *buffer, uint32_t *pos, uint32_t n_buffer, uint32_t delay,
			float *dst, const float *src, uint32_t n_samples);
};

#define spa_fga_dsp_method_r(o,type,method,version,...)			\
({									\
	type _res = NULL;						\
	struct spa_fga_dsp *_o = o;					\
	spa_interface_call_fast_res(&_o->iface,				\
			struct spa_fga_dsp_methods, _res,		\
			method, version, ##__VA_ARGS__);		\
	_res;								\
})


#define spa_fga_dsp_method(o,method,version,...)			\
({									\
	struct spa_fga_dsp *_o = o;					\
	spa_interface_call_fast(&_o->iface,				\
			struct spa_fga_dsp_methods,			\
			method, version, ##__VA_ARGS__);		\
})


#define spa_fga_dsp_clear(o,...)	spa_fga_dsp_method(o,clear,0,__VA_ARGS__)
#define spa_fga_dsp_copy(o,...)		spa_fga_dsp_method(o,copy,0,__VA_ARGS__)
#define spa_fga_dsp_mix_gain(o,...)	spa_fga_dsp_method(o,mix_gain,0,__VA_ARGS__)
#define spa_fga_dsp_biquad_run(o,...)	spa_fga_dsp_method(o,biquad_run,0,__VA_ARGS__)
#define spa_fga_dsp_sum(o,...)		spa_fga_dsp_method(o,sum,0,__VA_ARGS__)
#define spa_fga_dsp_linear(o,...)	spa_fga_dsp_method(o,linear,0,__VA_ARGS__)
#define spa_fga_dsp_mult(o,...)		spa_fga_dsp_method(o,mult,0,__VA_ARGS__)
#define spa_fga_dsp_delay(o,...)	spa_fga_dsp_method(o,delay,0,__VA_ARGS__)

#define spa_fga_dsp_fft_new(o,...)	spa_fga_dsp_method_r(o,void*,fft_new,0,__VA_ARGS__)
#define spa_fga_dsp_fft_free(o,...)	spa_fga_dsp_method(o,fft_free,0,__VA_ARGS__)
#define spa_fga_dsp_fft_memalloc(o,...)	spa_fga_dsp_method_r(o,void*,fft_memalloc,0,__VA_ARGS__)
#define spa_fga_dsp_fft_memfree(o,...)	spa_fga_dsp_method(o,fft_memfree,0,__VA_ARGS__)
#define spa_fga_dsp_fft_memclear(o,...)	spa_fga_dsp_method(o,fft_memclear,0,__VA_ARGS__)
#define spa_fga_dsp_fft_run(o,...)	spa_fga_dsp_method(o,fft_run,0,__VA_ARGS__)
#define spa_fga_dsp_fft_cmul(o,...)	spa_fga_dsp_method(o,fft_cmul,0,__VA_ARGS__)
#define spa_fga_dsp_fft_cmul(o,...)	spa_fga_dsp_method(o,fft_cmul,0,__VA_ARGS__)
#define spa_fga_dsp_fft_cmuladd(o,...)	spa_fga_dsp_method(o,fft_cmuladd,0,__VA_ARGS__)

#endif /* SPA_FGA_DSP_H */
