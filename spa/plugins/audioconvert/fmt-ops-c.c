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

#include <spa/support/cpu.h>
#include <spa/utils/defs.h>
#include <spa/param/audio/format-utils.h>

#include "fmt-ops.h"
#include "law.h"

void
conv_copy8d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	for (i = 0; i < n_channels; i++)
		spa_memcpy(dst[i], src[i], n_samples);
}

void
conv_copy8_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	spa_memcpy(dst[0], src[0], n_samples * conv->n_channels);
}


void
conv_copy16d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	for (i = 0; i < n_channels; i++)
		spa_memcpy(dst[i], src[i], n_samples * sizeof(int16_t));
}

void
conv_copy16_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	spa_memcpy(dst[0], src[0], n_samples * sizeof(int16_t) * conv->n_channels);
}

void
conv_copy24d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	for (i = 0; i < n_channels; i++)
		spa_memcpy(dst[i], src[i], n_samples * 3);
}

void
conv_copy24_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	spa_memcpy(dst[0], src[0], n_samples * 3 * conv->n_channels);
}

void
conv_copy32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	for (i = 0; i < n_channels; i++)
		spa_memcpy(dst[i], src[i], n_samples * sizeof(int32_t));
}

void
conv_copy32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	spa_memcpy(dst[0], src[0], n_samples * sizeof(int32_t) * conv->n_channels);
}

void
conv_copy64d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	for (i = 0; i < n_channels; i++)
		spa_memcpy(dst[i], src[i], n_samples * sizeof(int64_t));
}

void
conv_copy64_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	spa_memcpy(dst[0], src[0], n_samples * sizeof(int64_t) * conv->n_channels);
}

void
conv_u8d_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, n_channels = conv->n_channels;

	for (i = 0; i < n_channels; i++) {
		const uint8_t *s = src[i];
		float *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = U8_TO_F32(s[j]);
	}
}

void
conv_u8_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const uint8_t *s = src[0];
	float *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = U8_TO_F32(s[i]);
}

void
conv_u8_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint8_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = U8_TO_F32(*s++);
	}
}

void
conv_u8d_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint8_t **s = (const uint8_t **) src;
	float *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = U8_TO_F32(s[i][j]);
	}
}

void
conv_s8d_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, n_channels = conv->n_channels;

	for (i = 0; i < n_channels; i++) {
		const int8_t *s = src[i];
		float *d = dst[i];
		for (j = 0; j < n_samples; j++)
			d[j] = S8_TO_F32(s[j]);
	}
}

void
conv_s8_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const int8_t *s = src[0];
	float *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = S8_TO_F32(s[i]);
}

void
conv_s8_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int8_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = S8_TO_F32(*s++);
	}
}

void
conv_s8d_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int8_t **s = (const int8_t **) src;
	float *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = S8_TO_F32(s[i][j]);
	}
}

void
conv_alaw_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint8_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = alaw_to_f32(*s++);
	}
}

void
conv_ulaw_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint8_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = ulaw_to_f32(*s++);
	}
}

void
conv_u16_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const uint16_t *s = src[0];
	float *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = U16_TO_F32(s[i]);
}

void
conv_u16_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint16_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = U16_TO_F32(*s++);
	}
}

void
conv_s16d_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, n_channels = conv->n_channels;

	for (i = 0; i < n_channels; i++) {
		const int16_t *s = src[i];
		float *d = dst[i];
		for (j = 0; j < n_samples; j++)
			d[j] = S16_TO_F32(s[j]);
	}
}

void
conv_s16_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const int16_t *s = src[0];
	float *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = S16_TO_F32(s[i]);
}

void
conv_s16_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int16_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = S16_TO_F32(*s++);
	}
}

void
conv_s16s_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int16_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = S16S_TO_F32(*s++);
	}
}

void
conv_s16d_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int16_t **s = (const int16_t **) src;
	float *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = S16_TO_F32(s[i][j]);
	}
}

void
conv_u32_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const uint32_t *s = src[0];
	float *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = U32_TO_F32(s[i]);
}

void
conv_u32_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint32_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = U32_TO_F32(*s++);
	}
}

void
conv_s32d_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, n_channels = conv->n_channels;

	for (i = 0; i < n_channels; i++) {
		const int32_t *s = src[i];
		float *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = S32_TO_F32(s[j]);
	}
}

void
conv_s32_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const int32_t *s = src[0];
	float *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = S32_TO_F32(s[i]);
}

