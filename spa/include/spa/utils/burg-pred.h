/* Simple Plugin API */
/* SPDX-FileCopyrightText: Copyright © 2026 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BURG_PRED_H
#define SPA_BURG_PRED_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \defgroup spa_burg_pred Burg Predictor
 * A Burg Predictor implementation
 */

/**
 * \addtogroup spa_burg_pred
 * \{
 */

struct spa_burg_pred;

#include <spa/utils/defs.h>

#ifndef SPA_API_BURG_PRED
 #ifdef SPA_API_IMPL
  #define SPA_API_BURG_PRED SPA_API_IMPL
 #else
  #define SPA_API_BURG_PRED static inline
 #endif
#endif

struct spa_burg_pred {
	float *coef;
	uint32_t n_coef;
	float *state;
	uint32_t pos;
};

SPA_API_BURG_PRED void spa_burg_pred_fit(struct spa_burg_pred *p, float *samples, uint32_t len,
		double threshold, float *state, float *coef, uint32_t max_coef)
{
	double f[SPA_MAX(len, 1u)], b[SPA_MAX(len, 1u)];
	double a[max_coef+1], Dk0;
	double thr = (1.0 - threshold) * (1.0 - threshold);
	uint32_t i, m;
	uint32_t order = len > 0 ? SPA_MIN(len - 1, max_coef) : 0;

	memset(a, 0, sizeof(a));
	a[0] = 1.0;

	double Dk = 0.0;
	for (i = 0; i < len; i++) {
		f[i] = b[i] = samples[i];
		Dk += 2.0 * f[i] * f[i];
	}
	if (len > 0)
		Dk -= f[0] * f[0] + b[len-1] * b[len-1];
	Dk0 = Dk;

	for (m = 0; m < order; m++) {
		if (Dk <= 0.0 || (Dk < thr * Dk0))
		    break;

		double mu = 0.0;
		for (i = 0; i < len-1 - m; i++ )
                        mu += f[i+m+1] * b[i];
		mu *= -2.0 / Dk;

		for (i = 0; i <= (m + 1) / 2; i++ ) {
                        double t1 = a[m+1-i] + mu * a[i];
                        a[i] = a[i] + mu * a[m+1-i];
                        a[m+1-i] = t1;
		}
		for (i = 0; i < len-1 - m; i++ ) {
			double t1 = f[i+m+1] + mu * b[i];
			b[i] = b[i] + mu * f[i+m+1];
                        f[i+m+1] = t1;
		}
		Dk0 = Dk;
		Dk = (1.0 - mu*mu) * Dk - f[m+1] * f[m+1] - b[len-m-2] * b[len-m-2];
	}
	if (m == 0 && max_coef > 0 && len > 0) {
		m = 1;
		coef[0] = 1.0f;
		state[0] = samples[len-1];
	} else {
		for (i = 0; i < m; i++) {
			coef[i] = (float)-a[m-i];
			state[i] = samples[len-m+i];
		}
	}
	p->coef = coef;
	p->state = state;
	p->n_coef = m;
	p->pos = 0;
}

SPA_API_BURG_PRED float spa_burg_pred_next(struct spa_burg_pred *p)
{
	uint32_t i;
	float v;

	if (p->n_coef < 1)
		return 0.0f;

	v = p->state[0] * p->coef[0];

	for (i = 1; i < p->n_coef; i++)
		v += p->coef[i] * (p->state[i-1] = p->state[i]);
	p->state[i-1] = v;

	return v;
}


/**
 * \}
 */

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_BURG_PRED_H */
