/* Spa Bluez5 rate control */
/* SPDX-FileCopyrightText: Copyright © 2022 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_RATE_CONTROL_H
#define SPA_BLUEZ5_RATE_CONTROL_H

#include <spa/utils/defs.h>

/** Windowed min/max */
struct spa_bt_ptp
{
	union {
		int32_t min;
		int32_t mins[4];
	};
	union {
		int32_t max;
		int32_t maxs[4];
	};
	uint32_t pos;
	uint32_t left;
	uint32_t period;
};

static inline void spa_bt_ptp_init(struct spa_bt_ptp *p, int32_t period, uint32_t min_duration)
{
	size_t i;

	spa_zero(*p);
	for (i = 0; i < SPA_N_ELEMENTS(p->mins); ++i) {
		p->mins[i] = INT32_MAX;
		p->maxs[i] = INT32_MIN;
	}
	p->left = min_duration;
	p->period = period;
}

static inline void spa_bt_ptp_update(struct spa_bt_ptp *p, int32_t value, uint32_t duration)
{
	const size_t n = SPA_N_ELEMENTS(p->mins);
	size_t i;

	for (i = 0; i < n; ++i) {
		p->mins[i] = SPA_MIN(p->mins[i], value);
		p->maxs[i] = SPA_MAX(p->maxs[i], value);
	}

	p->pos += duration;
	if (p->pos >= p->period / (n - 1)) {
		p->pos = 0;
		for (i = 1; i < SPA_N_ELEMENTS(p->mins); ++i) {
			p->mins[i-1] = p->mins[i];
			p->maxs[i-1] = p->maxs[i];
		}
		p->mins[n-1] = INT32_MAX;
		p->maxs[n-1] = INT32_MIN;
	}

	if (p->left < duration)
		p->left = 0;
	else
		p->left -= duration;
}

static inline bool spa_bt_ptp_valid(struct spa_bt_ptp *p)
{
	return p->left == 0;
}

/**
 * Rate controller.
 *
 * It's here in a form, where it operates on the running average
 * so it's compatible with the level spike determination, and
 * clamping the rate to a range is easy. The impulse response
 * is similar to spa_dll, and step response does not have sign changes.
 *
 * The controller iterates as
 *
 *    avg(j+1) = (1 - beta) avg(j) + beta level(j)
 *    corr(j+1) = corr(j) + a [avg(j+1) - avg(j)] / duration
 *			  + b [avg(j) - target] / duration
 *
 * with beta = duration/avg_period < 0.5 is the moving average parameter,
 * and a = beta/3 + ..., b = beta^2/27 + ....
 *
 * This choice results to c(j) being low-pass filtered, and buffer level(j)
 * converging towards target with stable damped evolution with eigenvalues
 * real and close to each other around (1 - beta)^(1/3).
 *
 * Derivation:
 *
 * The deviation from the buffer level target evolves as
 *
 *     delta(j) = level(j) - target
 *     delta(j+1) = delta(j) + r(j) - c(j)
 *
 * where r is samples received in one duration, and c corrected rate
 * (samples per duration).
 *
 * Note that the rate correction calculated on *previous* cycle is what affects the
 * current one.
 *
 * The rate correction is in general determined by linear filter f
 *
 *     c(j+1) = c(j) + \sum_{k=0}^\infty delta(j - k) f(k)
 *
 * If \sum_k f(k) is not zero, the only fixed point is c=r, delta=0,
 * so this structure (if the filter is stable) rate matches and
 * drives buffer level to target.
 *
 * The z-transform then is
 *
 *     delta(z) = G(z) r(z)
 *     c(z) = F(z) delta(z)
 *     G(z) = (z - 1) / [(z - 1)^2 + f(z)]
 *     F(z) = f(z) / (z - 1)
 *
 * We now want: poles of G(z) must be in |z|<1 for stability, F(z)
 * should damp high frequencies, and f(z) is causal.
 *
 * To satisfy the conditions, take
 *
 *     (z - 1)^2 + f(z) = p(z) / q(z)
 *
 * where p(z) / q(z) are polynomials such that p(z)/q(z) ~ z^2 - 2 z + O(1)
 * in 1/z expansion. This guarantees f(z) is causal, and G(z) = (z-1) q(z) / p(z).
 * We can choose p(z) and q(z) to improve low-pass properties of F(z).
 *
 * Simplest choice is p(z)=(z-1)^2 and q(z)=1, but that does not supress
 * high frequency response in F(z). Better choice is p(z) = (z-u)*(z-v)*(z-w)
 * and q(z) = z - r. Causality requires r = u + v + w - 2.
 * Then,
 *
 *     G(z) = (z - u*v*w)*(z - 1) / [(z - u)*(z - v)*(z - w)]
 *     F(z) = (a z + b - a) / (z - 1) *	 H(z)
 *     H(z) = beta / (z - 1 + beta)
 *     beta = 3 - u - v - w
 *     a = [u*v + u*w + v*w - u - v - w + beta] / beta
 *     b = (1-u)*(1-v)*(1-w) / beta
 *
 * which corresponds to iteration for c(j):
 *
 *    avg(j+1) = (1 - beta) avg(j) + beta delta(j)
 *    c(j+1) = c(j) + a [avg(j+1) - avg(j)] + b avg(j)
 *
 * So the controller operates on the running average,
 * which gives the low-pass property for c(j).
 *
 * The simplest filter is obtained by putting the poles at
 * u=v=w=(1 - beta/3). Then a=beta/3 and b=beta^2/27
 *
 * The same filter is obtained if one uses c(j+1) instead of c(j)
 * in the starting point and takes limit beta -> 0.
 *
 * Overshoot in impulse response could be reduced by moving one of the
 * poles closer to z=1, but this increases the step response time.
 */
struct spa_bt_rate_control
{
	double avg;
	double corr;
};

static inline void spa_bt_rate_control_init(struct spa_bt_rate_control *this, double level)
{
	this->avg = level;
	this->corr = 1.0;
}

static inline double spa_bt_rate_control_update(struct spa_bt_rate_control *this, double level,
		double target, double duration, double period, double rate_diff_max)
{
	/*
	 * x = a / beta
	 * y = b / beta
	 */
	const double beta = SPA_CLAMP(duration / period, 0, 0.5);
	const double x = 1.0/3;
	const double y = beta/27;
	double avg;

	avg = beta * level + (1 - beta) * this->avg;
	this->corr += x * (avg - this->avg) / period
		+ y * (this->avg - target) / period;
	this->avg = avg;

	this->corr = SPA_CLAMP(this->corr, 1 - rate_diff_max, 1 + rate_diff_max);

	return this->corr;
}

#endif
