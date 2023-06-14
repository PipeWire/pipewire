/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <spa/param/audio/format.h>

#include "resample-native-impl.h"

struct quality {
	uint32_t n_taps;
	double cutoff;
};

static const struct quality window_qualities[] = {
	{ 8, 0.53, },
	{ 16, 0.67, },
	{ 24, 0.75, },
	{ 32, 0.80, },
	{ 48, 0.85, },                  /* default */
	{ 64, 0.88, },
	{ 80, 0.895, },
	{ 96, 0.910, },
	{ 128, 0.936, },
	{ 144, 0.945, },
	{ 160, 0.950, },
	{ 192, 0.960, },
	{ 256, 0.970, },
	{ 896, 0.990, },
	{ 1024, 0.995, },
};

static inline double sinc(double x)
{
	if (x < 1e-6) return 1.0;
	x *= M_PI;
	return sin(x) / x;
}

static inline double window_blackman(double x, double n_taps)
{
	double alpha = 0.232, r;
	x =  2.0 * M_PI * x / n_taps;
	r = (1.0 - alpha) / 2.0 + (1.0 / 2.0) * cos(x) +
		(alpha / 2.0) * cos(2.0 * x);
	return r;
}

static inline double window_cosh(double x, double n_taps)
{
	double r;
	double A = 16.97789;
	double x2;
	x =  2.0 * x / n_taps;
	x2 = x * x;
	if (x2 >= 1.0)
		return 0.0;
	/* doi:10.1109/RME.2008.4595727 with tweak */
	r = (exp(A * sqrt(1 - x2)) - 1) / (exp(A) - 1);
	return r;
}

#define window (1 ? window_cosh : window_blackman)

static int build_filter(float *taps, uint32_t stride, uint32_t n_taps, uint32_t n_phases, double cutoff)
{
	uint32_t i, j, n_taps12 = n_taps/2;

	for (i = 0; i <= n_phases; i++) {
		double t = (double) i / (double) n_phases;
		for (j = 0; j < n_taps12; j++, t += 1.0) {
			/* exploit symmetry in filter taps */
			taps[(n_phases - i) * stride + n_taps12 + j] =
				taps[i * stride + (n_taps12 - j - 1)] =
					cutoff * sinc(t * cutoff) * window(t, n_taps);
		}
	}
	return 0;
}

MAKE_RESAMPLER_COPY(c);