void
conv_s32_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int32_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = S32_TO_F32(*s++);
	}
}

void
conv_s32s_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int32_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = S32S_TO_F32(*s++);
	}
}

void
conv_s32d_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int32_t **s = (const int32_t **) src;
	float *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = S32_TO_F32(s[i][j]);
	}
}

void
conv_u24_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const uint8_t *s = src[0];
	float *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++) {
		d[i] = U24_TO_F32(read_u24(s));
		s += 3;
	}
}

void
conv_u24_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint8_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			d[i][j] = U24_TO_F32(read_u24(s));
			s += 3;
		}
	}
}

void
conv_s24d_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, n_channels = conv->n_channels;

	for (i = 0; i < n_channels; i++) {
		const int8_t *s = src[i];
		float *d = dst[i];

		for (j = 0; j < n_samples; j++) {
			d[j] = S24_TO_F32(read_s24(s));
			s += 3;
		}
	}
}

void
conv_s24_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const int8_t *s = src[0];
	float *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++) {
		d[i] = S24_TO_F32(read_s24(s));
		s += 3;
	}
}

void
conv_s24_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint8_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			d[i][j] = S24_TO_F32(read_s24(s));
			s += 3;
		}
	}
}

void
conv_s24s_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint8_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			d[i][j] = S24_TO_F32(read_s24s(s));
			s += 3;
		}
	}
}

void
conv_s24d_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint8_t **s = (const uint8_t **) src;
	float *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			*d++ = S24_TO_F32(read_s24(&s[i][j*3]));
		}
	}
}

void
conv_u24_32_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const uint32_t *s = src[0];
	float *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++) {
		d[i] = U24_32_TO_F32(s[i]);
	}
}

void
conv_u24_32_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint32_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = U24_32_TO_F32(*s++);
	}
}

void
conv_s24_32d_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, n_channels = conv->n_channels;

	for (i = 0; i < n_channels; i++) {
		const int32_t *s = src[i];
		float *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = S24_32_TO_F32(s[j]);
	}
}

void
conv_s24_32_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const int32_t *s = src[0];
	float *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++) {
		d[i] = S24_32_TO_F32(s[i]);
	}
}

void
conv_s24_32_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int32_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = S24_32_TO_F32(*s++);
	}
}

void
conv_s24_32s_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int32_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = S24_32S_TO_F32(*s++);
	}
}

void
conv_s24_32d_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int32_t **s = (const int32_t **) src;
	float *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = S24_32_TO_F32(s[i][j]);
	}
}

void
conv_f64d_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, n_channels = conv->n_channels;

	for (i = 0; i < n_channels; i++) {
		const double *s = src[i];
		float *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = s[j];
	}
}

void
conv_f64_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const double *s = src[0];
	float *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = s[i];
}

void
conv_f64_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const double *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = *s++;
	}
}

void
conv_f64s_to_f32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const double *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = bswap_64(*s++);
	}
}

void
conv_f64d_to_f32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const double **s = (const double **) src;
	float *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = s[i][j];
	}
}

static inline int32_t
lcnoise(uint32_t *state)
{
        *state = (*state * 96314165) + 907633515;
        return (int32_t)(*state);
}

static inline void update_dither_c(struct convert *conv, uint32_t n_samples)
{
	uint32_t n;
	float *dither = conv->dither, scale = conv->scale;
	uint32_t *state = &conv->random[0];

	for (n = 0; n < n_samples; n++)
		dither[n] = lcnoise(state) * scale;
}

#define SHAPER5(type,s,scale,offs,sh,min,max,d)			\
({								\
	type t;							\
	float v = s * scale + offs +				\
		- sh->e[idx] * 2.033f				\
		+ sh->e[(idx - 1) & NS_MASK] * 2.165f		\
		- sh->e[(idx - 2) & NS_MASK] * 1.959f		\
		+ sh->e[(idx - 3) & NS_MASK] * 1.590f		\
		- sh->e[(idx - 4) & NS_MASK] * 0.6149f;		\
	t = (type)SPA_CLAMP(v + d, min, max);			\
	idx = (idx + 1) & NS_MASK;				\
	sh->e[idx] = t - v;					\
	t;							\
})

