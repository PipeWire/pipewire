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

static void
conv_copy8d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	uint32_t i;
	for (i = 0; i < n_channels; i++)
		spa_memcpy(dst[i], src[i], n_samples);
}

static void
conv_copy8(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	spa_memcpy(dst[0], src[0], n_samples * n_channels);
}


static void
conv_copy16d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	uint32_t i;
	for (i = 0; i < n_channels; i++)
		spa_memcpy(dst[i], src[i], n_samples * sizeof(int16_t));
}

static void
conv_copy16(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	spa_memcpy(dst[0], src[0], n_samples * sizeof(int16_t) * n_channels);
}

static void
conv_copy24d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	uint32_t i;
	for (i = 0; i < n_channels; i++)
		spa_memcpy(dst[i], src[i], n_samples * 3);
}

static void
conv_copy24(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	spa_memcpy(dst[0], src[0], n_samples * 3 * n_channels);
}

static void
conv_copy32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	uint32_t i;
	for (i = 0; i < n_channels; i++)
		spa_memcpy(dst[i], src[i], n_samples * sizeof(int32_t));
}

static void
conv_copy32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	spa_memcpy(dst[0], src[0], n_samples * sizeof(int32_t) * n_channels);
}

static void
conv_u8d_to_f32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	uint32_t i, j;

	for (i = 0; i < n_channels; i++) {
		const uint8_t *s = src[i];
		float *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = U8_TO_F32(s[j]);
	}
}

static void
conv_u8_to_f32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	conv_u8d_to_f32d(data, dst, src, 1, n_samples * n_channels);
}

static void
conv_u8_to_f32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const uint8_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = U8_TO_F32(*s++);
	}
}

static void
conv_u8d_to_f32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const uint8_t **s = (const uint8_t **) src;
	float *d = dst[0];
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = U8_TO_F32(s[i][j]);
	}
}

static void
conv_s16d_to_f32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	uint32_t i, j;

	for (i = 0; i < n_channels; i++) {
		const int16_t *s = src[i];
		float *d = dst[i];
		for (j = 0; j < n_samples; j++)
			d[j] = S16_TO_F32(s[j]);
	}
}

static void
conv_s16_to_f32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	conv_s16d_to_f32d(data, dst, src, 1, n_samples * n_channels);
}

static void
conv_s16_to_f32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const int16_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = S16_TO_F32(*s++);
	}
}

static void
conv_s16d_to_f32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const int16_t **s = (const int16_t **) src;
	float *d = dst[0];
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = S16_TO_F32(s[i][j]);
	}
}

static void
conv_s32d_to_f32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	uint32_t i, j;

	for (i = 0; i < n_channels; i++) {
		const int32_t *s = src[i];
		float *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = S32_TO_F32(s[j]);
	}
}

static void
conv_s32_to_f32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	conv_s32d_to_f32d(data, dst, src, 1, n_samples * n_channels);
}

static void
conv_s32_to_f32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const int32_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = S32_TO_F32(*s++);
	}
}

static void
conv_s32d_to_f32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const int32_t **s = (const int32_t **) src;
	float *d = dst[0];
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = S32_TO_F32(s[i][j]);
	}
}

static void
conv_s24d_to_f32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	uint32_t i, j;

	for (i = 0; i < n_channels; i++) {
		const int8_t *s = src[i];
		float *d = dst[i];

		for (j = 0; j < n_samples; j++) {
			d[j] = S24_TO_F32(read_s24(s));
			s += 3;
		}
	}
}

static void
conv_s24_to_f32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	conv_s24d_to_f32d(data, dst, src, 1, n_samples * n_channels);
}

static void
conv_s24_to_f32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const uint8_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			d[i][j] = S24_TO_F32(read_s24(s));
			s += 3;
		}
	}
}

static void
conv_s24d_to_f32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const uint8_t **s = (const uint8_t **) src;
	float *d = dst[0];
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			*d++ = S24_TO_F32(read_s24(&s[i][j*3]));
		}
	}
}

static void
conv_s24_32d_to_f32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	uint32_t i, j;

	for (i = 0; i < n_channels; i++) {
		const int32_t *s = src[i];
		float *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = S24_TO_F32(s[j]);
	}
}

static void
conv_s24_32_to_f32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	conv_s24_32d_to_f32d(data, dst, src, 1, n_samples * n_channels);
}

static void
conv_s24_32_to_f32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const int32_t *s = src[0];
	float **d = (float **) dst;
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = S24_TO_F32(*s++);
	}
}

static void
conv_s24_32d_to_f32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const int32_t **s = (const int32_t **) src;
	float *d = dst[0];
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = S24_TO_F32(s[i][j]);
	}
}

