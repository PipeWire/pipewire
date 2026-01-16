/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <spa/param/audio/format.h>

#include "resample-native-impl.h"
#ifndef RESAMPLE_DISABLE_PRECOMP
#include "resample-native-precomp.h"
#endif

SPA_LOG_TOPIC_DEFINE(resample_log_topic, "spa.resample");

#define MAX_TAPS	(1u<<18)
#define MAX_PHASES	1024u

#define INHERIT_PARAM(c,q,p)	if ((c)->params[p] == 0.0) (c)->params[p] = (q)->params[p];

struct quality {
	uint32_t n_taps;
	double cutoff_up;		/* when upsampling */
	double cutoff_down;		/* for downsampling */
	double params[RESAMPLE_MAX_PARAMS];
};


struct window_info {
	uint32_t window;
	void (*func) (struct resample *r, double *w, double t, uint32_t n_taps);
	uint32_t n_qualities;
	const struct quality *qualities;
	void (*config) (struct resample *r);
};
struct window_info window_info[];

static const struct quality blackman_qualities[] = {
	{ 8, 0.58, 0.58, { 0.16, }},
	{ 16, 0.70, 0.70, { 0.20, }},
	{ 24, 0.77, 0.77, { 0.16, }},
	{ 32, 0.82, 0.82, { 0.16, }},
	{ 48, 0.87, 0.87, { 0.16, }},                  /* default */
	{ 64, 0.895, 0.895, { 0.16, }},
	{ 80, 0.910, 0.910, { 0.16, }},
	{ 96, 0.925, 0.925, { 0.16, }},
	{ 128, 0.942, 0.942, { 0.16, }},
	{ 144, 0.950, 0.950, { 0.16, }},
	{ 160, 0.958, 0.958, { 0.16, }},
	{ 192, 0.966, 0.966, { 0.16, }},
	{ 256, 0.975, 0.975, { 0.16, }},
	{ 896, 0.988, 0.988, { 0.16, }},
	{ 1024, 0.990, 0.990, { 0.16, }},
};

static inline void blackman_window(struct resample *r, double *w, double t, uint32_t n_taps)
{
	double x, alpha = r->config.params[RESAMPLE_PARAM_BLACKMAN_ALPHA];
	uint32_t i, n_taps12 = n_taps/2;
	for (i = 0; i < n_taps12; i++, t += 1.0) {
		x =  2.0 * M_PI * t / n_taps;
		w[i] = (1.0 - alpha) / 2.0 + (1.0 / 2.0) * cos(x) +
			(alpha / 2.0) * cos(2.0 * x);
	}
}
static inline void blackman_config(struct resample *r)
{
	const struct quality *q = &window_info[r->config.window].qualities[r->quality];
	INHERIT_PARAM(&r->config, q, RESAMPLE_PARAM_BLACKMAN_ALPHA);
}

static const struct quality exp_qualities[] = {
	{ 8, 0.58, 0.58, { 16.97789, }},
	{ 16, 0.70, 0.70, { 16.97789, }},
	{ 24, 0.77, 0.77, { 16.97789, }},
	{ 32, 0.82, 0.82, { 16.97789, }},
	{ 48, 0.87, 0.87, { 16.97789, }},                  /* default */
	{ 64, 0.895, 0.895, { 16.97789, }},
	{ 80, 0.910, 0.910, { 16.97789, }},
	{ 96, 0.925, 0.925, { 16.97789, }},
	{ 128, 0.942, 0.942, { 16.97789, }},
	{ 144, 0.950, 0.950, { 16.97789, }},
	{ 160, 0.958, 0.958, { 16.97789, }},
	{ 192, 0.966, 0.966, { 16.97789, }},
	{ 256, 0.975, 0.975, { 16.97789, }},
	{ 896, 0.988, 0.988, { 16.97789, }},
	{ 1024, 0.990, 0.990, { 16.97789, }},
};

static inline void exp_window(struct resample *r, double *w, double t, uint32_t n_taps)
{
	double x, A = r->config.params[RESAMPLE_PARAM_EXP_A];
	uint32_t i, n_taps12 = n_taps/2;

	for (i = 0; i < n_taps12; i++, t += 1.0) {
		x =  2.0 * t / n_taps;
		/* doi:10.1109/RME.2008.4595727 with tweak */
		w[i] = (exp(A * sqrt(fmax(0.0, 1.0 - x*x))) - 1) / (exp(A) - 1);
	}
}
static inline void exp_config(struct resample *r)
{
	const struct quality *q = &window_info[r->config.window].qualities[r->quality];
	INHERIT_PARAM(&r->config, q, RESAMPLE_PARAM_EXP_A);
}

#include "dbesi0.c"