#define F32_TO_U8_SH(s,sh,d)	SHAPER5(uint8_t, s, U8_SCALE, U8_OFFS, sh, U8_MIN, U8_MAX, d)
#define F32_TO_S8_SH(s,sh,d)	SHAPER5(int8_t, s, S8_SCALE, 0, sh, S8_MIN, S8_MAX, d)
#define F32_TO_S16_SH(s,sh,d)	SHAPER5(int16_t, s, S16_SCALE, 0, sh, S16_MIN, S16_MAX, d)
#define F32_TO_S16S_SH(s,sh,d)	bswap_16(F32_TO_S16_SH(s,sh,d))

void
conv_f32d_to_u8d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, n_channels = conv->n_channels;

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		uint8_t *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = F32_TO_U8(s[j]);
	}
}

void
conv_f32d_to_u8d_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		uint8_t *d = dst[i];

		for (j = 0; j < n_samples;) {
			chunk = SPA_MIN(n_samples - j, dither_size);
			for (k = 0; k < chunk; k++, j++)
				d[j] = F32_TO_U8_D(s[j], dither[k]);
		}
	}
}

void
conv_f32d_to_u8d_shaped_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		uint8_t *d = dst[i];
		struct shaper *sh = &conv->shaper[i];
		uint32_t idx = sh->idx;

		for (j = 0; j < n_samples;) {
			chunk = SPA_MIN(n_samples - j, dither_size);
			for (k = 0; k < chunk; k++, j++)
				d[j] = F32_TO_U8_SH(s[j], sh, dither[k]);
		}
		sh->idx = idx;
	}
}

void
conv_f32_to_u8_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const float *s = src[0];
	uint8_t *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = F32_TO_U8(s[i]);
}

void
conv_f32_to_u8d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float *s = src[0];
	uint8_t **d = (uint8_t **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = F32_TO_U8(*s++);
	}
}

void
conv_f32d_to_u8_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	uint8_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = F32_TO_U8(s[i][j]);
	}
}

void
conv_f32d_to_u8_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	uint8_t *d = dst[0];
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (j = 0; j < n_samples;) {
		chunk = SPA_MIN(n_samples - j, dither_size);
		for (k = 0; k < chunk; k++, j++) {
			for (i = 0; i < n_channels; i++)
				*d++ = F32_TO_U8_D(s[i][j], dither[k]);
		}
	}
}

void
conv_f32d_to_u8_shaped_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint8_t *d0 = dst[0];
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		uint8_t *d = &d0[i];
		struct shaper *sh = &conv->shaper[i];
		uint32_t idx = sh->idx;

		for (j = 0; j < n_samples;) {
			chunk = SPA_MIN(n_samples - j, dither_size);
			for (k = 0; k < chunk; k++, j++)
				d[j * n_channels] = F32_TO_U8_SH(s[j], sh, dither[k]);
		}
		sh->idx = idx;
	}
}

void
conv_f32d_to_s8d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, n_channels = conv->n_channels;

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int8_t *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = F32_TO_S8(s[j]);
	}
}

void
conv_f32d_to_s8d_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int8_t *d = dst[i];

		for (j = 0; j < n_samples;) {
			chunk = SPA_MIN(n_samples - j, dither_size);
			for (k = 0; k < chunk; k++, j++)
				d[j] = F32_TO_S8_D(s[j], dither[k]);
		}
	}
}

void
conv_f32d_to_s8d_shaped_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int8_t *d = dst[i];
		struct shaper *sh = &conv->shaper[i];
		uint32_t idx = sh->idx;

		for (j = 0; j < n_samples;) {
			chunk = SPA_MIN(n_samples - j, dither_size);
			for (k = 0; k < chunk; k++, j++)
				d[j] = F32_TO_S8_SH(s[j], sh, dither[k]);
		}
		sh->idx = idx;
	}
}

void
conv_f32_to_s8_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const float *s = src[0];
	int8_t *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = F32_TO_S8(s[i]);
}

void
conv_f32_to_s8d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float *s = src[0];
	int8_t **d = (int8_t **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = F32_TO_S8(*s++);
	}
}

void
conv_f32d_to_s8_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	int8_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = F32_TO_S8(s[i][j]);
	}
}

void
conv_f32d_to_s8_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	int8_t *d = dst[0];
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (j = 0; j < n_samples;) {
		chunk = SPA_MIN(n_samples - j, dither_size);
		for (k = 0; k < chunk; k++, j++) {
			for (i = 0; i < n_channels; i++)
				*d++ = F32_TO_S8_D(s[i][j], dither[k]);
		}
	}
}

