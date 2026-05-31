/* AVB support */
/* SPDX-FileCopyrightText: Copyright © 2025 Kebag-Logic */
/* SPDX-License-Identifier: MIT */

/*
 * mc-recover.h — AAF media-clock recovery estimator (listener side).
 *
 * Self-contained and pure (no PipeWire/stream deps) so it can be unit-tested
 * in isolation. A second-order DLL (spa_dll) recovers the talker media rate
 * from the AAF avtp_timestamp progression: each PDU carries a presentation
 * time in the talker's gPTP domain, advancing by frames_per_pdu samples. The
 * model clock advances by the DLL-corrected period; the phase error against the
 * received timestamp drives the DLL. Recovered rate = nominal / corr.
 */

#ifndef AVB_MC_RECOVER_H
#define AVB_MC_RECOVER_H

#include <stdint.h>
#include <stdbool.h>

#include <spa/utils/dll.h>

struct mc_recover {
	bool init;
	struct spa_dll dll;
	double corr;		/* DLL output (period multiplier, ~1.0) */
	double rate;		/* recovered media rate, Hz */
	int32_t last_err_ns;	/* last phase error (model vs avtp_ts), ns */
	uint64_t model_ns;	/* model presentation clock (DLL-tracked) */
	uint32_t last_avtp_ts;	/* previous fed timestamp; model advances by actual PDU count */
	uint64_t pdus;		/* PDUs since prime */
};

static inline void mc_recover_reset(struct mc_recover *m, double nominal_rate)
{
	m->init = false;
	m->corr = 1.0;
	m->rate = nominal_rate;
	m->last_err_ns = 0;
	m->model_ns = 0;
	m->last_avtp_ts = 0;
	m->pdus = 0;
}

/* Feed one PDU's presentation timestamp (low 32 bits of CLOCK_TAI ns). Returns
 * the recovered media rate in Hz. nominal_rate/frames_per_pdu/pdu_period_ns
 * describe the stream's nominal media clock. */
static inline double mc_recover_update(struct mc_recover *m, uint32_t avtp_ts,
		int frames_per_pdu, int nominal_rate, int64_t pdu_period_ns)
{
	int32_t err_ns;
	double err_samples;
	uint64_t step;
	int32_t raw_delta;
	int n_pdus;

	if (!m->init) {
		spa_dll_init(&m->dll);
		spa_dll_set_bw(&m->dll, SPA_DLL_BW_MIN, frames_per_pdu, nominal_rate);
		m->corr = 1.0;
		m->rate = nominal_rate;
		m->last_err_ns = 0;
		m->model_ns = avtp_ts;
		m->last_avtp_ts = avtp_ts;
		m->pdus = 0;
		m->init = true;
		return m->rate;
	}

	/* Advance the model by the ACTUAL number of nominal PDUs elapsed since the
	 * last fed timestamp (avtp_ts delta rounded to pdu_period), then measure the
	 * phase error. Using the real PDU count (not a fixed one-per-call) keeps a
	 * non-1:1 feed — dropped or coalesced PDUs — from accumulating phase error
	 * and saturating the loop into a ±ppm hunt. err>0 = talker ahead of model;
	 * spa_dll returns corr<1, step grows, model catches up (negative feedback);
	 * recovered rate = nominal*corr. A large jump (>8 PDUs: stream gap, reorder,
	 * or the bind-transient seed) re-seeds the phase rather than slewing the
	 * deliberately-slow loop, which otherwise wedges it. */
	raw_delta = (int32_t)(avtp_ts - m->last_avtp_ts);
	m->last_avtp_ts = avtp_ts;
	n_pdus = (int)((double)raw_delta / (double)pdu_period_ns + 0.5);
	if (n_pdus < 1)
		n_pdus = 1;
	if (n_pdus > 8) {
		m->model_ns = avtp_ts;
		m->last_err_ns = 0;
		m->pdus++;
		return m->rate;
	}
	step = (uint64_t)((double)n_pdus * (double)pdu_period_ns / m->corr + 0.5);
	m->model_ns += step;
	err_ns = (int32_t)(avtp_ts - (uint32_t)m->model_ns);
	m->last_err_ns = err_ns;
	err_samples = (double)err_ns * (double)nominal_rate / 1e9;
	/* bound the response to a single corrupt/late timestamp */
	if (err_samples > 128.0)
		err_samples = 128.0;
	else if (err_samples < -128.0)
		err_samples = -128.0;

	m->corr = spa_dll_update(&m->dll, err_samples);
	/* clamp to ±10 % — far beyond any real media clock; guards 1/corr */
	if (m->corr < 0.9)
		m->corr = 0.9;
	else if (m->corr > 1.1)
		m->corr = 1.1;
	m->rate = (double)nominal_rate * m->corr;
	m->pdus++;
	return m->rate;
}

#endif /* AVB_MC_RECOVER_H */
