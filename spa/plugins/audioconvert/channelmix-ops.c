/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <spa/param/audio/format-utils.h>
#include <spa/support/cpu.h>
#include <spa/support/log.h>
#include <spa/utils/defs.h>
#include <spa/debug/types.h>

#include "channelmix-ops.h"
#include "hilbert.h"

#define ANY	((uint32_t)-1)
#define EQ	((uint32_t)-2)

typedef void (*channelmix_func_t) (struct channelmix *mix, void * SPA_RESTRICT dst[],
			const void * SPA_RESTRICT src[], uint32_t n_samples);

#define MAKE(sc,sm,dc,dm,func,...) \
	{ sc, sm, dc, dm, func, #func, __VA_ARGS__ }

static const struct channelmix_info {
	uint32_t src_chan;
	uint64_t src_mask;
	uint32_t dst_chan;
	uint64_t dst_mask;

	channelmix_func_t process;
	const char *name;

	uint32_t cpu_flags;
} channelmix_table[] =
{
#if defined (HAVE_SSE)
	MAKE(2, MASK_MONO, 2, MASK_MONO, channelmix_copy_sse, SPA_CPU_FLAG_SSE),
	MAKE(2, MASK_STEREO, 2, MASK_STEREO, channelmix_copy_sse, SPA_CPU_FLAG_SSE),
	MAKE(EQ, 0, EQ, 0, channelmix_copy_sse, SPA_CPU_FLAG_SSE),
#endif
	MAKE(2, MASK_MONO, 2, MASK_MONO, channelmix_copy_c),
	MAKE(2, MASK_STEREO, 2, MASK_STEREO, channelmix_copy_c),
	MAKE(EQ, 0, EQ, 0, channelmix_copy_c),

	MAKE(1, MASK_MONO, 2, MASK_STEREO, channelmix_f32_1_2_c),
	MAKE(2, MASK_STEREO, 1, MASK_MONO, channelmix_f32_2_1_c),
	MAKE(4, MASK_QUAD, 1, MASK_MONO, channelmix_f32_4_1_c),
	MAKE(4, MASK_3_1, 1, MASK_MONO, channelmix_f32_4_1_c),
	MAKE(2, MASK_STEREO, 4, MASK_QUAD, channelmix_f32_2_4_c),
#if defined (HAVE_SSE)
	MAKE(2, MASK_STEREO, 4, MASK_3_1, channelmix_f32_2_3p1_sse, SPA_CPU_FLAG_SSE),
#endif
	MAKE(2, MASK_STEREO, 4, MASK_3_1, channelmix_f32_2_3p1_c),
#if defined (HAVE_SSE)
	MAKE(2, MASK_STEREO, 6, MASK_5_1, channelmix_f32_2_5p1_sse, SPA_CPU_FLAG_SSE),
#endif
	MAKE(2, MASK_STEREO, 6, MASK_5_1, channelmix_f32_2_5p1_c),
#if defined (HAVE_SSE)
	MAKE(2, MASK_STEREO, 8, MASK_7_1, channelmix_f32_2_7p1_sse, SPA_CPU_FLAG_SSE),
#endif
	MAKE(2, MASK_STEREO, 8, MASK_7_1, channelmix_f32_2_7p1_c),
#if defined (HAVE_SSE)
	MAKE(4, MASK_3_1, 2, MASK_STEREO, channelmix_f32_3p1_2_sse, SPA_CPU_FLAG_SSE),
#endif
	MAKE(4, MASK_3_1, 2, MASK_STEREO, channelmix_f32_3p1_2_c),
#if defined (HAVE_SSE)
	MAKE(6, MASK_5_1, 2, MASK_STEREO, channelmix_f32_5p1_2_sse, SPA_CPU_FLAG_SSE),
#endif
	MAKE(6, MASK_5_1, 2, MASK_STEREO, channelmix_f32_5p1_2_c),
#if defined (HAVE_SSE)
	MAKE(6, MASK_5_1, 4, MASK_QUAD, channelmix_f32_5p1_4_sse, SPA_CPU_FLAG_SSE),
#endif
	MAKE(6, MASK_5_1, 4, MASK_QUAD, channelmix_f32_5p1_4_c),

#if defined (HAVE_SSE)
	MAKE(6, MASK_5_1, 4, MASK_3_1, channelmix_f32_5p1_3p1_sse, SPA_CPU_FLAG_SSE),
#endif
	MAKE(6, MASK_5_1, 4, MASK_3_1, channelmix_f32_5p1_3p1_c),

	MAKE(8, MASK_7_1, 2, MASK_STEREO, channelmix_f32_7p1_2_c),
	MAKE(8, MASK_7_1, 4, MASK_QUAD, channelmix_f32_7p1_4_c),
	MAKE(8, MASK_7_1, 4, MASK_3_1, channelmix_f32_7p1_3p1_c),

#if defined (HAVE_SSE)
	MAKE(ANY, 0, ANY, 0, channelmix_f32_n_m_sse, SPA_CPU_FLAG_SSE),
#endif
	MAKE(ANY, 0, ANY, 0, channelmix_f32_n_m_c),
};
#undef MAKE

#define MATCH_CHAN(a,b)		((a) == ANY || (a) == (b))
#define MATCH_CPU_FLAGS(a,b)	((a) == 0 || ((a) & (b)) == a)
#define MATCH_MASK(a,b)		((a) == 0 || ((a) & (b)) == (b))

static const struct channelmix_info *find_channelmix_info(uint32_t src_chan, uint64_t src_mask,
		uint32_t dst_chan, uint64_t dst_mask, uint32_t cpu_flags)
{
	SPA_FOR_EACH_ELEMENT_VAR(channelmix_table, info) {
		if (!MATCH_CPU_FLAGS(info->cpu_flags, cpu_flags))
			continue;

		if (src_chan == dst_chan && src_mask == dst_mask)
			return info;

		if (MATCH_CHAN(info->src_chan, src_chan) &&
		    MATCH_CHAN(info->dst_chan, dst_chan) &&
		    MATCH_MASK(info->src_mask, src_mask) &&
		    MATCH_MASK(info->dst_mask, dst_mask))
			return info;
	}
	return NULL;
}

#define SQRT3_2      1.224744871f  /* sqrt(3/2) */
#define SQRT1_2      0.707106781f
#define SQRT2	     1.414213562f

#define MATRIX_NORMAL	0
#define MATRIX_DOLBY	1
#define MATRIX_DPLII	2

#define _SH		2
#define _CH(ch)		((SPA_AUDIO_CHANNEL_ ## ch)-_SH)
#define _MASK(ch)	(1ULL << _CH(ch))
#define FRONT		(_MASK(FC))
#define STEREO		(_MASK(FL)|_MASK(FR))
#define REAR		(_MASK(RL)|_MASK(RR))
#define SIDE		(_MASK(SL)|_MASK(SR))

static uint32_t mask_to_ch(struct channelmix *mix, uint64_t mask)
{
	uint32_t ch = 0;
	while (mask > 1) {
		ch++;
		mask >>= 1;
	}
	return ch;
}

static void distribute_mix(struct channelmix *mix,
		float matrix[SPA_AUDIO_MAX_CHANNELS][SPA_AUDIO_MAX_CHANNELS],
		uint64_t mask)
{
	uint32_t i, ch = mask_to_ch(mix, mask);
	for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++)
		matrix[i][ch]= 1.0f;
}
static void average_mix(struct channelmix *mix,
		float matrix[SPA_AUDIO_MAX_CHANNELS][SPA_AUDIO_MAX_CHANNELS],
		uint64_t mask)
{
	uint32_t i, ch = mask_to_ch(mix, mask);
	for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++)
		matrix[ch][i]= 1.0f;
}
static void pair_mix(float matrix[SPA_AUDIO_MAX_CHANNELS][SPA_AUDIO_MAX_CHANNELS])
{
	uint32_t i;
	for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++)
		matrix[i][i]= 1.0f;
}
static bool match_mix(struct channelmix *mix,
		float matrix[SPA_AUDIO_MAX_CHANNELS][SPA_AUDIO_MAX_CHANNELS],
		uint64_t src_mask, uint64_t dst_mask)
{
	bool matched = false;
	uint32_t i;
	for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++) {
		if ((src_mask & dst_mask & (1ULL << i))) {
			spa_log_info(mix->log, "matched channel %u (%f)", i, 1.0f);
			matrix[i][i] = 1.0f;
			matched = true;
		}
	}
	return matched;
}