static void
conv_f32d_to_u8d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	uint32_t i, j;

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		uint8_t *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = F32_TO_U8(s[j]);
	}
}

static void
conv_f32_to_u8(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	conv_f32d_to_u8d(data, dst, src, 1, n_samples * n_channels);
}

static void
conv_f32_to_u8d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const float *s = src[0];
	uint8_t **d = (uint8_t **) dst;
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = F32_TO_U8(*s++);
	}
}

static void
conv_f32d_to_u8(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const float **s = (const float **) src;
	uint8_t *d = dst[0];
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = F32_TO_U8(s[i][j]);
	}
}

static void
conv_f32d_to_s16d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	uint32_t i, j;

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int16_t *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = F32_TO_S16(s[j]);
	}
}

static void
conv_f32_to_s16(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	conv_f32d_to_s16d(data, dst, src, 1, n_samples * n_channels);
}

static void
conv_f32_to_s16d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const float *s = src[0];
	int16_t **d = (int16_t **) dst;
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = F32_TO_S16(*s++);
	}
}

static void
conv_f32d_to_s16(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const float **s = (const float **) src;
	int16_t *d = dst[0];
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = F32_TO_S16(s[i][j]);
	}
}

static void
conv_f32d_to_s32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	uint32_t i, j;

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int32_t *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = F32_TO_S32(s[j]);
	}
}

static void
conv_f32_to_s32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	conv_f32d_to_s32d(data, dst, src, 1, n_samples * n_channels);
}

static void
conv_f32_to_s32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const float *s = src[0];
	int32_t **d = (int32_t **) dst;
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = F32_TO_S32(*s++);
	}
}

static void
conv_f32d_to_s32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const float **s = (const float **) src;
	int32_t *d = dst[0];
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = F32_TO_S32(s[i][j]);
	}
}



static void
conv_f32d_to_s24d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	uint32_t i, j;

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		uint8_t *d = dst[i];

		for (j = 0; j < n_samples; j++) {
			write_s24(d, F32_TO_S24(s[j]));
			d += 3;
		}
	}
}

static void
conv_f32_to_s24(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	conv_f32d_to_s24d(data, dst, src, 1, n_samples * n_channels);
}

static void
conv_f32_to_s24d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const float *s = src[0];
	uint8_t **d = (uint8_t **) dst;
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			write_s24(&d[i][j*3], F32_TO_S24(*s++));
		}
	}
}

static void
conv_f32d_to_s24(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const float **s = (const float **) src;
	uint8_t *d = dst[0];
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			write_s24(d, F32_TO_S24(s[i][j]));
			d += 3;
		}
	}
}


static void
conv_f32d_to_s24_32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	uint32_t i, j;

	for (i = 0; i < n_channels; i++) {
		const float *s = src[i];
		int32_t *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = F32_TO_S24(s[j]);
	}
}

static void
conv_f32_to_s24_32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	conv_f32d_to_s24_32d(data, dst, src, 1, n_samples * n_channels);
}

static void
conv_f32_to_s24_32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const float *s = src[0];
	int32_t **d = (int32_t **) dst;
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = F32_TO_S24(*s++);
	}
}

static void
conv_f32d_to_s24_32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const float **s = (const float **) src;
	int32_t *d = dst[0];
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = F32_TO_S24(s[i][j]);
	}
}

static void
deinterleave_8(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const uint8_t *s = src[0];
	uint8_t **d = (uint8_t **) dst;
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = *s++;
	}
}

static void
deinterleave_16(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const uint16_t *s = src[0];
	uint16_t **d = (uint16_t **) dst;
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = *s++;
	}
}

static void
deinterleave_24(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const uint8_t *s = src[0];
	uint8_t **d = (uint8_t **) dst;
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			write_s24(&d[i][j*3], read_s24(s));
			s += 3;
		}
	}
}

static void
deinterleave_32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const uint32_t *s = src[0];
	uint32_t **d = (uint32_t **) dst;
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			d[i][j] = *s++;
	}
}

static void
interleave_8(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const int8_t **s = (const int8_t **) src;
	uint8_t *d = dst[0];
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = s[i][j];
	}
}

static void
interleave_16(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const int16_t **s = (const int16_t **) src;
	uint16_t *d = dst[0];
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = s[i][j];
	}
}

static void
interleave_24(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const int8_t **s = (const int8_t **) src;
	uint8_t *d = dst[0];
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++) {
			write_s24(d, read_s24(&s[i][j*3]));
			d += 3;
		}
	}
}

static void
interleave_32(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[], uint32_t n_channels, uint32_t n_samples)
{
	const int32_t **s = (const int32_t **) src;
	uint32_t *d = dst[0];
	uint32_t i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_channels; i++)
			*d++ = s[i][j];
	}
}

