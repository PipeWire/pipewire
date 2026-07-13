/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2025 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <math.h>

#include <spa/support/log.h>

#include "gaps-ops.h"

#ifndef M_PIf
# define M_PIf  3.14159265358979323846f /* pi */
#endif

#define MODE_ZERO	0
#define MODE_NORMAL	1
#define MODE_FADE_IN	2
#define MODE_FADE_OUT	3

static int run_gap_check(struct gaps *gaps, uint32_t c, const float * SPA_RESTRICT src[], uint32_t n_samples,
		bool *empty)
{
	uint32_t n;
	bool head_filled = true, tail_filled = true;
	struct gaps_state *s = &gaps->states[c];
	const float *in = src[c];

	for (n = 0; n < SPA_MIN(gaps->gap, n_samples); n++) {
		if (in[n] == 0.0f) {
			head_filled = false;
			break;
		}
	}
	if (n_samples > gaps->gap) {
		for (n = n_samples - gaps->gap - 1; n < n_samples; n++) {
			if (in[n] == 0.0f) {
				tail_filled = false;
				break;
			}
		}
	} else {
		tail_filled = head_filled;
	}
	if (s->mode == MODE_NORMAL && head_filled && tail_filled) {
		/* in normal mode and head and tail seem to have data */
		if (n_samples > 0)
			s->history[0] = in[n_samples-1];
		*empty = false;
		return 0;
	}
	else if (s->mode == MODE_ZERO && !tail_filled && !head_filled) {
		/* zero mode and head and tail seem to be empty */
		return 0;
	}
	*empty = false;
	return 1;
}

static int run_gap_check_ramp(struct gaps *gaps, uint32_t c, const float * SPA_RESTRICT src[], uint32_t n_samples,
		bool *empty)
{
	struct gaps_state *s = &gaps->states[c];
	const float *in = src[c];

	if (s->mode == MODE_ZERO)
		s->mode = MODE_NORMAL;

	if (s->mode == MODE_NORMAL) {
		/* in normal mode remember last sample */
		if (n_samples > 0)
			s->history[0] = in[n_samples-1];
		*empty = false;
		return 0;
	}
	*empty = false;
	return 1;
}

int gaps_check_c(struct gaps *gaps, const float * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t c;
	int res = 0;
	gaps->empty = true;
	if (gaps->gap > 0) {
		for (c = 0; c < gaps->channels; c++)
			res += run_gap_check(gaps, c, src, n_samples, &gaps->empty);
	} else {
		for (c = 0; c < gaps->channels; c++)
			res += run_gap_check_ramp(gaps, c, src, n_samples, &gaps->empty);
	}
	return res;
}

static void run_gap_fix(struct gaps *gaps, uint32_t c, float * SPA_RESTRICT dst[],
		const float * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t n;
	struct gaps_state *s = &gaps->states[c];
	const float *in = src[c];
	float *out = dst[c];

	for (n = 0; n < n_samples; n++) {
		bool is_zero = in[n] == 0.0f;

		if (s->mode == MODE_ZERO) {
			/* zero mode */
			if (!is_zero) {
				/* gap ended, move to fade-in mode */
				s->mode = gaps->gap ? MODE_FADE_IN : MODE_NORMAL;
				s->count = 0;
			} else {
				out[n] = 0.0f;
			}
		}
		else if (s->mode == MODE_NORMAL) {
			out[n] = in[n];
			/* normal mode, finding gaps */
			if (is_zero && gaps->gap > 0) {
				if (++s->count >= gaps->gap) {
					n -= SPA_MIN(s->count, n);
					s->mode = MODE_FADE_OUT;
					s->count = 0;
				}
			} else {
				/* keep last samples to fade out when needed */
				s->count = 0;
				s->history[0] = in[n];
			}
		}
		if (s->mode == MODE_FADE_IN) {
			/* fade-in mode */
			if (s->count == 0)
				spa_log_info(gaps->log, "%p start %d fade-in %d", gaps, c, n);

			out[n] = in[n] * gaps->curve[s->count];

			if (++s->count >= gaps->duration) {
				/* fade in complete, back to normal mode */
				s->mode = MODE_NORMAL;
				s->count = 0;
				spa_log_debug(gaps->log, "%p stop %d fade-in %d", gaps, c, n);
			}
		}
		else if (s->mode == MODE_FADE_OUT) {
			/* fade-out mode */
			if (s->count == 0)
				spa_log_info(gaps->log, "%p start %d fade-out %f %d",
						gaps, c, s->history[0], n);

			out[n] = s->history[0] * (1.0f - gaps->curve[s->count]);

			if (++s->count >= gaps->duration) {
				/* fade out complete, go to zero mode */
				s->mode = gaps->gap ? MODE_ZERO : MODE_NORMAL;
				s->count = 0;
				spa_log_debug(gaps->log, "%p stop %d  fade-out %d", gaps, c, n);
			}
		}
	}
}

void gaps_fix_c(struct gaps *gaps, float * SPA_RESTRICT dst[],
		const float * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t c;
	for (c = 0; c < gaps->channels; c++)
		run_gap_fix(gaps, c, dst, src, n_samples);
}
