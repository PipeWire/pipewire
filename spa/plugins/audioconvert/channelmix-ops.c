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

#include <spa/support/cpu.h>
#include <spa/utils/defs.h>

#define VOLUME_MIN 0.0f
#define VOLUME_NORM 1.0f

#include "channelmix-ops.h"

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


#define ANY	((uint32_t)-1)
#define EQ	((uint32_t)-2)

static const struct channelmix_info {
	uint32_t src_chan;
	uint64_t src_mask;
	uint32_t dst_chan;
	uint64_t dst_mask;

	channelmix_func_t func;
#define FEATURE_SSE	SPA_CPU_FLAG_SSE
	uint32_t features;
} channelmix_table[] =
{
#if defined (HAVE_SSE)
	{ 2, MASK_MONO, 2, MASK_MONO, channelmix_copy_sse, FEATURE_SSE },
	{ 2, MASK_STEREO, 2, MASK_STEREO, channelmix_copy_sse, FEATURE_SSE },
	{ EQ, 0, EQ, 0, channelmix_copy_sse, FEATURE_SSE },
#endif
	{ 2, MASK_MONO, 2, MASK_MONO, channelmix_copy_c, 0 },
	{ 2, MASK_STEREO, 2, MASK_STEREO, channelmix_copy_c, 0 },
	{ EQ, 0, EQ, 0, channelmix_copy_c, 0 },

	{ 1, MASK_MONO, 2, MASK_STEREO, channelmix_f32_1_2_c, 0 },
	{ 2, MASK_STEREO, 1, MASK_MONO, channelmix_f32_2_1_c, 0 },
	{ 4, MASK_QUAD, 1, MASK_MONO, channelmix_f32_4_1_c, 0 },
	{ 4, MASK_3_1, 1, MASK_MONO, channelmix_f32_3p1_1_c, 0 },
#if defined (HAVE_SSE)
	{ 2, MASK_STEREO, 4, MASK_QUAD, channelmix_f32_2_4_sse, FEATURE_SSE },
#endif
	{ 2, MASK_STEREO, 4, MASK_QUAD, channelmix_f32_2_4_c, 0 },
	{ 2, MASK_STEREO, 4, MASK_3_1, channelmix_f32_2_3p1_c, 0 },
	{ 2, MASK_STEREO, 6, MASK_5_1, channelmix_f32_2_5p1_c, 0 },
#if defined (HAVE_SSE)
	{ 6, MASK_5_1, 2, MASK_STEREO, channelmix_f32_5p1_2_sse, FEATURE_SSE },
#endif
	{ 6, MASK_5_1, 2, MASK_STEREO, channelmix_f32_5p1_2_c, 0 },
#if defined (HAVE_SSE)
	{ 6, MASK_5_1, 4, MASK_QUAD, channelmix_f32_5p1_4_sse, FEATURE_SSE },
#endif
	{ 6, MASK_5_1, 4, MASK_QUAD, channelmix_f32_5p1_4_c, 0 },

#if defined (HAVE_SSE)
	{ 6, MASK_5_1, 4, MASK_3_1, channelmix_f32_5p1_3p1_sse, FEATURE_SSE },
#endif
	{ 6, MASK_5_1, 4, MASK_3_1, channelmix_f32_5p1_3p1_c, 0 },

	{ 8, MASK_7_1, 2, MASK_STEREO, channelmix_f32_7p1_2_c, 0 },
	{ 8, MASK_7_1, 4, MASK_QUAD, channelmix_f32_7p1_4_c, 0 },
	{ 8, MASK_7_1, 4, MASK_3_1, channelmix_f32_7p1_3p1_c, 0 },

	{ ANY, 0, ANY, 0, channelmix_f32_n_m_c, 0 },
};

#define MATCH_CHAN(a,b)		((a) == ANY || (a) == (b))
#define MATCH_FEATURES(a,b)	((a) == 0 || ((a) & (b)) != 0)
#define MATCH_MASK(a,b)		((a) == 0 || ((a) & (b)) == (b))

static const struct channelmix_info *find_channelmix_info(uint32_t src_chan, uint64_t src_mask,
		uint32_t dst_chan, uint64_t dst_mask, uint32_t features)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(channelmix_table); i++) {
		if (!MATCH_FEATURES(channelmix_table[i].features, features))
			continue;

		if (src_chan == dst_chan && src_mask == dst_mask)
			return &channelmix_table[i];

		if (MATCH_CHAN(channelmix_table[i].src_chan, src_chan) &&
		    MATCH_CHAN(channelmix_table[i].dst_chan, dst_chan) &&
		    MATCH_MASK(channelmix_table[i].src_mask, src_mask) &&
		    MATCH_MASK(channelmix_table[i].dst_mask, dst_mask))
			return &channelmix_table[i];
	}
	return NULL;
}