static int make_matrix(struct channelmix *mix)
{
	float matrix[SPA_AUDIO_MAX_CHANNELS][SPA_AUDIO_MAX_CHANNELS] = {{ 0.0f }};
	uint64_t src_mask = mix->src_mask, src_paired;
	uint64_t dst_mask = mix->dst_mask, dst_paired;
	uint32_t src_chan = mix->src_chan;
	uint32_t dst_chan = mix->dst_chan;
	uint64_t unassigned, keep;
	uint32_t i, j, ic, jc, matrix_encoding = MATRIX_NORMAL;
	float clev = SQRT1_2;
	float slev = SQRT1_2;
	float llev = 0.5f;
	float maxsum = 0.0f;
	bool filter_fc = false, filter_lfe = false, matched = false, normalize;
#define _MATRIX(s,d)	matrix[_CH(s)][_CH(d)]

	normalize =  SPA_FLAG_IS_SET(mix->options, CHANNELMIX_OPTION_NORMALIZE);

	spa_log_debug(mix->log, "src-mask:%08"PRIx64" dst-mask:%08"PRIx64
			" options:%08x", src_mask, dst_mask, mix->options);

	/* shift so that bit 0 is MONO */
	src_mask >>= _SH;
	dst_mask >>= _SH;

	if (src_chan > 1 && (src_mask & _MASK(MONO)))
		src_mask = 0;
	if (dst_chan > 1 && (dst_mask & _MASK(MONO)))
		dst_mask = 0;

	src_paired = src_mask;
	dst_paired = dst_mask;

	/* unknown channels */
	if (src_mask == 0 || dst_mask == 0) {
		if (src_chan == 1) {
			/* one src channel goes everywhere */
			spa_log_info(mix->log, "distribute UNK (%f) %"PRIu64, 1.0f, src_mask);
			distribute_mix(mix, matrix, src_mask);
		} else if (dst_chan == 1) {
			/* one dst channel get average of everything */
			spa_log_info(mix->log, "average UNK (%f) %"PRIu64, 1.0f / src_chan, dst_mask);
			average_mix(mix, matrix, dst_mask);
			normalize = true;
		} else {
			/* just pair channels */
			spa_log_info(mix->log, "pairing UNK channels (%f)", 1.0f);
			if (src_mask == 0)
				src_paired = dst_mask;
			else if (dst_mask == 0)
				dst_paired = src_mask;
			pair_mix(matrix);
		}
		goto done;
	} else {
		spa_log_debug(mix->log, "matching channels");
		matched = match_mix(mix, matrix, src_mask, dst_mask);
	}

	unassigned = src_mask & ~dst_mask;
	keep = dst_mask & ~src_mask;

	if (!SPA_FLAG_IS_SET(mix->options, CHANNELMIX_OPTION_UPMIX)) {
		/* upmix completely disabled */
		keep = 0;
	} else {
		/* some upmixing (FC and LFE) enabled. */
		if (mix->upmix == CHANNELMIX_UPMIX_NONE)
			keep = 0;
		if (mix->fc_cutoff > 0.0f)
			keep |= FRONT;
		else
			keep &= ~FRONT;
		if (mix->lfe_cutoff > 0.0f)
			keep |= _MASK(LFE);
		else
			keep &= ~_MASK(LFE);
	}
	/* if we have no channel matched, try to upmix or keep the stereo
	 * pair or else we might end up with silence. */
	if (dst_mask & STEREO && !matched)
		keep |= STEREO;

	spa_log_info(mix->log, "unassigned downmix %08" PRIx64 " %08" PRIx64, unassigned, keep);

	if (unassigned & _MASK(MONO)) {
		if ((dst_mask & STEREO) == STEREO) {
			spa_log_info(mix->log, "assign MONO to STEREO (%f)", 1.0f);
			_MATRIX(FL,MONO) += 1.0f;
			_MATRIX(FR,MONO) += 1.0f;
			keep &= ~STEREO;
		} else if ((dst_mask & FRONT) == FRONT) {
			spa_log_info(mix->log, "assign MONO to FRONT (%f)", 1.0f);
			_MATRIX(FC,MONO) += 1.0f;
			normalize = true;
		} else {
			spa_log_warn(mix->log, "can't assign MONO");
		}
	}

	if (unassigned & FRONT) {
		if ((dst_mask & STEREO) == STEREO){
			if (src_mask & STEREO) {
				spa_log_info(mix->log, "assign FC to STEREO (%f)", clev);
				_MATRIX(FL,FC) += clev;
				_MATRIX(FR,FC) += clev;
			} else {
				spa_log_info(mix->log, "assign FC to STEREO (%f)", SQRT1_2);
				_MATRIX(FL,FC) += SQRT1_2;
				_MATRIX(FR,FC) += SQRT1_2;
			}
			keep &= ~STEREO;
		} else if (dst_mask & _MASK(MONO)){
			spa_log_info(mix->log, "assign FC to MONO (%f)", 1.0f);
			for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++)
				matrix[i][_CH(FC)]= 1.0f;
			normalize = true;
		} else {
			spa_log_warn(mix->log, "can't assign FC");
		}
	}

	if (unassigned & STEREO){
		if (dst_mask & FRONT) {
			spa_log_info(mix->log, "assign STEREO to FC (%f)", SQRT1_2);
			_MATRIX(FC,FL) += SQRT1_2;
			_MATRIX(FC,FR) += SQRT1_2;
			if (src_mask & FRONT) {
				spa_log_info(mix->log, "assign FC to FC (%f)", clev * SQRT2);
				_MATRIX(FC,FC) = clev * SQRT2;
			}
			keep &= ~FRONT;
		} else if ((dst_mask & _MASK(MONO))){
			spa_log_info(mix->log, "assign STEREO to MONO (%f)", 1.0f);
			for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++) {
				matrix[i][_CH(FL)]= 1.0f;
				matrix[i][_CH(FR)]= 1.0f;
			}
			normalize = true;
		} else {
			spa_log_warn(mix->log, "can't assign STEREO");
		}
	}

	if (unassigned & _MASK(RC)) {
		if (dst_mask & REAR){
			spa_log_info(mix->log, "assign RC to RL+RR (%f)", SQRT1_2);
			_MATRIX(RL,RC) += SQRT1_2;
			_MATRIX(RR,RC) += SQRT1_2;
		} else if (dst_mask & SIDE) {
			spa_log_info(mix->log, "assign RC to SL+SR (%f)", SQRT1_2);
			_MATRIX(SL,RC) += SQRT1_2;
			_MATRIX(SR,RC) += SQRT1_2;
		} else if(dst_mask & STEREO) {
			spa_log_info(mix->log, "assign RC to FL+FR");
			if (matrix_encoding == MATRIX_DOLBY ||
			    matrix_encoding == MATRIX_DPLII) {
				if (unassigned & (_MASK(RL)|_MASK(RR))) {
					_MATRIX(FL,RC) -= slev * SQRT1_2;
					_MATRIX(FR,RC) += slev * SQRT1_2;
		                } else {
					_MATRIX(FL,RC) -= slev;
					_MATRIX(FR,RC) += slev;
				}
			} else {
				_MATRIX(FL,RC) += slev * SQRT1_2;
				_MATRIX(FR,RC) += slev * SQRT1_2;
			}
		} else if (dst_mask & FRONT) {
			spa_log_info(mix->log, "assign RC to FC (%f)", slev * SQRT1_2);
			_MATRIX(FC,RC) += slev * SQRT1_2;
		} else if (dst_mask & _MASK(MONO)){
			spa_log_info(mix->log, "assign RC to MONO (%f)", 1.0f);
			for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++)
				matrix[i][_CH(RC)]= 1.0f;
			normalize = true;
		} else {
			spa_log_warn(mix->log, "can't assign RC");
		}
	}

	if (unassigned & REAR) {
		if (dst_mask & _MASK(RC)) {
			spa_log_info(mix->log, "assign RL+RR to RC");
			_MATRIX(RC,RL) += SQRT1_2;
			_MATRIX(RC,RR) += SQRT1_2;
		} else if (dst_mask & SIDE) {
			spa_log_info(mix->log, "assign RL+RR to SL+SR");
			if (src_mask & SIDE) {
				_MATRIX(SL,RL) += SQRT1_2;
				_MATRIX(SR,RR) += SQRT1_2;
			} else {
				_MATRIX(SL,RL) += 1.0f;
				_MATRIX(SR,RR) += 1.0f;
			}
			keep &= ~SIDE;
		} else if (dst_mask & STEREO) {
			spa_log_info(mix->log, "assign RL+RR to FL+FR (%f)", slev);
			if (matrix_encoding == MATRIX_DOLBY) {
				_MATRIX(FL,RL) -= slev * SQRT1_2;
				_MATRIX(FL,RR) -= slev * SQRT1_2;
				_MATRIX(FR,RL) += slev * SQRT1_2;
				_MATRIX(FR,RR) += slev * SQRT1_2;
			} else if (matrix_encoding == MATRIX_DPLII) {
				_MATRIX(FL,RL) -= slev * SQRT3_2;
				_MATRIX(FL,RR) -= slev * SQRT1_2;
				_MATRIX(FR,RL) += slev * SQRT1_2;
				_MATRIX(FR,RR) += slev * SQRT3_2;
			} else {
				_MATRIX(FL,RL) += slev;
				_MATRIX(FR,RR) += slev;
			}
		} else if (dst_mask & FRONT) {
			spa_log_info(mix->log, "assign RL+RR to FC (%f)",
					slev * SQRT1_2);
			_MATRIX(FC,RL)+= slev * SQRT1_2;
			_MATRIX(FC,RR)+= slev * SQRT1_2;
		} else if (dst_mask & _MASK(MONO)){
			spa_log_info(mix->log, "assign RL+RR to MONO (%f)", 1.0f);
			for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++) {
				matrix[i][_CH(RL)]= 1.0f;
				matrix[i][_CH(RR)]= 1.0f;
			}
			normalize = true;
		} else {
			spa_log_warn(mix->log, "can't assign RL");
		}
	}

	if (unassigned & SIDE) {
		if (dst_mask & REAR) {
			if (src_mask & _MASK(RL)) {
				spa_log_info(mix->log, "assign SL+SR to RL+RR (%f)", SQRT1_2);
				_MATRIX(RL,SL) += SQRT1_2;
				_MATRIX(RR,SR) += SQRT1_2;
			} else {
				spa_log_info(mix->log, "assign SL+SR to RL+RR (%f)", 1.0f);
				_MATRIX(RL,SL) += 1.0f;
				_MATRIX(RR,SR) += 1.0f;
			}
			keep &= ~REAR;
		} else if (dst_mask & _MASK(RC)) {
			spa_log_info(mix->log, "assign SL+SR to RC (%f)", SQRT1_2);
			_MATRIX(RC,SL)+= SQRT1_2;
			_MATRIX(RC,SR)+= SQRT1_2;
		} else if (dst_mask & STEREO) {
			if (matrix_encoding == MATRIX_DOLBY) {
				spa_log_info(mix->log, "assign SL+SR to FL+FR (%f)",
						slev * SQRT1_2);
				_MATRIX(FL,SL) -= slev * SQRT1_2;
				_MATRIX(FL,SR) -= slev * SQRT1_2;
				_MATRIX(FR,SL) += slev * SQRT1_2;
				_MATRIX(FR,SR) += slev * SQRT1_2;
			} else if (matrix_encoding == MATRIX_DPLII) {
				spa_log_info(mix->log, "assign SL+SR to FL+FR (%f / %f)",
						slev * SQRT3_2, slev * SQRT1_2);
				_MATRIX(FL,SL) -= slev * SQRT3_2;
				_MATRIX(FL,SR) -= slev * SQRT1_2;
				_MATRIX(FR,SL) += slev * SQRT1_2;
				_MATRIX(FR,SR) += slev * SQRT3_2;
			} else {
				spa_log_info(mix->log, "assign SL+SR to FL+FR (%f)", slev);
				_MATRIX(FL,SL) += slev;
				_MATRIX(FR,SR) += slev;
			}
		} else if (dst_mask & FRONT) {
			spa_log_info(mix->log, "assign SL+SR to FC (%f)", slev * SQRT1_2);
			_MATRIX(FC,SL) += slev * SQRT1_2;
			_MATRIX(FC,SR) += slev * SQRT1_2;
		} else if (dst_mask & _MASK(MONO)){
			spa_log_info(mix->log, "assign SL+SR to MONO (%f)", 1.0f);
			for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++) {
				matrix[i][_CH(SL)]= 1.0f;
				matrix[i][_CH(SR)]= 1.0f;
			}
			normalize = true;
		} else {
			spa_log_warn(mix->log, "can't assign SL");
		}
	}

	if (unassigned & _MASK(FLC)) {
		if (dst_mask & STEREO) {
			spa_log_info(mix->log, "assign FLC+FRC to FL+FR (%f)", 1.0f);
			_MATRIX(FL,FLC)+= 1.0f;
			_MATRIX(FR,FRC)+= 1.0f;
		} else if(dst_mask & FRONT) {
			spa_log_info(mix->log, "assign FLC+FRC to FC (%f)", SQRT1_2);
			_MATRIX(FC,FLC)+= SQRT1_2;
			_MATRIX(FC,FRC)+= SQRT1_2;
		} else if (dst_mask & _MASK(MONO)){
			spa_log_info(mix->log, "assign FLC+FRC to MONO (%f)", 1.0f);
			for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++) {
				matrix[i][_CH(FLC)]= 1.0f;
				matrix[i][_CH(FRC)]= 1.0f;
			}
			normalize = true;
		} else {
			spa_log_warn(mix->log, "can't assign FLC");
		}
	}
	if (unassigned & _MASK(LFE) &&
	    SPA_FLAG_IS_SET(mix->options, CHANNELMIX_OPTION_MIX_LFE)) {
		if (dst_mask & FRONT) {
			spa_log_info(mix->log, "assign LFE to FC (%f)", llev);
			_MATRIX(FC,LFE) += llev;
		} else if (dst_mask & STEREO) {
			spa_log_info(mix->log, "assign LFE to FL+FR (%f)",
					llev * SQRT1_2);
			_MATRIX(FL,LFE) += llev * SQRT1_2;
			_MATRIX(FR,LFE) += llev * SQRT1_2;
		} else if ((dst_mask & _MASK(MONO))){
			spa_log_info(mix->log, "assign LFE to MONO (%f)", 1.0f);
			for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++)
				matrix[i][_CH(LFE)]= 1.0f;
			normalize = true;
		} else {
			spa_log_warn(mix->log, "can't assign LFE");
		}
	}

	unassigned = dst_mask & ~src_mask & keep;

	spa_log_info(mix->log, "unassigned upmix %08"PRIx64" lfe:%f",
			unassigned, mix->lfe_cutoff);

	if (unassigned & STEREO) {
		if ((src_mask & FRONT) == FRONT) {
			spa_log_info(mix->log, "produce STEREO from FC (%f)", clev);
			_MATRIX(FL,FC) += clev;
			_MATRIX(FR,FC) += clev;
		} else if (src_mask & _MASK(MONO)) {
			spa_log_info(mix->log, "produce STEREO from MONO (%f)", 1.0f);
			_MATRIX(FL,MONO) += 1.0f;
			_MATRIX(FR,MONO) += 1.0f;
		} else {
			spa_log_warn(mix->log, "can't produce STEREO");
		}
	}
	if (unassigned & FRONT) {
		if ((src_mask & STEREO) == STEREO) {
			spa_log_info(mix->log, "produce FC from STEREO (%f)", clev);
			_MATRIX(FC,FL) += clev;
			_MATRIX(FC,FR) += clev;
			filter_fc = true;
		} else if (src_mask & _MASK(MONO)) {
			spa_log_info(mix->log, "produce FC from MONO (%f)", 1.0f);
			_MATRIX(FC,MONO) += 1.0f;
			filter_fc = true;
		} else {
			spa_log_warn(mix->log, "can't produce FC");
		}
	}
	if (unassigned & _MASK(LFE)) {
		if ((src_mask & STEREO) == STEREO) {
			spa_log_info(mix->log, "produce LFE from STEREO (%f)", llev);
			_MATRIX(LFE,FL) += llev;
			_MATRIX(LFE,FR) += llev;
			filter_lfe = true;
		} else if ((src_mask & FRONT) == FRONT) {
			spa_log_info(mix->log, "produce LFE from FC (%f)", llev);
			_MATRIX(LFE,FC) += llev;
			filter_lfe = true;
		} else if (src_mask & _MASK(MONO)) {
			spa_log_info(mix->log, "produce LFE from MONO (%f)", 1.0f);
			_MATRIX(LFE,MONO) += 1.0f;
			filter_lfe = true;
		} else {
			spa_log_warn(mix->log, "can't produce LFE");
		}
	}
	if (unassigned & SIDE) {
		if ((src_mask & REAR) == REAR) {
			spa_log_info(mix->log, "produce SIDE from REAR (%f)", 1.0f);
			_MATRIX(SL,RL) += 1.0f;
			_MATRIX(SR,RR) += 1.0f;
		} else if ((src_mask & STEREO) == STEREO) {
			spa_log_info(mix->log, "produce SIDE from STEREO (%f)", slev);
			_MATRIX(SL,FL) += slev;
			_MATRIX(SR,FR) += slev;
		} else if ((src_mask & FRONT) == FRONT &&
			mix->upmix == CHANNELMIX_UPMIX_SIMPLE) {
			spa_log_info(mix->log, "produce SIDE from FC (%f)", clev);
			_MATRIX(SL,FC) += clev;
			_MATRIX(SR,FC) += clev;
		} else if (src_mask & _MASK(MONO) &&
			mix->upmix == CHANNELMIX_UPMIX_SIMPLE) {
			spa_log_info(mix->log, "produce SIDE from MONO (%f)", 1.0f);
			_MATRIX(SL,MONO) += 1.0f;
			_MATRIX(SR,MONO) += 1.0f;
		} else {
			spa_log_info(mix->log, "won't produce SIDE");
		}
	}
	if (unassigned & REAR) {
		if ((src_mask & SIDE) == SIDE) {
			spa_log_info(mix->log, "produce REAR from SIDE (%f)", 1.0f);
			_MATRIX(RL,SL) += 1.0f;
			_MATRIX(RR,SR) += 1.0f;
		} else if ((src_mask & STEREO) == STEREO) {
			spa_log_info(mix->log, "produce REAR from STEREO (%f)", slev);
			_MATRIX(RL,FL) += slev;
			_MATRIX(RR,FR) += slev;
		} else if ((src_mask & FRONT) == FRONT &&
			mix->upmix == CHANNELMIX_UPMIX_SIMPLE) {
			spa_log_info(mix->log, "produce REAR from FC (%f)", clev);
			_MATRIX(RL,FC) += clev;
			_MATRIX(RR,FC) += clev;
		} else if (src_mask & _MASK(MONO) &&
			mix->upmix == CHANNELMIX_UPMIX_SIMPLE) {
			spa_log_info(mix->log, "produce REAR from MONO (%f)", 1.0f);
			_MATRIX(RL,MONO) += 1.0f;
			_MATRIX(RR,MONO) += 1.0f;
		} else {
			spa_log_info(mix->log, "won't produce SIDE");
		}
	}
	if (unassigned & _MASK(RC)) {
		if ((src_mask & REAR) == REAR) {
			spa_log_info(mix->log, "produce RC from REAR (%f)", 0.5f);
			_MATRIX(RC,RL) += 0.5f;
			_MATRIX(RC,RR) += 0.5f;
		} else if ((src_mask & SIDE) == SIDE) {
			spa_log_info(mix->log, "produce RC from SIDE (%f)", 0.5f);
			_MATRIX(RC,SL) += 0.5f;
			_MATRIX(RC,SR) += 0.5f;
		} else if ((src_mask & STEREO) == STEREO) {
			spa_log_info(mix->log, "produce RC from STEREO (%f)", 0.5f);
			_MATRIX(RC,FL) += 0.5f;
			_MATRIX(RC,FR) += 0.5f;
		} else if ((src_mask & FRONT) == FRONT &&
			mix->upmix == CHANNELMIX_UPMIX_SIMPLE) {
			spa_log_info(mix->log, "produce RC from FC (%f)", slev);
			_MATRIX(RC,FC) += slev;
		} else if (src_mask & _MASK(MONO) &&
			mix->upmix == CHANNELMIX_UPMIX_SIMPLE) {
			spa_log_info(mix->log, "produce RC from MONO (%f)", 1.0f);
			_MATRIX(RC,MONO) += 1.0f;
		} else {
			spa_log_info(mix->log, "won't produce RC");
		}
	}

