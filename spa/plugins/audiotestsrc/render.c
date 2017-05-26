/* Spa
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <math.h>

#define M_PI_M2 ( M_PI + M_PI )

#define DEFINE_SINE(type,scale)								\
static void										\
audio_test_src_create_sine_##type (struct impl *this, type * samples, size_t n_samples) \
{											\
	int i, c, channels;								\
	double step, amp;								\
											\
	channels = this->current_format.info.raw.channels;				\
	step = M_PI_M2 * this->props.freq / this->current_format.info.raw.rate;		\
	amp = this->props.volume * scale;						\
											\
	for (i = 0; i < n_samples; i++) {						\
		this->accumulator += step;						\
		if (this->accumulator >= M_PI_M2)					\
			this->accumulator -= M_PI_M2;					\
		for (c = 0; c < channels; ++c)						\
			*samples++ = (type) (sin (this->accumulator) * amp);		\
	}										\
}

DEFINE_SINE(int16_t, 32767.0);
DEFINE_SINE(int32_t, 2147483647.0);
DEFINE_SINE(float, 1.0);
DEFINE_SINE(double, 1.0);

static const render_func_t sine_funcs[] = {
	(render_func_t) audio_test_src_create_sine_int16_t,
	(render_func_t) audio_test_src_create_sine_int32_t,
	(render_func_t) audio_test_src_create_sine_float,
	(render_func_t) audio_test_src_create_sine_double
};
