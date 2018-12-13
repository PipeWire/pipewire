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

#include <math.h>

struct peaks_data {
	unsigned o_count;
	unsigned i_count;
	float max_f[0];
};

static void impl_peaks_free(struct resample *r)
{
	if (r->data)
		free(r->data);
	r->data = NULL;
}

static void impl_peaks_update_rate(struct resample *r, double rate)
{
}

static void impl_peaks_process(struct resample *r, int channel,
			void *src, uint32_t *in_len, void *dst, uint32_t *out_len)
{
	struct peaks_data *pd = r->data;
	float *s = src, *d = dst;
	int i, o, end, chunk;

	o = i = 0;

	while (i < *in_len && o < *out_len) {
		end = ((uint64_t) (pd->o_count + 1) * r->i_rate) / r->o_rate;
		end = end > pd->i_count ? end - pd->i_count : 0;
		chunk = SPA_MIN(end, *in_len);

		for (; i < chunk; i++) {
			float n = fabsf(s[i]);
			if (n > pd->max_f[channel])
				pd->max_f[channel] = n;
		}

		if (i == end) {
			d[o++] = pd->max_f[channel];
			pd->max_f[channel] = 0.0f;
			pd->o_count++;
		}
	}
	*out_len = o;
	*in_len = i;
	pd->i_count += i;

	while (pd->i_count >= r->i_rate) {
		pd->i_count -= r->i_rate;
		pd->o_count -= r->o_rate;
	}
}

static void impl_peaks_reset (struct resample *r)
{
	struct peaks_data *d = r->data;
	d->i_count = d->o_count = 0;
}

static int impl_peaks_init(struct resample *r)
{
	struct peaks_data *d;

	r->free = impl_peaks_free;
	r->update_rate = impl_peaks_update_rate;
	r->process = impl_peaks_process;
	r->reset = impl_peaks_reset;
	d = r->data = calloc(1, sizeof(struct peaks_data) * sizeof(float) * r->channels);
	if (r->data == NULL)
		return -ENOMEM;

	d->i_count = d->o_count = 0;
	return 0;
}