done:
	if (dst_paired == 0)
		dst_paired = ~0LU;
	if (src_paired == 0)
		src_paired = ~0LU;

	for (jc = 0, ic = 0, i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++) {
		float sum = 0.0f;
		char str1[1024], str2[1024];
		struct spa_strbuf sb1, sb2;

		spa_strbuf_init(&sb1, str1, sizeof(str1));
		spa_strbuf_init(&sb2, str2, sizeof(str2));

		if ((dst_paired & (1UL << i)) == 0)
			continue;
		for (jc = 0, j = 0; j < SPA_AUDIO_MAX_CHANNELS; j++) {
			if ((src_paired & (1UL << j)) == 0)
				continue;
			if (ic >= dst_chan || jc >= src_chan)
				continue;

			if (ic == 0)
				spa_strbuf_append(&sb2, "%-4.4s  ",
						src_mask == 0 ? "UNK" :
						spa_debug_type_find_short_name(spa_type_audio_channel, j + _SH));

			mix->matrix_orig[ic][jc++] = matrix[i][j];
			sum += fabs(matrix[i][j]);

			if (matrix[i][j] == 0.0f)
				spa_strbuf_append(&sb1, "      ");
			else
				spa_strbuf_append(&sb1, "%1.3f ", matrix[i][j]);
		}
		if (sb2.pos > 0)
			spa_log_info(mix->log, "     %s", str2);
		if (sb1.pos > 0) {
			spa_log_info(mix->log, "%-4.4s %s   %f",
					dst_mask == 0 ? "UNK" :
					spa_debug_type_find_short_name(spa_type_audio_channel, i + _SH),
					str1, sum);
		}

		maxsum = SPA_MAX(maxsum, sum);
		if (i == _CH(LFE) && mix->lfe_cutoff > 0.0f && filter_lfe) {
			spa_log_info(mix->log, "channel %d is LFE cutoff:%f", ic, mix->lfe_cutoff);
			lr4_set(&mix->lr4[ic], BQ_LOWPASS, mix->lfe_cutoff / mix->freq);
		} else if (i == _CH(FC) && mix->fc_cutoff > 0.0f && filter_fc) {
			spa_log_info(mix->log, "channel %d is FC cutoff:%f", ic, mix->fc_cutoff);
			lr4_set(&mix->lr4[ic], BQ_LOWPASS, mix->fc_cutoff / mix->freq);
		} else {
			mix->lr4[ic].active = false;
		}
		ic++;
	}
	if (normalize && maxsum > 1.0f) {
		spa_log_info(mix->log, "normalize %f", maxsum);
		for (i = 0; i < dst_chan; i++)
			for (j = 0; j < src_chan; j++)
		                mix->matrix_orig[i][j] /= maxsum;
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

	spa_log_debug(mix->log, "volume:%f mute:%d n_volumes:%d", volume, mute, n_channel_volumes);

	/** apply global volume to channels */
	for (i = 0; i < n_channel_volumes; i++) {
		volumes[i] = channel_volumes[i] * vol;
		spa_log_debug(mix->log, "%d: %f * %f = %f", i, channel_volumes[i], vol, volumes[i]);
	}

	/** apply volumes per channel */
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
	} else if (n_channel_volumes == 0) {
		for (i = 0; i < dst_chan; i++) {
			for (j = 0; j < src_chan; j++) {
				mix->matrix[i][j] = mix->matrix_orig[i][j] * vol;
			}
		}
	}

	SPA_FLAG_SET(mix->flags, CHANNELMIX_FLAG_ZERO);
	SPA_FLAG_SET(mix->flags, CHANNELMIX_FLAG_EQUAL);
	SPA_FLAG_SET(mix->flags, CHANNELMIX_FLAG_COPY);

	t = 0.0;
	for (i = 0; i < dst_chan; i++) {
		for (j = 0; j < src_chan; j++) {
			float v = mix->matrix[i][j];
			spa_log_debug(mix->log, "%d %d: %f", i, j, v);
			if (i == 0 && j == 0)
				t = v;
			else if (t != v)
				SPA_FLAG_CLEAR(mix->flags, CHANNELMIX_FLAG_EQUAL);
			if (v != 0.0)
				SPA_FLAG_CLEAR(mix->flags, CHANNELMIX_FLAG_ZERO);
			if ((i == j && v != 1.0f) ||
			    (i != j && v != 0.0f))
				SPA_FLAG_CLEAR(mix->flags, CHANNELMIX_FLAG_COPY);
		}
	}
	SPA_FLAG_UPDATE(mix->flags, CHANNELMIX_FLAG_IDENTITY,
			dst_chan == src_chan && SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_COPY));

	spa_log_debug(mix->log, "flags:%08x", mix->flags);
}

