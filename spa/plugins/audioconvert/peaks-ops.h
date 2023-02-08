/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>

#include <spa/utils/defs.h>

struct peaks {
	uint32_t cpu_flags;
	const char *func_name;

	struct spa_log *log;

	uint32_t flags;

	void (*min_max) (struct peaks *peaks, const float * SPA_RESTRICT src,
		uint32_t n_samples, float *min, float *max);
	float (*abs_max) (struct peaks *peaks, const float * SPA_RESTRICT src,
			uint32_t n_samples, float max);

	void (*free) (struct peaks *peaks);
};

int peaks_init(struct peaks *peaks);

#define peaks_min_max(peaks,...)	(peaks)->min_max(peaks, __VA_ARGS__)
#define peaks_abs_max(peaks,...)	(peaks)->abs_max(peaks, __VA_ARGS__)
#define peaks_free(peaks)		(peaks)->free(peaks)

#define DEFINE_MIN_MAX_FUNCTION(arch)				\
void peaks_min_max_##arch(struct peaks *peaks,			\
		const float * SPA_RESTRICT src,			\
		uint32_t n_samples, float *min, float *max);

#define DEFINE_ABS_MAX_FUNCTION(arch)				\
float peaks_abs_max_##arch(struct peaks *peaks,			\
		const float * SPA_RESTRICT src,			\
		uint32_t n_samples, float max);

#define PEAKS_OPS_MAX_ALIGN	16

DEFINE_MIN_MAX_FUNCTION(c);
DEFINE_ABS_MAX_FUNCTION(c);

#if defined (HAVE_SSE)
DEFINE_MIN_MAX_FUNCTION(sse);
DEFINE_ABS_MAX_FUNCTION(sse);
#endif

#undef DEFINE_FUNCTION