struct conv_info {
	uint32_t src_fmt;
	uint32_t dst_fmt;
#define FEATURE_SSE2	SPA_CPU_FLAG_SSE2
#define FEATURE_SSE41	SPA_CPU_FLAG_SSE41
#define FEATURE_SSSE3	SPA_CPU_FLAG_SSSE3
	uint32_t features;

	convert_func_t func;
};

static struct conv_info conv_table[] =
{
	/* to f32 */
	{ SPA_AUDIO_FORMAT_U8, SPA_AUDIO_FORMAT_F32, 0, conv_u8_to_f32 },
	{ SPA_AUDIO_FORMAT_U8P, SPA_AUDIO_FORMAT_F32P, 0, conv_u8d_to_f32d },
	{ SPA_AUDIO_FORMAT_U8, SPA_AUDIO_FORMAT_F32P, 0, conv_u8_to_f32d },
	{ SPA_AUDIO_FORMAT_U8P, SPA_AUDIO_FORMAT_F32, 0, conv_u8d_to_f32 },


	{ SPA_AUDIO_FORMAT_S16, SPA_AUDIO_FORMAT_F32, 0, conv_s16_to_f32 },
	{ SPA_AUDIO_FORMAT_S16P, SPA_AUDIO_FORMAT_F32P, 0, conv_s16d_to_f32d },
#if defined (HAVE_SSE2)
	{ SPA_AUDIO_FORMAT_S16, SPA_AUDIO_FORMAT_F32P, FEATURE_SSE2, conv_s16_to_f32d_sse2 },
#endif
	{ SPA_AUDIO_FORMAT_S16, SPA_AUDIO_FORMAT_F32P, 0, conv_s16_to_f32d },
	{ SPA_AUDIO_FORMAT_S16P, SPA_AUDIO_FORMAT_F32, 0, conv_s16d_to_f32 },

	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_F32, 0, conv_copy32 },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_F32P, 0, conv_copy32d },
	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_F32P, 0, deinterleave_32 },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_F32, 0, interleave_32 },

	{ SPA_AUDIO_FORMAT_S32, SPA_AUDIO_FORMAT_F32, 0, conv_s32_to_f32 },
	{ SPA_AUDIO_FORMAT_S32P, SPA_AUDIO_FORMAT_F32P, 0, conv_s32d_to_f32d },
	{ SPA_AUDIO_FORMAT_S32, SPA_AUDIO_FORMAT_F32P, 0, conv_s32_to_f32d },
	{ SPA_AUDIO_FORMAT_S32P, SPA_AUDIO_FORMAT_F32, 0, conv_s32d_to_f32 },

	{ SPA_AUDIO_FORMAT_S24, SPA_AUDIO_FORMAT_F32, 0, conv_s24_to_f32 },
	{ SPA_AUDIO_FORMAT_S24P, SPA_AUDIO_FORMAT_F32P, 0, conv_s24d_to_f32d },
#if defined (HAVE_SSSE3)
//	{ SPA_AUDIO_FORMAT_S24, SPA_AUDIO_FORMAT_F32P, FEATURE_SSSE3, conv_s24_to_f32d_ssse3 },
#endif
#if defined (HAVE_SSE41)
	{ SPA_AUDIO_FORMAT_S24, SPA_AUDIO_FORMAT_F32P, FEATURE_SSE41, conv_s24_to_f32d_sse41 },
#endif
#if defined (HAVE_SSE2)
	{ SPA_AUDIO_FORMAT_S24, SPA_AUDIO_FORMAT_F32P, FEATURE_SSE2, conv_s24_to_f32d_sse2 },
#endif
	{ SPA_AUDIO_FORMAT_S24, SPA_AUDIO_FORMAT_F32P, 0, conv_s24_to_f32d },
	{ SPA_AUDIO_FORMAT_S24P, SPA_AUDIO_FORMAT_F32, 0, conv_s24d_to_f32 },

	{ SPA_AUDIO_FORMAT_S24_32, SPA_AUDIO_FORMAT_F32, 0, conv_s24_32_to_f32 },
	{ SPA_AUDIO_FORMAT_S24_32P, SPA_AUDIO_FORMAT_F32P, 0, conv_s24_32d_to_f32d },
	{ SPA_AUDIO_FORMAT_S24_32, SPA_AUDIO_FORMAT_F32P, 0, conv_s24_32_to_f32d },
	{ SPA_AUDIO_FORMAT_S24_32P, SPA_AUDIO_FORMAT_F32, 0, conv_s24_32d_to_f32 },

	/* from f32 */
	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_U8, 0, conv_f32_to_u8 },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_U8P, 0, conv_f32d_to_u8d },
	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_U8P, 0, conv_f32_to_u8d },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_U8, 0, conv_f32d_to_u8 },

	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S16, 0, conv_f32_to_s16 },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S16P, 0, conv_f32d_to_s16d },
	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S16P, 0, conv_f32_to_s16d },
