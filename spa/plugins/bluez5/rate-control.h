/* Spa Bluez5 rate control */
/* SPDX-FileCopyrightText: Copyright © 2022 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_RATE_CONTROL_H
#define SPA_BLUEZ5_RATE_CONTROL_H

#include <spa/utils/defs.h>

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
 *     delta(j+1) = delta(j) + r(j) - c(j+1)
 *
 * where r is samples received in one duration, and c corrected rate
 * (samples per duration).
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
 *     G(z) = (z - 1) / [(z - 1)^2 + z f(z)]
 *     F(z) = f(z) / (z - 1)
 *
 * We now want: poles of G(z) must be in |z|<1 for stability, F(z)
 * should damp high frequencies, and f(z) is causal.
 *
 * To satisfy the conditions, take
 *
 *     (z - 1)^2 + z f(z) = p(z) / q(z)
 *
 * where p(z) is polynomial with leading term z^n with wanted root
 * structure, and q(z) is any polynomial with leading term z^{n-2}.
 * This guarantees f(z) is causal, and G(z) = (z-1) q(z) / p(z).
 * We can choose p(z) and q(z) to improve low-pass properties of F(z).
 *
 * Simplest choice is p(z)=(z-x)^2 and q(z)=1, but that gives flat
 * high frequency response in F(z). Better choice is p(z) = (z-u)*(z-v)*(z-w)
 * and q(z) = z - r. To make F(z) better lowpass, one can cancel
 * a resulting 1/z pole in F(z) by setting r=u*v*w. Then,
 *
 *     G(z) = (z - u*v*w)*(z - 1) / [(z - u)*(z - v)*(z - w)]
 *     F(z) = (a z + b - a) / (z - 1) *	 H(z)
 *     H(z) = beta / (z - 1 + beta)
 *     beta = 1 - u*v*w
 *     a = [(1-u) + (1-v) + (1-w) - beta] / beta
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
 * u=v=w=(1-beta)**(1/3). Since beta << 1, computing the root
 * can be avoided by expanding in series.
 *
 * Overshoot in impulse response could be reduced by moving one of the
 * poles closer to z=1, but this increases the step response time.
 */
struct spa_bt_rate_control
{
	double avg;
	double corr;
};

static void spa_bt_rate_control_init(struct spa_bt_rate_control *this, double level)
{
	this->avg = level;
	this->corr = 1.0;
}

static double spa_bt_rate_control_update(struct spa_bt_rate_control *this, double level,
		double target, double duration, double period, double rate_diff_max)
{
	/*
	 * u = (1 - beta)^(1/3)
	 * x = a / beta
	 * y = b / beta
	 * a = (2 + u) * (1 - u)^2 / beta
	 * b = (1 - u)^3 / beta
	 * beta -> 0
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
