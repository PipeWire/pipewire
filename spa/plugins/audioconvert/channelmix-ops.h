/* Spa
 *
 * Copyright © 2018 Wim Taymans
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

#define VOLUME_MIN 0.0f
#define VOLUME_NORM 1.0f

#define _M(ch)		(1UL << SPA_AUDIO_CHANNEL_ ## ch)
#define MASK_MONO	_M(FC)|_M(MONO)|_M(UNKNOWN)
#define MASK_STEREO	_M(FL)|_M(FR)|_M(UNKNOWN)
#define MASK_QUAD	_M(FL)|_M(FR)|_M(RL)|_M(RR)|_M(UNKNOWN)
#define MASK_3_1	_M(FL)|_M(FR)|_M(FC)|_M(LFE)
#define MASK_5_1	_M(FL)|_M(FR)|_M(FC)|_M(LFE)|_M(SL)|_M(SR)|_M(RL)|_M(RR)
#define MASK_7_1	_M(FL)|_M(FR)|_M(FC)|_M(LFE)|_M(SL)|_M(SR)|_M(RL)|_M(RR)

typedef void (*channelmix_func_t) (void *data, int n_dst, void * SPA_RESTRICT dst[n_dst],
				   int n_src, const void * SPA_RESTRICT src[n_src],
				   const void * SPA_RESTRICT matrix, float v, int n_samples);

#define DEFINE_FUNCTION(name,arch)					\
void channelmix_##name##_##arch(void *data,				\
		int n_dst, void * SPA_RESTRICT dst[n_dst],		\
		int n_src, const void * SPA_RESTRICT src[n_src],	\
		const void *matrix, float v, int n_samples);

DEFINE_FUNCTION(copy, c);
DEFINE_FUNCTION(f32_n_m, c);
DEFINE_FUNCTION(f32_1_2, c);
DEFINE_FUNCTION(f32_2_1, c);
DEFINE_FUNCTION(f32_4_1, c);
DEFINE_FUNCTION(f32_3p1_1, c);
DEFINE_FUNCTION(f32_2_4, c);
DEFINE_FUNCTION(f32_2_3p1, c);
DEFINE_FUNCTION(f32_2_5p1, c);
DEFINE_FUNCTION(f32_5p1_2, c);
DEFINE_FUNCTION(f32_5p1_3p1, c);
DEFINE_FUNCTION(f32_5p1_4, c);
DEFINE_FUNCTION(f32_7p1_2, c);
DEFINE_FUNCTION(f32_7p1_3p1, c);
DEFINE_FUNCTION(f32_7p1_4, c);

#if defined (HAVE_SSE)
DEFINE_FUNCTION(copy, sse);
DEFINE_FUNCTION(f32_2_4, sse);
DEFINE_FUNCTION(f32_5p1_2, sse);
DEFINE_FUNCTION(f32_5p1_3p1, sse);
DEFINE_FUNCTION(f32_5p1_4, sse);
DEFINE_FUNCTION(f32_7p1_4, sse);
#endif