#define MAKE(fmt,copy,full,inter,...) \
	{ SPA_AUDIO_FORMAT_ ##fmt, do_resample_ ##copy, #copy, \
		do_resample_ ##full, #full, do_resample_ ##inter, #inter, __VA_ARGS__ }

static struct resample_info resample_table[] =
{
#if defined (HAVE_NEON)
	MAKE(F32, copy_c, full_neon, inter_neon, SPA_CPU_FLAG_NEON),
#endif
#if defined(HAVE_AVX) && defined(HAVE_FMA)
	MAKE(F32, copy_c, full_avx, inter_avx, SPA_CPU_FLAG_AVX | SPA_CPU_FLAG_FMA3),
#endif
#if defined (HAVE_SSSE3)
	MAKE(F32, copy_c, full_ssse3, inter_ssse3, SPA_CPU_FLAG_SSSE3 | SPA_CPU_FLAG_SLOW_UNALIGNED),
#endif
#if defined (HAVE_SSE)
	MAKE(F32, copy_c, full_sse, inter_sse, SPA_CPU_FLAG_SSE),
#endif
	MAKE(F32, copy_c, full_c, inter_c),
};
#undef MAKE

#define MATCH_CPU_FLAGS(a,b)	((a) == 0 || ((a) & (b)) == a)
static const struct resample_info *find_resample_info(uint32_t format, uint32_t cpu_flags)
{
	SPA_FOR_EACH_ELEMENT_VAR(resample_table, t) {
		if (t->format == format &&
		    MATCH_CPU_FLAGS(t->cpu_flags, cpu_flags))
			return t;
	}
	return NULL;
}

static void impl_native_free(struct resample *r)
{
	spa_log_debug(r->log, "native %p: free", r);
	free(r->data);
	r->data = NULL;
}

static inline uint32_t calc_gcd(uint32_t a, uint32_t b)
{
	while (b != 0) {
		uint32_t temp = a;
		a = b;
		b = temp % b;
	}
	return a;
}

static void impl_native_update_rate(struct resample *r, double rate)
{
	struct native_data *data = r->data;
	uint32_t in_rate, out_rate, phase, gcd, old_out_rate;

	if (SPA_LIKELY(data->rate == rate))
		return;

	old_out_rate = data->out_rate;
	in_rate = r->i_rate / rate;
	out_rate = r->o_rate;
	phase = data->phase;

	gcd = calc_gcd(in_rate, out_rate);
	in_rate /= gcd;
	out_rate /= gcd;

	data->rate = rate;
	data->phase = phase * out_rate / old_out_rate;
	data->in_rate = in_rate;
	data->out_rate = out_rate;

	data->inc = data->in_rate / data->out_rate;
	data->frac = data->in_rate % data->out_rate;

	if (data->in_rate == data->out_rate) {
		data->func = data->info->process_copy;
		r->func_name = data->info->copy_name;
	}
	else if (rate == 1.0) {
		data->func = data->info->process_full;
		r->func_name = data->info->full_name;
	}
	else {
		data->func = data->info->process_inter;
		r->func_name = data->info->inter_name;
	}

	spa_log_trace_fp(r->log, "native %p: rate:%f in:%d out:%d gcd:%d phase:%d inc:%d frac:%d", r,
			rate, r->i_rate, r->o_rate, gcd, data->phase, data->inc, data->frac);

}

static uint32_t impl_native_in_len(struct resample *r, uint32_t out_len)
{
	struct native_data *data = r->data;
	uint32_t in_len;

	in_len = (data->phase + out_len * data->frac) / data->out_rate;
	in_len += out_len * data->inc +	(data->n_taps - data->hist);

	spa_log_trace_fp(r->log, "native %p: hist:%d %d->%d", r, data->hist, out_len, in_len);

	return in_len;
}

static void impl_native_process(struct resample *r,
		const void * SPA_RESTRICT src[], uint32_t *in_len,
		void * SPA_RESTRICT dst[], uint32_t *out_len)
{
	struct native_data *data = r->data;
	uint32_t n_taps = data->n_taps;
	float **history = data->history;
	const float **s = (const float **)src;
	uint32_t c, refill, hist, in, out, remain;

	hist = data->hist;
	refill = 0;

	if (SPA_LIKELY(hist)) {
		/* first work on the history if any. */
		if (SPA_UNLIKELY(hist <= n_taps)) {
			/* we need at least n_taps to completely process the
			 * history before we can work on the new input. When
			 * we have less, refill the history. */
			refill = SPA_MIN(*in_len, n_taps-1);
			for (c = 0; c < r->channels; c++)
				spa_memcpy(&history[c][hist], s[c], refill * sizeof(float));

			if (SPA_UNLIKELY(hist + refill < n_taps)) {
				/* not enough in the history, keep the input in
				 * the history and produce no output */
				data->hist = hist + refill;
				*in_len = refill;
				*out_len = 0;
				return;
			}
		}
		/* now we have at least n_taps of data in the history
		 * and we try to process it */
		in = hist + refill;
		out = *out_len;
		data->func(r, (const void**)history, 0, &in, dst, 0, &out);
		spa_log_trace_fp(r->log, "native %p: in:%d/%d out %d/%d hist:%d",
				r, hist + refill, in, *out_len, out, hist);
	} else {
		out = in = 0;
	}

	if (SPA_LIKELY(in >= hist)) {
		int skip = in - hist;
		/* we are past the history and can now work on the new
		 * input data */
		in = *in_len;
		data->func(r, src, skip, &in, dst, out, out_len);

		spa_log_trace_fp(r->log, "native %p: in:%d/%d out %d/%d skip:%d",
				r, *in_len, in, *out_len, out, skip);

		remain = *in_len - in;
		if (remain > 0 && remain <= n_taps) {
			/* not enough input data remaining for more output,
			 * copy to history */
			for (c = 0; c < r->channels; c++)
				spa_memcpy(history[c], &s[c][in], remain * sizeof(float));
		} else {
			/* we have enough input data remaining to produce
			 * more output ask to resubmit. */
			remain = 0;
			*in_len = in;
		}
	} else {
		/* we are still working on the history */
		*out_len = out;
		remain = hist - in;
		if (*in_len < n_taps) {
			/* not enough input data, add it to the history because
			 * resubmitting it is not going to make progress.
			 * We copied this into the history above. */
			remain += refill;
		} else {
			/* input has enough data to possibly produce more output
			 * from the history so ask to resubmit */
			*in_len = 0;
		}
		if (remain) {
			/* move history */
			for (c = 0; c < r->channels; c++)
				spa_memmove(history[c], &history[c][in], remain * sizeof(float));
		}
		spa_log_trace_fp(r->log, "native %p: in:%d remain:%d", r, in, remain);

	}
	data->hist = remain;
	return;
}

static void impl_native_reset (struct resample *r)
{
	struct native_data *d = r->data;
	if (d == NULL)
		return;
	memset(d->hist_mem, 0, r->channels * sizeof(float) * d->n_taps * 2);
	if (r->options & RESAMPLE_OPTION_PREFILL)
		d->hist = d->n_taps - 1;
	else
		d->hist = (d->n_taps / 2) - 1;
	d->phase = 0;
}

static uint32_t impl_native_delay (struct resample *r)
{
	struct native_data *d = r->data;
	return d->n_taps / 2;
}

int resample_native_init(struct resample *r)
{
	struct native_data *d;
	const struct quality *q;
	double scale;
	uint32_t c, n_taps, n_phases, filter_size, in_rate, out_rate, gcd, filter_stride;
	uint32_t history_stride, history_size, oversample;

	r->quality = SPA_CLAMP(r->quality, 0, (int) SPA_N_ELEMENTS(window_qualities) - 1);
	r->free = impl_native_free;
	r->update_rate = impl_native_update_rate;
	r->in_len = impl_native_in_len;
	r->process = impl_native_process;
	r->reset = impl_native_reset;
	r->delay = impl_native_delay;

	q = &window_qualities[r->quality];

	gcd = calc_gcd(r->i_rate, r->o_rate);

	in_rate = r->i_rate / gcd;
	out_rate = r->o_rate / gcd;

	scale = SPA_MIN(q->cutoff * out_rate / in_rate, q->cutoff);

	/* multiple of 8 taps to ease simd optimizations */
	n_taps = SPA_ROUND_UP_N((uint32_t)ceil(q->n_taps / scale), 8);
	n_taps = SPA_MIN(n_taps, 1u << 18);

	/* try to get at least 256 phases so that interpolation is
	 * accurate enough when activated */
	n_phases = out_rate;
	oversample = (255 + n_phases) / n_phases;
	n_phases *= oversample;

	filter_stride = SPA_ROUND_UP_N(n_taps * sizeof(float), 64);
	filter_size = filter_stride * (n_phases + 1);
	history_stride = SPA_ROUND_UP_N(2 * n_taps * sizeof(float), 64);
	history_size = r->channels * history_stride;

	d = calloc(1, sizeof(struct native_data) +
			filter_size +
			history_size +
			(r->channels * sizeof(float*)) +
			64);

	if (d == NULL)
		return -errno;

	r->data = d;
	d->n_taps = n_taps;
	d->n_phases = n_phases;
	d->in_rate = in_rate;
	d->out_rate = out_rate;
	d->filter = SPA_PTROFF_ALIGN(d, sizeof(struct native_data), 64, float);
	d->hist_mem = SPA_PTROFF_ALIGN(d->filter, filter_size, 64, float);
	d->history = SPA_PTROFF(d->hist_mem, history_size, float*);
	d->filter_stride = filter_stride / sizeof(float);
	d->filter_stride_os = d->filter_stride * oversample;
	for (c = 0; c < r->channels; c++)
		d->history[c] = SPA_PTROFF(d->hist_mem, c * history_stride, float);

	build_filter(d->filter, d->filter_stride, n_taps, n_phases, scale);

	d->info = find_resample_info(SPA_AUDIO_FORMAT_F32, r->cpu_flags);
	if (SPA_UNLIKELY(d->info == NULL)) {
	    spa_log_error(r->log, "failed to find suitable resample format!");
	    return -ENOTSUP;
	}

	spa_log_debug(r->log, "native %p: q:%d in:%d out:%d gcd:%d n_taps:%d n_phases:%d features:%08x:%08x",
			r, r->quality, r->i_rate, r->o_rate, gcd, n_taps, n_phases,
			r->cpu_flags, d->info->cpu_flags);

	r->cpu_flags = d->info->cpu_flags;

	impl_native_reset(r);
	impl_native_update_rate(r, 1.0);

	return 0;
}
