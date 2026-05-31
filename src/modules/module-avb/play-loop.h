/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-License-Identifier: MIT */

/*
 * play-loop.h — consume-side actuator for the listener.
 *
 * Pure (no PipeWire deps) so it can be unit-tested like mc-recover.h. Keeps the
 * listener ring at a target fill by trimming the output resampler ratio
 * (SPA_IO_RateMatch). Same loop as module-rtp's receiver:
 *   error = target - avail; corr = spa_dll_update(dll, error); rate = ff / corr
 * ff = nominal/recovered rate feeds the recovered clock forward; the DLL trims
 * the rest. The sign is the trap (same as the old mc_recover bug); test_play_loop
 * locks it: converges on the right sign, diverges on the wrong one.
 */

#ifndef AVB_PLAY_LOOP_H
#define AVB_PLAY_LOOP_H

#include <stdbool.h>

#include <spa/utils/defs.h>
#include <spa/utils/dll.h>

struct play_loop {
	bool init;
	struct spa_dll dll;
	double corr;	/* DLL output, ~1.0 */
	double rate;	/* last applied resampler ratio (ff / corr) */
};

static inline void play_loop_reset(struct play_loop *p)
{
	p->init = false;
	p->corr = 1.0;
	p->rate = 1.0;
}

/* One step. error_samples = target - avail; max_error clamps a transient;
 * ff_ratio = nominal/recovered rate (1.0 = no feedforward). Returns the ratio
 * to apply via pw_stream_set_rate(). */
static inline double play_loop_update(struct play_loop *p, double error_samples,
		double max_error, double ff_ratio, unsigned period, unsigned rate)
{
	if (!p->init) {
		spa_dll_init(&p->dll);
		spa_dll_set_bw(&p->dll, SPA_DLL_BW_MIN, period, rate);
		p->corr = 1.0;
		p->rate = ff_ratio;
		p->init = true;
	}
	error_samples = SPA_CLAMPD(error_samples, -max_error, max_error);
	p->corr = spa_dll_update(&p->dll, error_samples);
	/* clamp ±10 %, guards 1/corr */
	if (p->corr < 0.9)
		p->corr = 0.9;
	else if (p->corr > 1.1)
		p->corr = 1.1;
	p->rate = ff_ratio / p->corr;
	return p->rate;
}

#endif /* AVB_PLAY_LOOP_H */