#if defined (HAVE_SSE2)
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S16, FEATURE_SSE2, conv_f32d_to_s16_sse2 },
#endif
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S16, 0, conv_f32d_to_s16 },

	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S32, 0, conv_f32_to_s32 },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S32P, 0, conv_f32d_to_s32d },
	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S32P, 0, conv_f32_to_s32d },
#if defined (HAVE_SSE2)
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S32, FEATURE_SSE2, conv_f32d_to_s32_sse2 },
#endif
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S32, 0, conv_f32d_to_s32 },

	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S24, 0, conv_f32_to_s24 },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S24P, 0, conv_f32d_to_s24d },
	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S24P, 0, conv_f32_to_s24d },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S24, 0, conv_f32d_to_s24 },

	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S24_32, 0, conv_f32_to_s24_32 },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S24_32P, 0, conv_f32d_to_s24_32d },
	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S24_32P, 0, conv_f32_to_s24_32d },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S24_32, 0, conv_f32d_to_s24_32 },

	/* u8 */
	{ SPA_AUDIO_FORMAT_U8, SPA_AUDIO_FORMAT_U8, 0, conv_copy8 },
	{ SPA_AUDIO_FORMAT_U8P, SPA_AUDIO_FORMAT_U8P, 0, conv_copy8d },
	{ SPA_AUDIO_FORMAT_U8, SPA_AUDIO_FORMAT_U8P, 0, deinterleave_8 },
	{ SPA_AUDIO_FORMAT_U8P, SPA_AUDIO_FORMAT_U8, 0, interleave_8 },

	/* s16 */
	{ SPA_AUDIO_FORMAT_S16, SPA_AUDIO_FORMAT_S16, 0, conv_copy16 },
	{ SPA_AUDIO_FORMAT_S16P, SPA_AUDIO_FORMAT_S16P, 0, conv_copy16d },
	{ SPA_AUDIO_FORMAT_S16, SPA_AUDIO_FORMAT_S16P, 0, deinterleave_16 },
	{ SPA_AUDIO_FORMAT_S16P, SPA_AUDIO_FORMAT_S16, 0, interleave_16 },

	/* s32 */
	{ SPA_AUDIO_FORMAT_S32, SPA_AUDIO_FORMAT_S32, 0, conv_copy32 },
	{ SPA_AUDIO_FORMAT_S32P, SPA_AUDIO_FORMAT_S32P, 0, conv_copy32d },
	{ SPA_AUDIO_FORMAT_S32, SPA_AUDIO_FORMAT_S32P, 0, deinterleave_32 },
	{ SPA_AUDIO_FORMAT_S32P, SPA_AUDIO_FORMAT_S32, 0, interleave_32 },

	/* s24 */
	{ SPA_AUDIO_FORMAT_S24, SPA_AUDIO_FORMAT_S24, 0, conv_copy24 },
	{ SPA_AUDIO_FORMAT_S24P, SPA_AUDIO_FORMAT_S24P, 0, conv_copy24d },
	{ SPA_AUDIO_FORMAT_S24, SPA_AUDIO_FORMAT_S24P, 0, deinterleave_24 },
	{ SPA_AUDIO_FORMAT_S24P, SPA_AUDIO_FORMAT_S24, 0, interleave_24 },

	/* s24_32 */
	{ SPA_AUDIO_FORMAT_S24_32, SPA_AUDIO_FORMAT_S24_32, 0, conv_copy32 },
	{ SPA_AUDIO_FORMAT_S24_32P, SPA_AUDIO_FORMAT_S24_32P, 0, conv_copy32d },
	{ SPA_AUDIO_FORMAT_S24_32, SPA_AUDIO_FORMAT_S24_32P, 0, deinterleave_32 },
	{ SPA_AUDIO_FORMAT_S24_32P, SPA_AUDIO_FORMAT_S24_32, 0, interleave_32 },
};

static const struct conv_info *find_conv_info(uint32_t src_fmt, uint32_t dst_fmt, uint32_t features)
{
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(conv_table); i++) {
		if (conv_table[i].src_fmt == src_fmt &&
		    conv_table[i].dst_fmt == dst_fmt &&
		    (conv_table[i].features == 0 || (conv_table[i].features & features) != 0))
			return &conv_table[i];
	}
	return NULL;
}