void
conv_f32d_to_s8_shaped_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	int8_t *d0 = dst[0];
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int8_t *d = &d0[i];
		struct shaper *sh = &conv->shaper[i];
		uint32_t idx = sh->idx;

		for (j = 0; j < n_samples;) {
			chunk = SPA_MIN(n_samples - j, dither_size);
			for (k = 0; k < chunk; k++, j++)
				d[j * n_channels] = F32_TO_S8_SH(s[j], sh, dither[k]);
		}
		sh->idx = idx;
	}
}

void
conv_f32d_to_alaw_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	int8_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = f32_to_alaw(s[i][j]);
	}
}

void
conv_f32d_to_ulaw_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	int8_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = f32_to_ulaw(s[i][j]);
	}
}

void
conv_f32_to_u16_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const float *s = src[0];
	uint16_t *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = F32_TO_U16(s[i]);
}
void
conv_f32d_to_u16_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	uint16_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = F32_TO_U16(s[i][j]);
	}
}

void
conv_f32d_to_s16d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, n_channels = conv->n_channels;

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int16_t *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = F32_TO_S16(s[j]);
	}
}

void
conv_f32d_to_s16d_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int16_t *d = dst[i];

		for (j = 0; j < n_samples;) {
			chunk = SPA_MIN(n_samples - j, dither_size);
			for (k = 0; k < chunk; k++, j++)
				d[j] = F32_TO_S16_D(s[j], dither[k]);
		}
	}
}


void
conv_f32d_to_s16d_shaped_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int16_t *d = dst[i];
		struct shaper *sh = &conv->shaper[i];
		uint32_t idx = sh->idx;

		for (j = 0; j < n_samples;) {
			chunk = SPA_MIN(n_samples - j, dither_size);
			for (k = 0; k < chunk; k++, j++)
				d[j] = F32_TO_S16_SH(s[j], sh, dither[k]);
		}
		sh->idx = idx;
	}
}

void
conv_f32_to_s16_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const float *s = src[0];
	int16_t *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = F32_TO_S16(s[i]);
}

void
conv_f32_to_s16d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float *s = src[0];
	int16_t **d = (int16_t **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = F32_TO_S16(*s++);
	}
}

void
conv_f32d_to_s16_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	int16_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = F32_TO_S16(s[i][j]);
	}
}

void
conv_f32d_to_s16_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	int16_t *d = dst[0];
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (j = 0; j < n_samples;) {
		chunk = SPA_MIN(n_samples - j, dither_size);
		for (k = 0; k < chunk; k++, j++) {
			for (i = 0; i < n_channels; i++)
				*d++ = F32_TO_S16_D(s[i][j], dither[k]);
		}
	}
}

void
conv_f32d_to_s16_shaped_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	int16_t *d0 = dst[0];
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int16_t *d = &d0[i];
		struct shaper *sh = &conv->shaper[i];
		uint32_t idx = sh->idx;

		for (j = 0; j < n_samples;) {
			chunk = SPA_MIN(n_samples - j, dither_size);
			for (k = 0; k < chunk; k++, j++)
				d[j * n_channels] = F32_TO_S16_SH(s[j], sh, dither[k]);
		}
		sh->idx = idx;
	}
}

void
conv_f32d_to_s16s_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	int16_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = F32_TO_S16S(s[i][j]);
	}
}

void
conv_f32d_to_s16s_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	uint16_t *d = dst[0];
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (j = 0; j < n_samples;) {
		chunk = SPA_MIN(n_samples - j, dither_size);
		for (k = 0; k < chunk; k++, j++) {
			for (i = 0; i < n_channels; i++)
				*d++ = F32_TO_S16S_D(s[i][j], dither[k]);
		}
	}
}

void
conv_f32d_to_s16s_shaped_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	int16_t *d0 = dst[0];
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int16_t *d = &d0[i];
		struct shaper *sh = &conv->shaper[i];
		uint32_t idx = sh->idx;

		for (j = 0; j < n_samples;) {
			chunk = SPA_MIN(n_samples - j, dither_size);
			for (k = 0; k < chunk; k++, j++)
				d[j * n_channels] = F32_TO_S16S_SH(s[j], sh, dither[k]);
		}
		sh->idx = idx;
	}
}
void
conv_f32_to_u32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const float *s = src[0];
	uint32_t *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = F32_TO_U32(s[i]);
}

void
conv_f32d_to_u32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	uint32_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = F32_TO_U32(s[i][j]);
	}
}

void
conv_f32d_to_s32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, n_channels = conv->n_channels;

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int32_t *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = F32_TO_S32(s[j]);
	}
}

