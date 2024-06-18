/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <math.h>

#define M_PI_M2f ( M_PIf + M_PIf )

#define DEFINE_SINE(type,scale)								\
static void										\
audio_test_src_create_sine_##type (struct impl *this, type *samples, size_t n_samples)	\
{											\
	size_t i;									\
	uint32_t c, channels;								\
	float step, amp;								\
	float freq = this->props.freq;							\
	float volume = this->props.volume;						\
											\
	channels = this->port.current_format.info.raw.channels;				\
	step = M_PI_M2f * freq / this->port.current_format.info.raw.rate;		\
	amp = volume * scale;								\
											\
	for (i = 0; i < n_samples; i++) {						\
		type val;								\
		this->port.accumulator += step;						\
		if (this->port.accumulator >= M_PI_M2f)					\
			this->port.accumulator -= M_PI_M2f;				\
		val = (type) (sin (this->port.accumulator) * amp);			\
		for (c = 0; c < channels; ++c)						\
			*samples++ = val;						\
	}										\
}

DEFINE_SINE(int16_t, 32767.0f);
DEFINE_SINE(int32_t, 2147483647.0f);
DEFINE_SINE(float, 1.0f);
DEFINE_SINE(double, 1.0f);

static const render_func_t sine_funcs[] = {
	(render_func_t) audio_test_src_create_sine_int16_t,
	(render_func_t) audio_test_src_create_sine_int32_t,
	(render_func_t) audio_test_src_create_sine_float,
	(render_func_t) audio_test_src_create_sine_double
};
