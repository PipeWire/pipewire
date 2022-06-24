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
#include <spa/param/audio/raw.h>

#define NOISE_SIZE	(1<<8)
#define NOISE_MOD	(NOISE_SIZE-1)

struct noise {
	uint32_t intensity;
	uint32_t n_channels;
	uint32_t cpu_flags;

	struct spa_log *log;

	void (*process) (struct noise *ns, void * SPA_RESTRICT dst[], uint32_t n_samples);
	void (*free) (struct noise *ns);

	float tab[NOISE_SIZE];
	int tab_idx;
};

int noise_init(struct noise *ns);

#define noise_process(ns,...)		(ns)->process(ns, __VA_ARGS__)
#define noise_free(ns)			(ns)->free(ns)

#define DEFINE_FUNCTION(name,arch)			\
void noise_##name##_##arch(struct noise *ns,		\
		void * SPA_RESTRICT dst[],		\
		uint32_t n_samples);

#define NOISE_OPS_MAX_ALIGN	16

DEFINE_FUNCTION(f32, c);

#undef DEFINE_FUNCTION
