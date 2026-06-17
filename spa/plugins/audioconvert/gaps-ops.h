/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2025 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>

#include <spa/utils/defs.h>
#include <spa/param/audio/raw.h>

struct gaps_state {
	uint32_t mode;
	uint32_t count;
	float history[1];
};

#define GAPS_MAX_CURVE	4096u

struct gaps {
	uint32_t cpu_flags;
	const char *func_name;

	struct spa_log *log;

	uint32_t flags;
	uint32_t channels;
	uint32_t gap;
	uint32_t duration;
	float curve[GAPS_MAX_CURVE];
	bool empty;

	int (*check) (struct gaps *gaps, const float * SPA_RESTRICT src[], uint32_t n_samples);
	void (*fix) (struct gaps *gaps, float * SPA_RESTRICT dst[],
			const float * SPA_RESTRICT src[], uint32_t n_samples);
	void (*free) (struct gaps *gaps);

	struct gaps_state states[SPA_AUDIO_MAX_CHANNELS];
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
