/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2025 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>

#include <spa/utils/defs.h>
#include <spa/utils/burg-pred.h>
#include <spa/param/audio/raw.h>

#include "history.h"

#define GAPS_MAX_HISTORY	8192u
#define GAPS_MAX_ORDER		128u
#define GAPS_MAX_CURVE		4096u

struct gaps_state {
#define GAPS_MODE_ZERO		0
#define GAPS_MODE_NORMAL	1
#define GAPS_MODE_FADE_IN	2
#define GAPS_MODE_FADE_OUT	3
	uint32_t mode;
	uint32_t count;
	struct spa_history hist;
	struct spa_burg_pred pred;
	float *history;
	float *coeff;
};

struct gaps {
	uint32_t cpu_flags;
	const char *func_name;

	struct spa_log *log;

	uint32_t flags;
	uint32_t channels;
	uint32_t gap;
	uint32_t duration;
	uint32_t history;
	uint32_t order;
	double threshold;
	float curve[GAPS_MAX_CURVE];
	bool empty;

	int (*check) (struct gaps *gaps, const float * SPA_RESTRICT src[], uint32_t n_samples);
	void (*fix) (struct gaps *gaps, float * SPA_RESTRICT dst[],
			const float * SPA_RESTRICT src[], uint32_t n_samples);
	void (*free) (struct gaps *gaps);

	struct gaps_state *states[SPA_AUDIO_MAX_CHANNELS];

	void *data;
};

int gaps_init(struct gaps *gaps);

#define gaps_check(gaps,...)	(gaps)->check(gaps, __VA_ARGS__)
#define gaps_fix(gaps,...)	(gaps)->fix(gaps, __VA_ARGS__)
#define gaps_free(gaps)		(gaps)->free(gaps)

#define DEFINE_CHECK_FUNCTION(arch)					\
int gaps_check_##arch(struct gaps *gaps, const float * SPA_RESTRICT src[],	\
		uint32_t n_samples);

#define DEFINE_FIX_FUNCTION(arch)					\
void gaps_fix_##arch(struct gaps *gaps, float * SPA_RESTRICT dst[],	\
		const float * SPA_RESTRICT src[], uint32_t n_samples);

#define GAPS_OPS_MAX_ALIGN	16

DEFINE_CHECK_FUNCTION(c);
DEFINE_FIX_FUNCTION(c);

#undef DEFINE_CHECK_FUNCTION
#undef DEFINE_FIX_FUNCTION
