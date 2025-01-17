/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Copyright (C) 2010 Google Inc. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE.WEBKIT file.
 */

#include <math.h>
#include "biquad.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Q = 1 / sqrt(2), also resulting Q value when S = 1 */
#define BIQUAD_DEFAULT_Q 0.707106781186548

static void set_coefficient(struct biquad *bq, double b0, double b1, double b2,
			    double a0, double a1, double a2)
{
	double a0_inv = 1 / a0;
	bq->b0 = (float)(b0 * a0_inv);
	bq->b1 = (float)(b1 * a0_inv);
	bq->b2 = (float)(b2 * a0_inv);
	bq->a1 = (float)(a1 * a0_inv);
	bq->a2 = (float)(a2 * a0_inv);
}

static void biquad_lowpass(struct biquad *bq, double cutoff, double Q)
{
	/* Limit cutoff to 0 to 1. */
	cutoff = fmax(0.0, fmin(cutoff, 1.0));

	if (cutoff == 1 || cutoff == 0) {
		/* When cutoff is 1, the z-transform is 1.
		 * When cutoff is zero, nothing gets through the filter, so set
		 * coefficients up correctly.
		 */
		set_coefficient(bq, cutoff, 0, 0, 1, 0, 0);
		return;
	}

	/* Set Q to a sane default value if not set */
	if (Q <= 0)
		Q = BIQUAD_DEFAULT_Q;

	/* Compute biquad coefficients for lowpass filter */
	/* H(s) = 1 / (s^2 + s/Q + 1) */
	double w0 = M_PI * cutoff;
	double alpha = sin(w0) / (2 * Q);
	double k = cos(w0);

	double b0 = (1 - k) / 2;
	double b1 = 1 - k;
	double b2 = (1 - k) / 2;
	double a0 = 1 + alpha;
	double a1 = -2 * k;
	double a2 = 1 - alpha;

	set_coefficient(bq, b0, b1, b2, a0, a1, a2);
}

static void biquad_highpass(struct biquad *bq, double cutoff, double Q)
{
	/* Limit cutoff to 0 to 1. */
	cutoff = fmax(0.0, fmin(cutoff, 1.0));

	if (cutoff == 1 || cutoff == 0) {
		/* When cutoff is one, the z-transform is 0. */
		/* When cutoff is zero, we need to be careful because the above
		 * gives a quadratic divided by the same quadratic, with poles
		 * and zeros on the unit circle in the same place. When cutoff
		 * is zero, the z-transform is 1.
		 */
		set_coefficient(bq, 1 - cutoff, 0, 0, 1, 0, 0);
		return;
	}

	/* Set Q to a sane default value if not set */
	if (Q <= 0)
		Q = BIQUAD_DEFAULT_Q;

	/* Compute biquad coefficients for highpass filter */
	/* H(s) = s^2 / (s^2 + s/Q + 1) */
	double w0 = M_PI * cutoff;
	double alpha = sin(w0) / (2 * Q);
	double k = cos(w0);

	double b0 = (1 + k) / 2;
	double b1 = -(1 + k);
	double b2 = (1 + k) / 2;
	double a0 = 1 + alpha;
	double a1 = -2 * k;
	double a2 = 1 - alpha;

	set_coefficient(bq, b0, b1, b2, a0, a1, a2);
}

static void biquad_bandpass(struct biquad *bq, double frequency, double Q)
{
	/* No negative frequencies allowed. */
	frequency = fmax(0.0, frequency);

	/* Don't let Q go negative, which causes an unstable filter. */
	Q = fmax(0.0, Q);

	if (frequency <= 0 || frequency >= 1) {
		/* When the cutoff is zero, the z-transform approaches 0, if Q
		 * > 0. When both Q and cutoff are zero, the z-transform is
		 * pretty much undefined. What should we do in this case?
		 * For now, just make the filter 0. When the cutoff is 1, the
		 * z-transform also approaches 0.
		 */
		set_coefficient(bq, 0, 0, 0, 1, 0, 0);
		return;
	}
	if (Q <= 0) {
		/* When Q = 0, the above formulas have problems. If we
		 * look at the z-transform, we can see that the limit
		 * as Q->0 is 1, so set the filter that way.
		 */
		set_coefficient(bq, 1, 0, 0, 1, 0, 0);
		return;
	}

	double w0 = M_PI * frequency;
	double alpha = sin(w0) / (2 * Q);
	double k = cos(w0);

	double b0 = alpha;
	double b1 = 0;
	double b2 = -alpha;
	double a0 = 1 + alpha;
	double a1 = -2 * k;
	double a2 = 1 - alpha;

	set_coefficient(bq, b0, b1, b2, a0, a1, a2);
}