void
conv_f32d_to_s32d_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int32_t *d = dst[i];

		for (j = 0; j < n_samples;) {
			chunk = SPA_MIN(n_samples - j, dither_size);
			for (k = 0; k < chunk; k++, j++)
				d[j] = F32_TO_S32_D(s[j], dither[k]);
		}
	}
}

void
conv_f32_to_s32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const float *s = src[0];
	int32_t *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = F32_TO_S32(s[i]);
}

void
conv_f32_to_s32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float *s = src[0];
	int32_t **d = (int32_t **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = F32_TO_S32(*s++);
	}
}

void
conv_f32d_to_s32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	int32_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = F32_TO_S32(s[i][j]);
	}
}

void
conv_f32d_to_s32_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	int32_t *d = dst[0];
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (j = 0; j < n_samples;) {
		chunk = SPA_MIN(n_samples - j, dither_size);
		for (k = 0; k < chunk; k++, j++) {
			for (i = 0; i < n_channels; i++)
				*d++ = F32_TO_S32_D(s[i][j], dither[k]);
		}
	}
}

void
conv_f32d_to_s32s_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	uint32_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = F32_TO_S32S(s[i][j]);
	}
}

void
conv_f32d_to_s32s_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	uint32_t *d = dst[0];
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (j = 0; j < n_samples;) {
		chunk = SPA_MIN(n_samples - j, dither_size);
		for (k = 0; k < chunk; k++, j++) {
			for (i = 0; i < n_channels; i++)
				*d++ = F32_TO_S32S_D(s[i][j], dither[k]);
		}
	}
}

void
conv_f32d_to_f64d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, n_channels = conv->n_channels;

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		double *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = s[j];
	}
}

void
conv_f32_to_f64_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const float *s = src[0];
	double *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = s[i];
}

void
conv_f32_to_f64d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float *s = src[0];
	double **d = (double **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = *s++;
	}
}

void
conv_f32d_to_f64_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	double *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = s[i][j];
	}
}

void
conv_f32d_to_f64s_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	double *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = bswap_32(s[i][j]);
	}
}

void
conv_f32_to_u24_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const float *s = src[0];
	uint8_t *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++) {
		write_u24(d, F32_TO_U24(s[i]));
		d += 3;
	}
}

void
conv_f32d_to_u24_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	uint8_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			write_u24(d, F32_TO_U24(s[i][j]));
			d += 3;
		}
	}
}

void
conv_f32d_to_s24d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, n_channels = conv->n_channels;

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		uint8_t *d = dst[i];

		for (j = 0; j < n_samples; j++) {
			write_s24(d, F32_TO_S24(s[j]));
			d += 3;
		}
	}
}

void
conv_f32d_to_s24d_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		uint8_t *d = dst[i];

		for (j = 0; j < n_samples;) {
			chunk = SPA_MIN(n_samples - j, dither_size);
			for (k = 0; k < chunk; k++, j++) {
				write_s24(d, F32_TO_S24_D(s[j], dither[k]));
				d += 3;
			}
		}
	}
}

void
conv_f32_to_s24_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const float *s = src[0];
	uint8_t *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++) {
		write_s24(d, F32_TO_S24(s[i]));
		d += 3;
	}
}

void
conv_f32_to_s24d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float *s = src[0];
	uint8_t **d = (uint8_t **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			write_s24(&d[i][j*3], F32_TO_S24(*s++));
		}
	}
}

void
conv_f32d_to_s24_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	uint8_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			write_s24(d, F32_TO_S24(s[i][j]));
			d += 3;
		}
	}
}

void
conv_f32d_to_s24_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	uint8_t *d = dst[0];
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (j = 0; j < n_samples;) {
		chunk = SPA_MIN(n_samples - j, dither_size);
		for (k = 0; k < chunk; k++, j++) {
			for (i = 0; i < n_channels; i++) {
				write_s24(d, F32_TO_S24_D(s[i][j], dither[k]));
				d += 3;
			}
		}
	}
}

void
conv_f32d_to_s24s_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	uint8_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			write_s24s(d, F32_TO_S24(s[i][j]));
			d += 3;
		}
	}
}

void
conv_f32d_to_s24s_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	uint8_t *d = dst[0];
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (j = 0; j < n_samples;) {
		chunk = SPA_MIN(n_samples - j, dither_size);
		for (k = 0; k < chunk; k++, j++) {
			for (i = 0; i < n_channels; i++) {
				write_s24s(d, F32_TO_S24_D(s[i][j], dither[k]));
				d += 3;
			}
		}
	}
}

