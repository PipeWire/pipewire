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
#include <spa/utils/string.h>
#include <spa/param/audio/raw.h>

#define DITHER_OPS_MAX_ALIGN	16
#define DITHER_OPS_MAX_OVERREAD	16

struct dither {
	uint32_t quantize;
#define DITHER_METHOD_NONE 		0
#define DITHER_METHOD_RECTANGULAR 	2
#define DITHER_METHOD_TRIANGULAR 	3
#define DITHER_METHOD_SHAPED_5	 	4
	uint32_t method;
	uint32_t n_channels;
	uint32_t cpu_flags;

	struct spa_log *log;

	void (*process) (struct dither *d, void * SPA_RESTRICT dst[],
			const void * SPA_RESTRICT src[], uint32_t n_samples);
	void (*free) (struct dither *d);

	uint32_t random[16 + DITHER_OPS_MAX_ALIGN/sizeof(uint32_t)];
	float *dither;
	uint32_t dither_size;
	float scale;
};

int dither_init(struct dither *d);

static const struct dither_method_info {
	const char *label;
	const char *description;
	uint32_t method;
} dither_method_info[] = {
	[DITHER_METHOD_NONE] = { "none", "Disabled", DITHER_METHOD_NONE },
	[DITHER_METHOD_RECTANGULAR] = { "rectangular", "Rectangular dithering", DITHER_METHOD_RECTANGULAR },
	[DITHER_METHOD_TRIANGULAR] = { "triangular", "Triangular dithering", DITHER_METHOD_TRIANGULAR },
	[DITHER_METHOD_SHAPED_5] = { "shaped5", "Shaped 5 dithering", DITHER_METHOD_SHAPED_5 }
};

static inline uint32_t dither_method_from_label(const char *label)
{
	uint32_t i;
	for (i = 0; i < SPA_N_ELEMENTS(dither_method_info); i++) {
		if (spa_streq(dither_method_info[i].label, label))
			return dither_method_info[i].method;
	}
	return DITHER_METHOD_NONE;
}

#define dither_process(d,...)		(d)->process(d, __VA_ARGS__)
#define dither_free(d)			(d)->free(d)

#define DEFINE_FUNCTION(name,arch)			\
void dither_##name##_##arch(struct dither *d,		\
		void * SPA_RESTRICT dst[],		\
		const void * SPA_RESTRICT src[],	\
		uint32_t n_samples);

DEFINE_FUNCTION(f32, c);
#if defined(HAVE_SSE2)
DEFINE_FUNCTION(f32, sse2);
#endif

#undef DEFINE_FUNCTION
