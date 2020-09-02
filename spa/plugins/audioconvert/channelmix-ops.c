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
#include <math.h>

#include <spa/param/audio/format-utils.h>
#include <spa/support/cpu.h>
#include <spa/support/log.h>
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

#define ANY	((uint32_t)-1)
#define EQ	((uint32_t)-2)

typedef void (*channelmix_func_t) (struct channelmix *mix, uint32_t n_dst, void * SPA_RESTRICT dst[n_dst],
			uint32_t n_src, const void * SPA_RESTRICT src[n_src], uint32_t n_samples);

static const struct channelmix_info {
	uint32_t src_chan;
	uint64_t src_mask;
	uint32_t dst_chan;
	uint64_t dst_mask;

	channelmix_func_t process;
	uint32_t cpu_flags;
} channelmix_table[] =
{
#if defined (HAVE_SSE)
	{ 2, MASK_MONO, 2, MASK_MONO, channelmix_copy_sse, SPA_CPU_FLAG_SSE },
	{ 2, MASK_STEREO, 2, MASK_STEREO, channelmix_copy_sse, SPA_CPU_FLAG_SSE },
	{ EQ, 0, EQ, 0, channelmix_copy_sse, SPA_CPU_FLAG_SSE },
#endif
	{ 2, MASK_MONO, 2, MASK_MONO, channelmix_copy_c, 0 },
	{ 2, MASK_STEREO, 2, MASK_STEREO, channelmix_copy_c, 0 },
	{ EQ, 0, EQ, 0, channelmix_copy_c, 0 },

	{ 1, MASK_MONO, 2, MASK_STEREO, channelmix_f32_1_2_c, 0 },
	{ 2, MASK_STEREO, 1, MASK_MONO, channelmix_f32_2_1_c, 0 },
	{ 4, MASK_QUAD, 1, MASK_MONO, channelmix_f32_4_1_c, 0 },
	{ 4, MASK_3_1, 1, MASK_MONO, channelmix_f32_3p1_1_c, 0 },
#if defined (HAVE_SSE)
	{ 2, MASK_STEREO, 4, MASK_QUAD, channelmix_f32_2_4_sse, SPA_CPU_FLAG_SSE },
#endif
	{ 2, MASK_STEREO, 4, MASK_QUAD, channelmix_f32_2_4_c, 0 },
	{ 2, MASK_STEREO, 4, MASK_3_1, channelmix_f32_2_3p1_c, 0 },
	{ 2, MASK_STEREO, 6, MASK_5_1, channelmix_f32_2_5p1_c, 0 },
#if defined (HAVE_SSE)
	{ 6, MASK_5_1, 2, MASK_STEREO, channelmix_f32_5p1_2_sse, SPA_CPU_FLAG_SSE },
#endif
	{ 6, MASK_5_1, 2, MASK_STEREO, channelmix_f32_5p1_2_c, 0 },
#if defined (HAVE_SSE)
	{ 6, MASK_5_1, 4, MASK_QUAD, channelmix_f32_5p1_4_sse, SPA_CPU_FLAG_SSE },
#endif
	{ 6, MASK_5_1, 4, MASK_QUAD, channelmix_f32_5p1_4_c, 0 },

#if defined (HAVE_SSE)
	{ 6, MASK_5_1, 4, MASK_3_1, channelmix_f32_5p1_3p1_sse, SPA_CPU_FLAG_SSE },
#endif
	{ 6, MASK_5_1, 4, MASK_3_1, channelmix_f32_5p1_3p1_c, 0 },

	{ 8, MASK_7_1, 2, MASK_STEREO, channelmix_f32_7p1_2_c, 0 },
	{ 8, MASK_7_1, 4, MASK_QUAD, channelmix_f32_7p1_4_c, 0 },
	{ 8, MASK_7_1, 4, MASK_3_1, channelmix_f32_7p1_3p1_c, 0 },

	{ ANY, 0, ANY, 0, channelmix_f32_n_m_c, 0 },
};