static const struct quality kaiser_qualities[] = {
	{ 8, 0.620000, 0.620000, { 3.553376, 110.000000, 0.888064 }},
	{ 16, 0.780000, 0.780000, { 3.553376, 110.000000, 0.444032 }},
	{ 24, 0.820000, 0.820000, { 3.904154, 120.000000, 0.325043 }},
	{ 32, 0.865000, 0.865000, { 4.254931, 130.000000, 0.265548 }},
	{ 48, 0.895000, 0.895000, { 4.254931, 130.000000, 0.177032 }},
	{ 64, 0.915000, 0.915000, { 4.254931, 130.000000, 0.132774 }},
	{ 80, 0.928000, 0.928000, { 4.254931, 130.000000, 0.106219 }},
	{ 96, 0.942000, 0.942000, { 4.254931, 130.000000, 0.088516 }},
	{ 128, 0.952000, 0.952000, { 4.254931, 130.000000, 0.066387 }},
	{ 160, 0.960000, 0.960000, { 4.254931, 130.000000, 0.053110 }},
	{ 192, 0.968000, 0.968000, { 4.254931, 130.000000, 0.044258 }},
	{ 256, 0.976000, 0.976000, { 4.605709, 140.000000, 0.035914 }},
	{ 512, 0.985000, 0.985000, { 4.781097, 145.000000, 0.018637 }},
	{ 768, 0.990000, 0.990000, { 4.956486, 150.000000, 0.012878 }},
	{ 1024, 0.993000, 0.993000, { 5.131875, 155.000000, 0.009999 }},
};

static inline void kaiser_window(struct resample *r, double *w, double t, uint32_t n_taps)
{
	double x, beta = r->config.params[RESAMPLE_PARAM_KAISER_ALPHA] * M_PI;
	double den = dbesi0(beta);
	uint32_t i, n_taps12 = n_taps/2;
	for (i = 0; i < n_taps12; i++, t += 1.0) {
		x = 2.0 * t / n_taps;
		w[i] = dbesi0(beta * sqrt(fmax(0.0, 1.0 - x*x))) / den;
	}
}

static inline void kaiser_config(struct resample *r)
{
	double A, B, dw, tr_bw, alpha;
	uint32_t n;
	const struct quality *q = &window_info[r->config.window].qualities[r->quality];

	if ((A = r->config.params[RESAMPLE_PARAM_KAISER_SB_ATT]) == 0.0)
		A = q->params[RESAMPLE_PARAM_KAISER_SB_ATT];
	if ((tr_bw = r->config.params[RESAMPLE_PARAM_KAISER_TR_BW]) == 0.0)
		tr_bw = q->params[RESAMPLE_PARAM_KAISER_TR_BW];

	if ((alpha = r->config.params[RESAMPLE_PARAM_KAISER_ALPHA]) == 0.0) {
		/* calculate Beta and alpha */
		if (A > 50)
			B = 0.1102 * (A - 8.7);
		else if (A >= 21)
			B = 0.5842 * pow (A - 21, 0.4) + 0.07886 * (A - 21);
		else
			B = 0.0;

		r->config.params[RESAMPLE_PARAM_KAISER_ALPHA] = B / M_PI;
	}
	if (r->config.n_taps == 0) {
		/* calculate transition width in radians */
		dw = 2 * M_PI * (tr_bw);
		/* order of the filter */
		n = (uint32_t)((A - 8.0) / (2.285 * dw));
		r->config.n_taps = n + 1;
	}
}

struct window_info window_info[] = {
	[RESAMPLE_WINDOW_EXP] = { RESAMPLE_WINDOW_EXP, exp_window,
		SPA_N_ELEMENTS(exp_qualities), exp_qualities, exp_config },
	[RESAMPLE_WINDOW_BLACKMAN] = { RESAMPLE_WINDOW_BLACKMAN, blackman_window,
		SPA_N_ELEMENTS(blackman_qualities), blackman_qualities, blackman_config },
	[RESAMPLE_WINDOW_KAISER] = { RESAMPLE_WINDOW_KAISER, kaiser_window,
		SPA_N_ELEMENTS(kaiser_qualities), kaiser_qualities, kaiser_config },
};

static inline double sinc(double x, double cutoff)
{
	if (x < 1e-6) return cutoff;
	x *= M_PI;
	return sin(x * cutoff) / x;
}

