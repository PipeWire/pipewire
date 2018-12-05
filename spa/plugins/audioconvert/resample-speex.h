/* Spa
 *
 * Copyright © 2018 Wim Taymans
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

#include <speex/speex_resampler.h>

static void impl_speex_free(struct resample *r)
{
	if (r->data)
		speex_resampler_destroy(r->data);
	r->data = NULL;
}

static void impl_speex_update_rate(struct resample *r, double rate)
{
	speex_resampler_set_rate_frac(r->data,
			r->i_rate * rate, r->o_rate, r->i_rate, r->o_rate);
}

static void impl_speex_process(struct resample *r, int channel,
                              void *src, uint32_t *in_len, void *dst, uint32_t *out_len)
{
	speex_resampler_process_float(r->data, channel, src, in_len, dst, out_len);
}

static void impl_speex_reset (struct resample *r)
{
}

static int impl_speex_init(struct resample *r)
{
	int err;

	r->free = impl_speex_free;
	r->update_rate = impl_speex_update_rate;
	r->process = impl_speex_process;
	r->reset = impl_speex_reset;
	r->data = speex_resampler_init_frac(r->channels,
					    r->i_rate, r->o_rate, r->i_rate, r->o_rate,
					    SPEEX_RESAMPLER_QUALITY_DEFAULT, &err);
	if (r->data == NULL) {
		return -ENOMEM;
	}

	return 0;
}