#define MATCH_CHAN(a,b)		((a) == ANY || (a) == (b))
#define MATCH_CPU_FLAGS(a,b)	((a) == 0 || ((a) & (b)) == a)
#define MATCH_MASK(a,b)		((a) == 0 || ((a) & (b)) == (b))

static const struct channelmix_info *find_channelmix_info(uint32_t src_chan, uint64_t src_mask,
		uint32_t dst_chan, uint64_t dst_mask, uint32_t cpu_flags)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(channelmix_table); i++) {
		if (!MATCH_CPU_FLAGS(channelmix_table[i].cpu_flags, cpu_flags))
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

#define M		0
#define FL		1
#define FR		2
#define FC		3
#define LFE		4
#define SL		5
#define SR		6
#define FLC		7
#define FRC		8
#define RC		9
#define RL		10
#define RR		11
#define TC		12
#define TFL		13
#define TFC		14
#define TFR		15
#define TRL		16
#define TRC		17
#define TRR		18
#define NUM_CHAN	19

#define SQRT3_2      1.224744871f  /* sqrt(3/2) */
#define SQRT1_2      0.707106781f
#define SQRT2	     1.414213562f

#define MATRIX_NORMAL	0
#define MATRIX_DOLBY	1
#define MATRIX_DPLII	2

#define _MASK(ch)	(1ULL << SPA_AUDIO_CHANNEL_ ## ch)
#define STEREO	(_MASK(FL)|_MASK(FR))

static int make_matrix(struct channelmix *mix)
{
	float matrix[NUM_CHAN][NUM_CHAN] = {{ 0.0f }};
	uint64_t src_mask = mix->src_mask;
	uint64_t dst_mask = mix->dst_mask;
	uint64_t unassigned;
	uint32_t i, j, ic, jc, matrix_encoding = MATRIX_NORMAL;
	float clev = SQRT1_2;
	float slev = SQRT1_2;
	float llev = 0.5f;

	spa_log_debug(mix->log, "src-mask:%08"PRIx64" dst-mask:%08"PRIx64,
			src_mask, dst_mask);

	if ((src_mask & _MASK(MONO)) == _MASK(MONO))
		src_mask = _MASK(FC);
	if ((dst_mask & _MASK(MONO)) == _MASK(MONO))
		dst_mask = _MASK(FC);

	for (i = 0; i < NUM_CHAN; i++) {
		if (src_mask & dst_mask & (1ULL << (i + 2)))
			matrix[i][i]= 1.0f;
	}

	unassigned = src_mask & ~dst_mask;

	spa_log_debug(mix->log, "unassigned %08" PRIx64, unassigned);

	if (unassigned & _MASK(FC)){
		if ((dst_mask & STEREO) == STEREO){
			if(src_mask & STEREO) {
				matrix[FL][FC] += clev;
				matrix[FR][FC] += clev;
			} else {
				matrix[FL][FC] += SQRT1_2;
				matrix[FR][FC] += SQRT1_2;
			}
		} else {
			spa_log_warn(mix->log, "can't assign FC");
		}
	}

	if (unassigned & STEREO){
		if (dst_mask & _MASK(FC)) {
			matrix[FC][FL] += SQRT1_2;
			matrix[FC][FR] += SQRT1_2;
			if (src_mask & _MASK(FC))
				matrix[FC][FC] = clev * SQRT2;
		} else {
			spa_log_warn(mix->log, "can't assign STEREO");
		}
	}

	if (unassigned & _MASK(RC)) {
		if (dst_mask & _MASK(RL)){
			matrix[RL][RC] += SQRT1_2;
			matrix[RR][RC] += SQRT1_2;
		} else if (dst_mask & _MASK(SL)) {
			matrix[SL][RC] += SQRT1_2;
			matrix[SR][RC] += SQRT1_2;
		} else if(dst_mask & _MASK(FL)) {
			if (matrix_encoding == MATRIX_DOLBY ||
			    matrix_encoding == MATRIX_DPLII) {
				if (unassigned & (_MASK(RL)|_MASK(RR))) {
					matrix[FL][RC] -= slev * SQRT1_2;
					matrix[FR][RC] += slev * SQRT1_2;
		                } else {
					matrix[FL][RC] -= slev;
					matrix[FR][RC] += slev;
				}
			} else {
				matrix[FL][RC] += slev * SQRT1_2;
				matrix[FR][RC] += slev * SQRT1_2;
			}
		} else if (dst_mask & _MASK(FC)) {
			matrix[FC][RC] += slev * SQRT1_2;
		} else {
			spa_log_warn(mix->log, "can't assign RC");
		}
	}

	if (unassigned & _MASK(RL)) {
		if (dst_mask & _MASK(RC)) {
			matrix[RC][RL] += SQRT1_2;
			matrix[RC][RR] += SQRT1_2;
		} else if (dst_mask & _MASK(SL)) {
			if (src_mask & _MASK(SL)) {
				matrix[SL][RL] += SQRT1_2;
				matrix[SR][RR] += SQRT1_2;
			} else {
				matrix[SL][RL] += 1.0f;
				matrix[SR][RR] += 1.0f;
			}
		} else if (dst_mask & _MASK(FL)) {
			if (matrix_encoding == MATRIX_DOLBY) {
				matrix[FL][RL] -= slev * SQRT1_2;
				matrix[FL][RR] -= slev * SQRT1_2;
				matrix[FR][RL] += slev * SQRT1_2;
				matrix[FR][RR] += slev * SQRT1_2;
			} else if (matrix_encoding == MATRIX_DPLII) {
				matrix[FL][RL] -= slev * SQRT3_2;
				matrix[FL][RR] -= slev * SQRT1_2;
				matrix[FR][RL] += slev * SQRT1_2;
				matrix[FR][RR] += slev * SQRT3_2;
			} else {
				matrix[FL][RL] += slev;
				matrix[FR][RR] += slev;
			}
		} else if (dst_mask & _MASK(FC)) {
			matrix[FC][RL]+= slev * SQRT1_2;
			matrix[FC][RR]+= slev * SQRT1_2;
		} else {
			spa_log_warn(mix->log, "can't assign RL");
		}
	}

	if (unassigned & _MASK(SL)) {
		if (dst_mask & _MASK(RL)) {
			if (src_mask & _MASK(RL)) {
				matrix[RL][SL] += SQRT1_2;
				matrix[RR][SR] += SQRT1_2;
			} else {
				matrix[RL][SL] += 1.0f;
				matrix[RR][SR] += 1.0f;
			}
		} else if (dst_mask & _MASK(RC)) {
			matrix[RC][SL]+= SQRT1_2;
			matrix[RC][SR]+= SQRT1_2;
		} else if (dst_mask & _MASK(FL)) {
			if (matrix_encoding == MATRIX_DOLBY) {
				matrix[FL][SL] -= slev * SQRT1_2;
				matrix[FL][SR] -= slev * SQRT1_2;
				matrix[FR][SL] += slev * SQRT1_2;
				matrix[FR][SR] += slev * SQRT1_2;
			} else if (matrix_encoding == MATRIX_DPLII) {
				matrix[FL][SL] -= slev * SQRT3_2;
				matrix[FL][SR] -= slev * SQRT1_2;
				matrix[FR][SL] += slev * SQRT1_2;
				matrix[FR][SR] += slev * SQRT3_2;
			} else {
				matrix[FL][SL] += slev;
				matrix[FR][SR] += slev;
			}
		} else if (dst_mask & _MASK(FC)) {
			matrix[FC][SL] += slev * SQRT1_2;
			matrix[FC][SR] += slev * SQRT1_2;
		} else {
			spa_log_warn(mix->log, "can't assign SL");
		}
	}

	if (unassigned & _MASK(FLC)) {
		if (dst_mask & _MASK(FL)) {
			matrix[FC][FLC]+= 1.0f;
			matrix[FC][FRC]+= 1.0f;
		} else if(dst_mask & _MASK(FC)) {
			matrix[FC][FLC]+= SQRT1_2;
			matrix[FC][FRC]+= SQRT1_2;
		} else {
			spa_log_warn(mix->log, "can't assign FLC");
		}
	}
	if (unassigned & _MASK(LFE)) {
		if (dst_mask & _MASK(FC)) {
			matrix[FC][LFE] += llev;
		} else if (dst_mask & _MASK(FL)) {
			matrix[FL][LFE] += llev * SQRT1_2;
			matrix[FR][LFE] += llev * SQRT1_2;
		} else {
			spa_log_warn(mix->log, "can't assign LFE");
		}
	}
	for (ic = 0, i = 0; i < NUM_CHAN; i++) {
		float sum = 0.0f;
		if ((dst_mask & (1UL << (i + 2))) == 0)
			continue;
		for (jc = 0, j = 0; j < NUM_CHAN; j++) {
			if ((src_mask & (1UL << (j + 2))) == 0)
				continue;
			mix->matrix_orig[ic][jc++] = matrix[i][j];
			sum += fabs(matrix[i][j]);
		}
		if (sum > 1.0f)
			for (j = 0; j < jc; j++)
		                mix->matrix_orig[ic][j] /= sum;
		ic++;
	}
	return 0;
}

static void impl_channelmix_set_volume(struct channelmix *mix, float volume, bool mute,
		uint32_t n_channel_volumes, float *channel_volumes)
{
	float volumes[SPA_AUDIO_MAX_CHANNELS];
	float vol = mute ? 0.0f : volume, t;
	uint32_t i, j;
	uint32_t src_chan = mix->src_chan;
	uint32_t dst_chan = mix->dst_chan;

	/** apply global volume to channels */
	mix->norm = true;
	for (i = 0; i < n_channel_volumes; i++) {
		volumes[i] = channel_volumes[i] * vol;
		if (volumes[i] != 1.0f)
			mix->norm = false;
	}

	if (n_channel_volumes == src_chan) {
		for (i = 0; i < dst_chan; i++) {
			for (j = 0; j < src_chan; j++) {
				mix->matrix[i][j] = mix->matrix_orig[i][j] * volumes[j];
			}
		}
	} else if (n_channel_volumes == dst_chan) {
		for (i = 0; i < dst_chan; i++) {
			for (j = 0; j < src_chan; j++) {
				mix->matrix[i][j] = mix->matrix_orig[i][j] * volumes[i];
			}
		}
	}

	mix->zero = true;
	mix->equal = true;
	mix->identity = dst_chan == src_chan;
	t = 0.0;

	for (i = 0; i < dst_chan; i++) {
		for (j = 0; j < src_chan; j++) {
			float v = mix->matrix[i][j];
			spa_log_debug(mix->log, "%d %d: %f", i, j, v);
			if (i == 0 && j == 0)
				t = v;
			else if (t != v)
				mix->equal = false;
			if (v != 0.0)
				mix->zero = false;
			if ((i == j && v != 1.0f) ||
			    (i != j && v != 0.0f))
				mix->identity = false;
		}
	}
	spa_log_debug(mix->log, "zero:%d norm:%d identity:%d", mix->zero, mix->norm, mix->identity);
}

static void impl_channelmix_free(struct channelmix *mix)
{
	mix->process = NULL;
}

int channelmix_init(struct channelmix *mix)
{
	const struct channelmix_info *info;

	info = find_channelmix_info(mix->src_chan, mix->src_mask, mix->dst_chan, mix->dst_mask,
			mix->cpu_flags);
	if (info == NULL)
		return -ENOTSUP;

	mix->free = impl_channelmix_free;
	mix->process = info->process;
	mix->set_volume = impl_channelmix_set_volume;
	mix->cpu_flags = info->cpu_flags;
	return make_matrix(mix);
}
