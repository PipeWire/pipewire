/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef RESAMPLE_H
#define RESAMPLE_H

#include <spa/support/cpu.h>
#include <spa/support/log.h>

#ifndef RESAMPLE_DEFAULT_QUALITY
#define RESAMPLE_DEFAULT_QUALITY	4
#endif

#define RESAMPLE_WINDOW_DEFAULT		RESAMPLE_WINDOW_EXP
#define RESAMPLE_MAX_PARAMS		16

struct resample_config {
#define RESAMPLE_WINDOW_EXP		0
#define RESAMPLE_WINDOW_BLACKMAN	1
#define RESAMPLE_WINDOW_KAISER		2
	uint32_t window;

	double cutoff;
	uint32_t n_taps;

#define RESAMPLE_PARAM_EXP_A			0
#define RESAMPLE_PARAM_BLACKMAN_ALPHA		0
#define RESAMPLE_PARAM_KAISER_ALPHA		0
#define RESAMPLE_PARAM_KAISER_SB_ATT		1	/* stopband attenuation */
#define RESAMPLE_PARAM_KAISER_TR_BW		2	/* transition bandwidth */
#define RESAMPLE_PARAM_INVALID			(RESAMPLE_MAX_PARAMS-1)
	double params[RESAMPLE_MAX_PARAMS];
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

	struct resample_config config;	/* set to all 0 for defaults */

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
	uint32_t n_params;
} resample_window_info[] = {
	[RESAMPLE_WINDOW_EXP] = { RESAMPLE_WINDOW_EXP,
		"exp", "Exponential window", 1 },
	[RESAMPLE_WINDOW_BLACKMAN] = { RESAMPLE_WINDOW_BLACKMAN,
		"blackman", "Blackman window", 1 },
	[RESAMPLE_WINDOW_KAISER] = { RESAMPLE_WINDOW_KAISER,
		"kaiser", "Kaiser window", 3 },
};

static inline uint32_t resample_window_from_label(const char *label)
{
	SPA_FOR_EACH_ELEMENT_VAR(resample_window_info, i) {
		if (spa_streq(i->label, label))
			return i->window;
	}
	return RESAMPLE_WINDOW_EXP;
}

static inline const char *resample_window_name(uint32_t idx)
{
	return resample_window_info[SPA_CLAMP(idx, 0u, SPA_N_ELEMENTS(resample_window_info)-1)].label;
}

static const struct resample_param_info {
	uint32_t window;
	uint32_t idx;
	const char *label;
} resample_param_info[] = {
	{ RESAMPLE_WINDOW_EXP, RESAMPLE_PARAM_EXP_A, "exp.A" },
	{ RESAMPLE_WINDOW_BLACKMAN, RESAMPLE_PARAM_BLACKMAN_ALPHA, "blackman.alpha" },
	{ RESAMPLE_WINDOW_KAISER, RESAMPLE_PARAM_KAISER_ALPHA, "kaiser.alpha" },
	{ RESAMPLE_WINDOW_KAISER, RESAMPLE_PARAM_KAISER_SB_ATT, "kaiser.stopband-attenuation" },
	{ RESAMPLE_WINDOW_KAISER, RESAMPLE_PARAM_KAISER_TR_BW, "kaiser.transition-bandwidth" },
};

static inline uint32_t resample_param_from_label(const char *label)
{
	SPA_FOR_EACH_ELEMENT_VAR(resample_param_info, i) {
		if (spa_streq(i->label, label))
			return i->idx;
	}
	return RESAMPLE_PARAM_INVALID;
}

#endif /* RESAMPLE_H */