void
conv_f32d_to_s24_32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, n_channels = conv->n_channels;

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int32_t *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = F32_TO_S24_32(s[j]);
	}
}

void
conv_f32d_to_s24_32d_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int32_t *d = dst[i];

		for (j = 0; j < n_samples;) {
			chunk = SPA_MIN(n_samples - j, dither_size);
			for (k = 0; k < chunk; k++, j++)
				d[j] = F32_TO_S24_32_D(s[j], dither[k]);
		}
	}
}

void
conv_f32_to_u24_32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const float *s = src[0];
	uint32_t *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = F32_TO_U24_32(s[i]);
}

void
conv_f32d_to_u24_32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	uint32_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = F32_TO_U24_32(s[i][j]);
	}
}

void
conv_f32_to_s24_32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	uint32_t i, n_channels = conv->n_channels;
	const float *s = src[0];
	int32_t *d = dst[0];

	n_samples *= n_channels;

	for (i = 0; i < n_samples; i++)
		d[i] = F32_TO_S24_32(s[i]);
}

void
conv_f32_to_s24_32d_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float *s = src[0];
	int32_t **d = (int32_t **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = F32_TO_S24_32(*s++);
	}
}

void
conv_f32d_to_s24_32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	int32_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = F32_TO_S24_32(s[i][j]);
	}
}

void
conv_f32d_to_s24_32_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	int32_t *d = dst[0];
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (j = 0; j < n_samples;) {
		chunk = SPA_MIN(n_samples - j, dither_size);
		for (k = 0; k < chunk; k++, j++) {
			for (i = 0; i < n_channels; i++)
				*d++ = F32_TO_S24_32_D(s[i][j], dither[k]);
		}
	}
}

void
conv_f32d_to_s24_32s_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	int32_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = F32_TO_S24_32S(s[i][j]);
	}
}

void
conv_f32d_to_s24_32s_dither_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const float **s = (const float **) src;
	int32_t *d = dst[0];
	uint32_t i, j, k, chunk, n_channels = conv->n_channels, dither_size = conv->dither_size;
	float *dither = conv->dither;

	update_dither_c(conv, SPA_MIN(n_samples, dither_size));

	for (j = 0; j < n_samples;) {
		chunk = SPA_MIN(n_samples - j, dither_size);
		for (k = 0; k < chunk; k++, j++) {
			for (i = 0; i < n_channels; i++)
				*d++ = F32_TO_S24_32S_D(s[i][j], dither[k]);
		}
	}
}

void
conv_deinterleave_8_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint8_t *s = src[0];
	uint8_t **d = (uint8_t **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = *s++;
	}
}

void
conv_deinterleave_16_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint16_t *s = src[0];
	uint16_t **d = (uint16_t **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = *s++;
	}
}

void
conv_deinterleave_24_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint8_t *s = src[0];
	uint8_t **d = (uint8_t **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			write_s24(&d[i][j*3], read_s24(s));
			s += 3;
		}
	}
}

void
conv_deinterleave_32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint32_t *s = src[0];
	uint32_t **d = (uint32_t **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = *s++;
	}
}

void
conv_deinterleave_32s_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint32_t *s = src[0];
	uint32_t **d = (uint32_t **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = bswap_32(*s++);
	}
}

void
conv_deinterleave_64_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const uint64_t *s = src[0];
	uint64_t **d = (uint64_t **) dst;
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = *s++;
	}
}

void
conv_interleave_8_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int8_t **s = (const int8_t **) src;
	uint8_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = s[i][j];
	}
}

void
conv_interleave_16_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int16_t **s = (const int16_t **) src;
	uint16_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = s[i][j];
	}
}

void
conv_interleave_24_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int8_t **s = (const int8_t **) src;
	uint8_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			write_s24(d, read_s24(&s[i][j*3]));
			d += 3;
		}
	}
}

void
conv_interleave_32_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int32_t **s = (const int32_t **) src;
	uint32_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = s[i][j];
	}
}

void
conv_interleave_32s_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int32_t **s = (const int32_t **) src;
	uint32_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = bswap_32(s[i][j]);
	}
}

void
conv_interleave_64_c(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int64_t **s = (const int64_t **) src;
	uint64_t *d = dst[0];
	uint32_t i, j, n_channels = conv->n_channels;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = s[i][j];
	}
}
