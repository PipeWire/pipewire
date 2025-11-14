/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef RESAMPLE_H
#define RESAMPLE_H

#include <spa/support/cpu.h>
#include <spa/support/log.h>

#define RESAMPLE_DEFAULT_QUALITY	4

struct resample_config {
#define RESAMPLE_WINDOW_DEFAULT		0
#define RESAMPLE_WINDOW_EXP		1
#define RESAMPLE_WINDOW_BLACKMAN	2
#define RESAMPLE_WINDOW_KAISER		3
	uint32_t window;

	uint32_t n_taps;
	double cutoff;

	union {
		double params[32];
		struct {
			double A;
		} exp_params;
		struct {
			double alpha;
		} blackman_params;
		struct {
			double stopband_attenuation;
			double transition_bandwidth;
			double alpha;
		} kaiser_params;
	};
};

struct resample {
	struct spa_log *log;
#define RESAMPLE_OPTION_PREFILL		(1<<0)
	uint32_t options;
	uint32_t cpu_flags;
	const char *func_name;

	uint32_t channels;
	uint32_t i_rate;
	uint32_t o_rate;
	double rate;
	int quality;

	struct resample_config config;

	void (*free)		(struct resample *r);
	void (*update_rate)	(struct resample *r, double rate);
	uint32_t (*in_len)	(struct resample *r, uint32_t out_len);
	uint32_t (*out_len)	(struct resample *r, uint32_t in_len);
	void (*process)		(struct resample *r,
				 const void * SPA_RESTRICT src[], uint32_t *in_len,
				 void * SPA_RESTRICT dst[], uint32_t *out_len);
	void (*reset)		(struct resample *r);
	uint32_t (*delay)	(struct resample *r);

	/** Fractional part of delay (in input samples) */
	float (*phase)		(struct resample *r);

	void *data;
};

#define resample_free(r)		(r)->free(r)
#define resample_update_rate(r,...)	(r)->update_rate(r,__VA_ARGS__)
#define resample_in_len(r,...)		(r)->in_len(r,__VA_ARGS__)
#define resample_out_len(r,...)		(r)->out_len(r,__VA_ARGS__)
#define resample_process(r,...)		(r)->process(r,__VA_ARGS__)
#define resample_reset(r)		(r)->reset(r)
#define resample_delay(r)		(r)->delay(r)
#define resample_phase(r)		(r)->phase(r)

int resample_native_init(struct resample *r);
int resample_native_init_config(struct resample *r, struct resample_config *conf);
int resample_peaks_init(struct resample *r);

static const struct resample_window_info {
	uint32_t window;
	const char *label;
	const char *description;
} resample_window_info[] = {
	[RESAMPLE_WINDOW_DEFAULT] = { RESAMPLE_WINDOW_DEFAULT,
		"default", "Default window", },
	[RESAMPLE_WINDOW_EXP] = { RESAMPLE_WINDOW_EXP,
		"exponential", "Exponential window", },
	[RESAMPLE_WINDOW_BLACKMAN] = { RESAMPLE_WINDOW_BLACKMAN,
		"blackman", "Blackman window", },
	[RESAMPLE_WINDOW_KAISER] = { RESAMPLE_WINDOW_KAISER,
		"kaiser", "Kaiser window", },
};

static inline uint32_t resample_window_from_label(const char *label)
{
	SPA_FOR_EACH_ELEMENT_VAR(resample_window_info, i) {
		if (spa_streq(i->label, label))
			return i->window;
	}
	return RESAMPLE_WINDOW_EXP;
}


#endif /* RESAMPLE_H */