static int build_filter(struct resample *r, float *taps, uint32_t stride, uint32_t n_taps,
		uint32_t n_phases, double cutoff)
{
	uint32_t i, j, n_taps12 = n_taps/2;
	double window[n_taps12+1];

	for (i = 0; i <= n_phases; i++) {
		double t = (double) i / (double) n_phases;
		window_info[r->config.window].func(r, window, t, n_taps);
		for (j = 0; j < n_taps12; j++, t += 1.0) {
			/* exploit symmetry in filter taps */
			taps[(n_phases - i) * stride + n_taps12 + j] =
				taps[i * stride + (n_taps12 - j - 1)] = (float)
					(sinc(t, cutoff) * window[j]);
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
#if defined(HAVE_AVX2) && defined(HAVE_FMA)
	MAKE(F32, copy_c, full_avx2, inter_avx2, SPA_CPU_FLAG_AVX2 | SPA_CPU_FLAG_FMA3),
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
	struct fixp in_rate;
	uint32_t out_rate;

	if (SPA_LIKELY(data->rate == rate))
		return;

	data->rate = rate;
	in_rate = UINT32_TO_FIXP(r->i_rate);
	out_rate = r->o_rate;

	if (rate != 1.0 || data->force_inter) {
		in_rate.value = (uint64_t)round(in_rate.value / rate);
		data->func = data->info->process_inter;
	}
	else if (in_rate.value == UINT32_TO_FIXP(out_rate).value) {
		data->func = data->info->process_copy;
	}
	else {
		in_rate.value /= data->gcd;
		out_rate /= data->gcd;
		data->func = data->info->process_full;
	}

	if (data->out_rate != out_rate) {
		/* Cast to double to avoid overflows */
		data->phase.value = (uint64_t)(data->phase.value * (double)out_rate / data->out_rate);
		if (data->phase.value >= UINT32_TO_FIXP(out_rate).value)
			data->phase.value = UINT32_TO_FIXP(out_rate).value - 1;
	}

	data->in_rate = in_rate;
	data->out_rate = out_rate;

	data->inc = in_rate.value / UINT32_TO_FIXP(out_rate).value;
	data->frac.value = in_rate.value % UINT32_TO_FIXP(out_rate).value;

	spa_log_trace_fp(r->log, "native %p: rate:%f in:%d out:%d phase:%f inc:%d frac:%f", r,
			rate, r->i_rate, r->o_rate, FIXP_TO_FLOAT(data->phase),
			data->inc, FIXP_TO_FLOAT(data->frac));
}

static uint64_t fixp_floor_a_plus_bc(struct fixp a, uint32_t b, struct fixp c)
{
	/* (a + b*c) >> FIXP_SHIFT, with bigger overflow threshold */
	uint64_t hi, lo;
	hi = (a.value >> FIXP_SHIFT) + b * (c.value >> FIXP_SHIFT);
	lo = (a.value & FIXP_MASK) + b * (c.value & FIXP_MASK);
	return hi + (lo >> FIXP_SHIFT);
}

static uint32_t impl_native_in_len(struct resample *r, uint32_t out_len)
{
	struct native_data *data = r->data;
	uint32_t in_len;

	in_len = fixp_floor_a_plus_bc(data->phase, out_len, data->frac) / data->out_rate;
	in_len += out_len * data->inc +	(data->n_taps - data->hist);

	spa_log_trace_fp(r->log, "native %p: hist:%d %d->%d", r, data->hist, out_len, in_len);

	return in_len;
}

static uint32_t impl_native_out_len(struct resample *r, uint32_t in_len)
{
	struct native_data *data = r->data;
	uint32_t out_len;

	in_len = in_len - SPA_MIN(in_len, data->n_taps - data->hist);
	out_len = in_len * data->out_rate - FIXP_TO_UINT32(data->phase);
	out_len = (UINT32_TO_FIXP(out_len).value + data->in_rate.value - 1) / data->in_rate.value;

	spa_log_trace_fp(r->log, "native %p: hist:%d %d->%d", r, data->hist, in_len, out_len);

	return out_len;
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
		d->hist = d->n_taps / 2;
	d->phase.value = 0;
}

static uint32_t impl_native_delay (struct resample *r)
{
	struct native_data *d = r->data;
	return d->n_taps / 2 - 1;
}

static float impl_native_phase (struct resample *r)
{
	struct native_data *d = r->data;
	float pho = 0;

	if (d->func == d->info->process_full) {
		pho = -(float)FIXP_TO_UINT32(d->phase) / d->out_rate;

		/* XXX: this is how it seems to behave, but not clear why */
		if (d->hist >= d->n_taps - 1)
			pho += 1.0f;
	} else if (d->func == d->info->process_inter) {
		pho = -FIXP_TO_FLOAT(d->phase) / d->out_rate;

		/* XXX: this is how it seems to behave, but not clear why */
		if (d->hist >= d->n_taps - 1)
			pho += 1.0f;
	}

	return pho;
}

int resample_native_init(struct resample *r)
{
	struct native_data *d;
	const struct quality *q;
	double scale, cutoff;
	uint32_t i, n_taps, n_phases, filter_size, in_rate, out_rate, gcd, filter_stride;
	uint32_t history_stride, history_size, oversample;
	struct resample_config *c = &r->config;
#ifndef RESAMPLE_DISABLE_PRECOMP
	struct resample_config def = { 0 };
	bool default_config;

	default_config = memcmp(c, &def, sizeof(def)) == 0;
#endif
	c->window = SPA_CLAMP(c->window, 0u, SPA_N_ELEMENTS(window_info)-1);
	r->quality = SPA_CLAMP(r->quality, 0, (int)(window_info[c->window].n_qualities - 1));
	r->free = impl_native_free;
	r->update_rate = impl_native_update_rate;
	r->in_len = impl_native_in_len;
	r->out_len = impl_native_out_len;
	r->process = impl_native_process;
	r->reset = impl_native_reset;
	r->delay = impl_native_delay;
	r->phase = impl_native_phase;

	window_info[c->window].config(r);

	q = &window_info[c->window].qualities[r->quality];
	cutoff = r->o_rate < r->i_rate ? q->cutoff_down : q->cutoff_up;
	c->cutoff = c->cutoff <= 0.0 ? cutoff: c->cutoff;
	n_taps = c->n_taps == 0 ? q->n_taps : c->n_taps;

	gcd = calc_gcd(r->i_rate, r->o_rate);

	in_rate = r->i_rate / gcd;
	out_rate = r->o_rate / gcd;

	scale = SPA_MIN(c->cutoff * out_rate / in_rate, c->cutoff);

	/* multiple of 8 taps to ease simd optimizations */
	n_taps = SPA_ROUND_UP_N((uint32_t)ceil(n_taps / scale), 8);
	n_taps = SPA_MIN(n_taps, MAX_TAPS);

	/* try to get at least 256 phases so that interpolation is
	 * accurate enough when activated */
	n_phases = SPA_MIN(MAX_PHASES, out_rate);
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
	c->n_taps = d->n_taps = n_taps;
	d->n_phases = n_phases;
	d->in_rate = UINT32_TO_FIXP(in_rate);
	d->out_rate = out_rate;
	d->force_inter = out_rate > n_phases;
	d->gcd = gcd;
	d->pm = (float)n_phases / r->o_rate / FIXP_SCALE;
	d->filter = SPA_PTROFF_ALIGN(d, sizeof(struct native_data), 64, float);
	d->hist_mem = SPA_PTROFF_ALIGN(d->filter, filter_size, 64, float);
	d->history = SPA_PTROFF(d->hist_mem, history_size, float*);
	d->filter_stride = filter_stride / sizeof(float);
	d->filter_stride_os = d->filter_stride * oversample;
	for (i = 0; i < r->channels; i++)
		d->history[i] = SPA_PTROFF(d->hist_mem, i * history_stride, float);

#ifndef RESAMPLE_DISABLE_PRECOMP
	/* See if we have precomputed coefficients */
	for (i = 0; precomp_coeffs[i].filter; i++) {
		if (default_config &&
		    precomp_coeffs[i].in_rate == r->i_rate &&
		    precomp_coeffs[i].out_rate == r->o_rate &&
		    precomp_coeffs[i].quality == r->quality)
			break;
	}

	if (precomp_coeffs[i].filter) {
		spa_log_info(r->log, "using precomputed filter for %u->%u(%u)",
				r->i_rate, r->o_rate, r->quality);
		spa_memcpy(d->filter, precomp_coeffs[i].filter, filter_size);
	} else {
#endif
		build_filter(r, d->filter, d->filter_stride, n_taps, n_phases, scale);
#ifndef RESAMPLE_DISABLE_PRECOMP
	}
#endif

	d->info = find_resample_info(SPA_AUDIO_FORMAT_F32, r->cpu_flags);
	if (SPA_UNLIKELY(d->info == NULL)) {
	    spa_log_error(r->log, "failed to find suitable resample format!");
	    return -ENOTSUP;
	}

	spa_log_info(r->log, "native %p: c:%f q:%d w:%d in:%d out:%d gcd:%d n_taps:%d n_phases:%d features:%08x:%08x",
			r, c->cutoff, r->quality, c->window, r->i_rate, r->o_rate, gcd, n_taps, n_phases,
			r->cpu_flags, d->info->cpu_flags);

	r->cpu_flags = d->info->cpu_flags;

	impl_native_reset(r);
	impl_native_update_rate(r, 1.0);

	if (d->func == d->info->process_copy)
		r->func_name = d->info->copy_name;
	else if (d->func == d->info->process_full)
		r->func_name = d->info->full_name;
	else
		r->func_name = d->info->inter_name;

	return 0;
}
