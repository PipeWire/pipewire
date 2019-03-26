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

#define MAKE_RESAMPLER_FULL(arch)						\
static void do_resample_full_##arch(struct resample *r,				\
	const void * SPA_RESTRICT src[], uint32_t *in_len,			\
	void * SPA_RESTRICT dst[], uint32_t offs, uint32_t *out_len)		\
{										\
	struct native_data *data = r->data;					\
	uint32_t index, phase;							\
	uint32_t out_rate = data->out_rate;					\
	uint32_t n_taps = data->n_taps;						\
	uint32_t c, o, olen = *out_len, ilen = *in_len;				\
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
			taps = &data->filter[phase * n_taps];			\
			index += data->inc;					\
			phase += data->frac;					\
			if (phase >= out_rate) {				\
				phase -= out_rate;				\
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
static void do_resample_inter_##arch(struct resample *r,			\
	const void * SPA_RESTRICT src[], uint32_t *in_len,			\
	void * SPA_RESTRICT dst[], uint32_t offs, uint32_t *out_len)		\
{										\
	struct native_data *data = r->data;					\
	uint32_t index, phase;							\
	uint32_t n_phases = data->n_phases, out_rate = data->out_rate;		\
	uint32_t n_taps = data->n_taps;						\
	uint32_t c, o, olen = *out_len, ilen = *in_len;				\
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
			t0 = &data->filter[(offset + 0) * n_taps];		\
			t1 = &data->filter[(offset + 1) * n_taps];		\
			index += data->inc;					\
			phase += data->frac;					\
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
