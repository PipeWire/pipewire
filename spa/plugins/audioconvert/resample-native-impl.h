/* Spa
 *
 * Copyright © 2019 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <math.h>

#include <spa/utils/defs.h>

#include "resample.h"

typedef void (*resample_func_t)(struct resample *r,
        const void * SPA_RESTRICT src[], uint32_t *in_len,
        void * SPA_RESTRICT dst[], uint32_t offs, uint32_t *out_len);

struct native_data {
	double rate;
	uint32_t n_taps;
	uint32_t n_phases;
	uint32_t in_rate;
	uint32_t out_rate;
	uint32_t index;
	uint32_t phase;
	uint32_t inc;
	uint32_t frac;
	uint32_t filter_stride;
	uint32_t filter_stride_os;
	uint32_t hist;
	float **history;
	resample_func_t func;
	float *filter;
	float *hist_mem;
};

#define DEFINE_RESAMPLER_FULL(arch)						\
void do_resample_full_##arch(struct resample *r,				\
	const void * SPA_RESTRICT src[], uint32_t *in_len,			\
	void * SPA_RESTRICT dst[], uint32_t offs, uint32_t *out_len)

#define DEFINE_RESAMPLER_INTER(arch)						\
void do_resample_inter_##arch(struct resample *r,				\
	const void * SPA_RESTRICT src[], uint32_t *in_len,			\
	void * SPA_RESTRICT dst[], uint32_t offs, uint32_t *out_len)

#define MAKE_RESAMPLER_COPY(arch)						\
static void do_resample_copy_##arch(struct resample *r,				\
	const void * SPA_RESTRICT src[], uint32_t *in_len,			\
	void * SPA_RESTRICT dst[], uint32_t offs, uint32_t *out_len)		\
{										\
	struct native_data *data = r->data;					\
	uint32_t index, n_taps = data->n_taps;					\
	uint32_t c, olen = *out_len, ilen = *in_len;				\
										\
	if (r->channels == 0)							\
		return;								\
										\
	index = data->index;							\
	if (offs < olen && index + n_taps <= ilen) {				\
		uint32_t to_copy = SPA_MIN(olen - offs,				\
				ilen - (index + n_taps) + 1);			\
		for (c = 0; c < r->channels; c++) {				\
			const float *s = src[c];				\
			float *d = dst[c];					\
			memcpy(&d[offs], &s[index], to_copy * sizeof(float));	\
		}								\
		index += to_copy;						\
		offs += to_copy;						\
	}									\
	*in_len = index - data->index;						\
	*out_len = offs;							\
	data->index = index;							\
}

#define MAKE_RESAMPLER_FULL(arch)						\
DEFINE_RESAMPLER_FULL(arch)							\
{										\
	struct native_data *data = r->data;					\
	uint32_t n_taps = data->n_taps, stride = data->filter_stride_os;	\
	uint32_t index, phase, n_phases = data->out_rate;			\
	uint32_t c, o, olen = *out_len, ilen = *in_len;				\
	uint32_t inc = data->inc, frac = data->frac;				\
										\
	if (r->channels == 0)							\
		return;								\
										\
	for (c = 0; c < r->channels; c++) {					\
		const float *s = src[c];					\
		float *d = dst[c];						\
										\
		index = data->index;						\
		phase = data->phase;						\
										\
		for (o = offs; o < olen && index + n_taps <= ilen; o++) {	\
			const float *ip, *taps;					\
										\
			ip = &s[index];						\
			taps = &data->filter[phase * stride];			\
			index += inc;						\
			phase += frac;						\
			if (phase >= n_phases) {				\
				phase -= n_phases;				\
				index += 1;					\
			}							\
			inner_product_##arch(&d[o], ip, taps, n_taps);		\
		}								\
	}									\
	*in_len = index - data->index;						\
	*out_len = o;								\
	data->index = index;							\
	data->phase = phase;							\
}

#define MAKE_RESAMPLER_INTER(arch)						\
DEFINE_RESAMPLER_INTER(arch)							\
{										\
	struct native_data *data = r->data;					\
	uint32_t index, phase, stride = data->filter_stride;			\
	uint32_t n_phases = data->n_phases, out_rate = data->out_rate;		\
	uint32_t n_taps = data->n_taps;						\
	uint32_t c, o, olen = *out_len, ilen = *in_len;				\
	uint32_t inc = data->inc, frac = data->frac;				\
										\
	if (r->channels == 0)							\
		return;								\
										\
	for (c = 0; c < r->channels; c++) {					\
		const float *s = src[c];					\
		float *d = dst[c];						\
										\
		index = data->index;						\
		phase = data->phase;						\
										\
		for (o = offs; o < olen && index + n_taps <= ilen; o++) {	\
			const float *ip;					\
			float ph, x, *t0, *t1;					\
			uint32_t offset;					\
										\
			ip = &s[index];						\
			ph = (float)phase * n_phases / out_rate;		\
			offset = floor(ph);					\
			x = ph - (float)offset;					\
										\
			t0 = &data->filter[(offset + 0) * stride];		\
			t1 = &data->filter[(offset + 1) * stride];		\
			index += inc;						\
			phase += frac;						\
			if (phase >= out_rate) {				\
				phase -= out_rate;				\
				index += 1;					\
			}							\
			inner_product_ip_##arch(&d[o], ip, t0, t1, x, n_taps);	\
		}								\
	}									\
	*in_len = index - data->index;						\
	*out_len = o;								\
	data->index = index;							\
	data->phase = phase;							\
}


DEFINE_RESAMPLER_FULL(c);
DEFINE_RESAMPLER_INTER(c);

#if defined (HAVE_SSE)
DEFINE_RESAMPLER_FULL(sse);
DEFINE_RESAMPLER_INTER(sse);
#endif
#if defined (HAVE_SSSE3)
DEFINE_RESAMPLER_FULL(ssse3);
DEFINE_RESAMPLER_INTER(ssse3);
#endif