static void biquad_lowshelf(struct biquad *bq, double frequency, double Q,
			    double db_gain)
{
	/* Clip frequencies to between 0 and 1, inclusive. */
	frequency = fmax(0.0, fmin(frequency, 1.0));

	double A = pow(10.0, db_gain / 40);

	if (frequency == 1) {
		/* The z-transform is a constant gain. */
		set_coefficient(bq, A * A, 0, 0, 1, 0, 0);
		return;
	}
	if (frequency <= 0) {
		/* When frequency is 0, the z-transform is 1. */
		set_coefficient(bq, 1, 0, 0, 1, 0, 0);
		return;
	}

	/* Set Q to an equivalent value to S = 1 if not specified */
	if (Q <= 0)
		Q = BIQUAD_DEFAULT_Q;

	double w0 = M_PI * frequency;
	double alpha = sin(w0) / (2 * Q);
	double k = cos(w0);
	double k2 = 2 * sqrt(A) * alpha;
	double a_plus_one = A + 1;
	double a_minus_one = A - 1;

	double b0 = A * (a_plus_one - a_minus_one * k + k2);
	double b1 = 2 * A * (a_minus_one - a_plus_one * k);
	double b2 = A * (a_plus_one - a_minus_one * k - k2);
	double a0 = a_plus_one + a_minus_one * k + k2;
	double a1 = -2 * (a_minus_one + a_plus_one * k);
	double a2 = a_plus_one + a_minus_one * k - k2;

	set_coefficient(bq, b0, b1, b2, a0, a1, a2);
}

static void biquad_highshelf(struct biquad *bq, double frequency, double Q,
			     double db_gain)
{
	/* Clip frequencies to between 0 and 1, inclusive. */
	frequency = fmax(0.0, fmin(frequency, 1.0));

	double A = pow(10.0, db_gain / 40);

	if (frequency == 1) {
		/* The z-transform is 1. */
		set_coefficient(bq, 1, 0, 0, 1, 0, 0);
		return;
	}
	if (frequency <= 0) {
		/* When frequency = 0, the filter is just a gain, A^2. */
		set_coefficient(bq, A * A, 0, 0, 1, 0, 0);
		return;
	}

	/* Set Q to an equivalent value to S = 1 if not specified */
	if (Q <= 0)
		Q = BIQUAD_DEFAULT_Q;

	double w0 = M_PI * frequency;
	double alpha = sin(w0) / (2 * Q);
	double k = cos(w0);
	double k2 = 2 * sqrt(A) * alpha;
	double a_plus_one = A + 1;
	double a_minus_one = A - 1;

	double b0 = A * (a_plus_one + a_minus_one * k + k2);
	double b1 = -2 * A * (a_minus_one + a_plus_one * k);
	double b2 = A * (a_plus_one + a_minus_one * k - k2);
	double a0 = a_plus_one - a_minus_one * k + k2;
	double a1 = 2 * (a_minus_one - a_plus_one * k);
	double a2 = a_plus_one - a_minus_one * k - k2;

	set_coefficient(bq, b0, b1, b2, a0, a1, a2);
}

static void biquad_peaking(struct biquad *bq, double frequency, double Q,
			   double db_gain)
{
	/* Clip frequencies to between 0 and 1, inclusive. */
	frequency = fmax(0.0, fmin(frequency, 1.0));

	/* Don't let Q go negative, which causes an unstable filter. */
	Q = fmax(0.0, Q);

	double A = pow(10.0, db_gain / 40);

	if (frequency <= 0 || frequency >= 1) {
		/* When frequency is 0 or 1, the z-transform is 1. */
		set_coefficient(bq, 1, 0, 0, 1, 0, 0);
		return;
	}
	if (Q <= 0) {
		/* When Q = 0, the above formulas have problems. If we
		 * look at the z-transform, we can see that the limit
		 * as Q->0 is A^2, so set the filter that way.
		 */
		set_coefficient(bq, A * A, 0, 0, 1, 0, 0);
		return;
	}

	double w0 = M_PI * frequency;
	double alpha = sin(w0) / (2 * Q);
	double k = cos(w0);

	double b0 = 1 + alpha * A;
	double b1 = -2 * k;
	double b2 = 1 - alpha * A;
	double a0 = 1 + alpha / A;
	double a1 = -2 * k;
	double a2 = 1 - alpha / A;

	set_coefficient(bq, b0, b1, b2, a0, a1, a2);
}

