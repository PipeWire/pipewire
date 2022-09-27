/* Spa
 *
 * Copyright © 2022 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

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