static void impl_channelmix_free(struct channelmix *mix)
{
	mix->process = NULL;
}

int channelmix_init(struct channelmix *mix)
{
	const struct channelmix_info *info;

	if (mix->src_chan > SPA_AUDIO_MAX_CHANNELS ||
	    mix->dst_chan > SPA_AUDIO_MAX_CHANNELS)
		return -EINVAL;

	info = find_channelmix_info(mix->src_chan, mix->src_mask, mix->dst_chan, mix->dst_mask,
			mix->cpu_flags);
	if (info == NULL)
		return -ENOTSUP;

	mix->free = impl_channelmix_free;
	mix->process = info->process;
	mix->set_volume = impl_channelmix_set_volume;
	mix->cpu_flags = info->cpu_flags;
	mix->delay = mix->rear_delay * mix->freq / 1000.0f;
	mix->func_name = info->name;

	spa_log_debug(mix->log, "selected %s delay:%d options:%08x", info->name, mix->delay,
			mix->options);

	if (mix->hilbert_taps > 0) {
		mix->n_taps = SPA_CLAMP(mix->hilbert_taps, 15u, 255u) | 1;
		blackman_window(mix->taps, mix->n_taps);
		hilbert_generate(mix->taps, mix->n_taps);
	} else {
		mix->n_taps = 1;
		mix->taps[0] = 1.0f;
	}
	return make_matrix(mix);
}