static void biquad_notch(struct biquad *bq, double frequency, double Q)
{
	/* Clip frequencies to between 0 and 1, inclusive. */
	frequency = fmax(0.0, fmin(frequency, 1.0));

	/* Don't let Q go negative, which causes an unstable filter. */
	Q = fmax(0.0, Q);

	if (frequency <= 0 || frequency >= 1) {
		/* When frequency is 0 or 1, the z-transform is 1. */
		set_coefficient(bq, 1, 0, 0, 1, 0, 0);
		return;
	}
	if (Q <= 0) {
		/* When Q = 0, the above formulas have problems. If we
		 * look at the z-transform, we can see that the limit
		 * as Q->0 is 0, so set the filter that way.
		 */
		set_coefficient(bq, 0, 0, 0, 1, 0, 0);
		return;
	}

	double w0 = M_PI * frequency;
	double alpha = sin(w0) / (2 * Q);
	double k = cos(w0);

	double b0 = 1;
	double b1 = -2 * k;
	double b2 = 1;
	double a0 = 1 + alpha;
	double a1 = -2 * k;
	double a2 = 1 - alpha;

	set_coefficient(bq, b0, b1, b2, a0, a1, a2);
}

static void biquad_allpass(struct biquad *bq, double frequency, double Q)
{
	/* Clip frequencies to between 0 and 1, inclusive. */
	frequency = fmax(0.0, fmin(frequency, 1.0));

	/* Don't let Q go negative, which causes an unstable filter. */
	Q = fmax(0.0, Q);

	if (frequency <= 0 || frequency >= 1) {
		/* When frequency is 0 or 1, the z-transform is 1. */
		set_coefficient(bq, 1, 0, 0, 1, 0, 0);
		return;
	}

	if (Q <= 0) {
		/* When Q = 0, the above formulas have problems. If we
		 * look at the z-transform, we can see that the limit
		 * as Q->0 is -1, so set the filter that way.
		 */
		set_coefficient(bq, -1, 0, 0, 1, 0, 0);
		return;
	}

	double w0 = M_PI * frequency;
	double alpha = sin(w0) / (2 * Q);
	double k = cos(w0);

	double b0 = 1 - alpha;
	double b1 = -2 * k;
	double b2 = 1 + alpha;
	double a0 = 1 + alpha;
	double a1 = -2 * k;
	double a2 = 1 - alpha;

	set_coefficient(bq, b0, b1, b2, a0, a1, a2);
}

void biquad_set(struct biquad *bq, enum biquad_type type, double freq, double Q,
		double gain)
{
	/* Clear history values. */
	bq->type = type;
	bq->x1 = 0;
	bq->x2 = 0;

	switch (type) {
	case BQ_LOWPASS:
		biquad_lowpass(bq, freq, Q);
		break;
	case BQ_HIGHPASS:
		biquad_highpass(bq, freq, Q);
		break;
	case BQ_BANDPASS:
		biquad_bandpass(bq, freq, Q);
		break;
	case BQ_LOWSHELF:
		biquad_lowshelf(bq, freq, Q, gain);
		break;
	case BQ_HIGHSHELF:
		biquad_highshelf(bq, freq, Q, gain);
		break;
	case BQ_PEAKING:
		biquad_peaking(bq, freq, Q, gain);
		break;
	case BQ_NOTCH:
		biquad_notch(bq, freq, Q);
		break;
	case BQ_ALLPASS:
		biquad_allpass(bq, freq, Q);
		break;
	case BQ_NONE:
	case BQ_RAW:
		/* Default is an identity filter. */
		set_coefficient(bq, 1, 0, 0, 1, 0, 0);
		break;
	}
}
