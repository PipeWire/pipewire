/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>

#include <spa/utils/defs.h>
#include <spa/utils/string.h>
#include <spa/param/audio/raw.h>

#include "crossover.h"
#include "delay.h"

#define VOLUME_MIN 0.0f
#define VOLUME_NORM 1.0f

#define _M(ch)		(1UL << SPA_AUDIO_CHANNEL_ ## ch)
#define MASK_MONO	_M(FC)|_M(MONO)|_M(UNKNOWN)
#define MASK_STEREO	_M(FL)|_M(FR)|_M(UNKNOWN)
#define MASK_QUAD	_M(FL)|_M(FR)|_M(RL)|_M(RR)|_M(UNKNOWN)
#define MASK_3_1	_M(FL)|_M(FR)|_M(FC)|_M(LFE)
#define MASK_5_1	_M(FL)|_M(FR)|_M(FC)|_M(LFE)|_M(SL)|_M(SR)|_M(RL)|_M(RR)
#define MASK_7_1	_M(FL)|_M(FR)|_M(FC)|_M(LFE)|_M(SL)|_M(SR)|_M(RL)|_M(RR)

#define BUFFER_SIZE 4096
#define MAX_TAPS 255

struct channelmix {
	uint32_t src_chan;
	uint32_t dst_chan;
	uint64_t src_mask;
	uint64_t dst_mask;
	uint32_t cpu_flags;
#define CHANNELMIX_OPTION_MIX_LFE	(1<<0)		/**< mix LFE */
#define CHANNELMIX_OPTION_NORMALIZE	(1<<1)		/**< normalize volumes */
#define CHANNELMIX_OPTION_UPMIX		(1<<2)		/**< do simple upmixing */
	uint32_t options;
#define CHANNELMIX_UPMIX_NONE		0		/**< disable upmixing */
#define CHANNELMIX_UPMIX_SIMPLE		1		/**< simple upmixing */
#define CHANNELMIX_UPMIX_PSD		2		/**< Passive Surround Decoding upmixing */
	uint32_t upmix;

	struct spa_log *log;
	const char *func_name;

#define CHANNELMIX_FLAG_ZERO		(1<<0)		/**< all zero components */
#define CHANNELMIX_FLAG_IDENTITY	(1<<1)		/**< identity matrix */
#define CHANNELMIX_FLAG_EQUAL		(1<<2)		/**< all values are equal */
#define CHANNELMIX_FLAG_COPY		(1<<3)		/**< 1 on diagonal, can be nxm */
	uint32_t flags;
	float matrix_orig[SPA_AUDIO_MAX_CHANNELS][SPA_AUDIO_MAX_CHANNELS];
	float matrix[SPA_AUDIO_MAX_CHANNELS][SPA_AUDIO_MAX_CHANNELS];

	float freq;					/* sample frequency */
	float lfe_cutoff;				/* in Hz, 0 is disabled */
	float fc_cutoff;				/* in Hz, 0 is disabled */
	float rear_delay;				/* in ms, 0 is disabled */
	float widen;					/* stereo widen. 0 is disabled */
	uint32_t hilbert_taps;				/* to phase shift, 0 disabled */
	struct lr4 lr4[SPA_AUDIO_MAX_CHANNELS];

	float buffer[2][BUFFER_SIZE];
	uint32_t pos[2];
	uint32_t delay;
	float taps[MAX_TAPS];
	uint32_t n_taps;

	void (*process) (struct channelmix *mix, void * SPA_RESTRICT dst[],
			const void * SPA_RESTRICT src[], uint32_t n_samples);
	void (*set_volume) (struct channelmix *mix, float volume, bool mute,
			uint32_t n_channel_volumes, float *channel_volumes);
	void (*free) (struct channelmix *mix);

	void *data;
};

int channelmix_init(struct channelmix *mix);

static const struct channelmix_upmix_info {
	const char *label;
	const char *description;
	uint32_t upmix;
} channelmix_upmix_info[] = {
	[CHANNELMIX_UPMIX_NONE] = { "none", "Disabled", CHANNELMIX_UPMIX_NONE },
	[CHANNELMIX_UPMIX_SIMPLE] = { "simple", "Simple upmixing", CHANNELMIX_UPMIX_SIMPLE },
	[CHANNELMIX_UPMIX_PSD] = { "psd", "Passive Surround Decoding", CHANNELMIX_UPMIX_PSD }
};

static inline uint32_t channelmix_upmix_from_label(const char *label)
{
	SPA_FOR_EACH_ELEMENT_VAR(channelmix_upmix_info, i) {
		if (spa_streq(i->label, label))
			return i->upmix;
	}
	return CHANNELMIX_UPMIX_NONE;
}

#define channelmix_process(mix,...)	(mix)->process(mix, __VA_ARGS__)
#define channelmix_set_volume(mix,...)	(mix)->set_volume(mix, __VA_ARGS__)
#define channelmix_free(mix)		(mix)->free(mix)

#define DEFINE_FUNCTION(name,arch)						\
void channelmix_##name##_##arch(struct channelmix *mix,				\
		void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],	\
		uint32_t n_samples);

#define CHANNELMIX_OPS_MAX_ALIGN 16

DEFINE_FUNCTION(copy, c);
DEFINE_FUNCTION(f32_n_m, c);
DEFINE_FUNCTION(f32_1_2, c);
DEFINE_FUNCTION(f32_2_1, c);
DEFINE_FUNCTION(f32_4_1, c);
DEFINE_FUNCTION(f32_2_4, c);
DEFINE_FUNCTION(f32_2_3p1, c);
DEFINE_FUNCTION(f32_2_5p1, c);
DEFINE_FUNCTION(f32_2_7p1, c);
DEFINE_FUNCTION(f32_3p1_2, c);
DEFINE_FUNCTION(f32_5p1_2, c);
DEFINE_FUNCTION(f32_5p1_3p1, c);
DEFINE_FUNCTION(f32_5p1_4, c);
DEFINE_FUNCTION(f32_7p1_2, c);
DEFINE_FUNCTION(f32_7p1_3p1, c);
DEFINE_FUNCTION(f32_7p1_4, c);

#if defined (HAVE_SSE)
DEFINE_FUNCTION(copy, sse);
DEFINE_FUNCTION(f32_n_m, sse);
DEFINE_FUNCTION(f32_2_3p1, sse);
DEFINE_FUNCTION(f32_2_5p1, sse);
DEFINE_FUNCTION(f32_2_7p1, sse);
DEFINE_FUNCTION(f32_3p1_2, sse);
DEFINE_FUNCTION(f32_5p1_2, sse);
DEFINE_FUNCTION(f32_5p1_3p1, sse);
DEFINE_FUNCTION(f32_5p1_4, sse);
DEFINE_FUNCTION(f32_7p1_4, sse);
#endif

#undef DEFINE_FUNCTION
