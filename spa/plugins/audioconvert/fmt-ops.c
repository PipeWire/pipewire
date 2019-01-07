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

#define U8_MIN		0
#define U8_MAX		255
#define U8_SCALE	127
#define U8_OFFS		128

#define S16_MIN		-32767
#define S16_MAX		32767
#define S16_MAX_F	32767.0f
#define S16_SCALE	32767

#define S24_MIN		-8388607
#define S24_MAX		8388607
#define S24_MAX_F	8388607.0f
#define S24_SCALE	8388607

#define S32_MIN		-2147483647
#define S32_MAX		2147483647
#define S32_SCALE	2147483647


static inline int32_t read_s24(const void *src)
{
	const int8_t *s = src;
#if __BYTE_ORDER == __LITTLE_ENDIAN
	return (((int32_t)s[2] << 16) | ((uint32_t)(uint8_t)s[1] << 8) | (uint32_t)(uint8_t)s[0]);
#else
	return (((int32_t)s[0] << 16) | ((uint32_t)(uint8_t)s[1] << 8) | (uint32_t)(uint8_t)s[2]);
#endif
}

#define READ24(s) read_s24(s)

#if defined (__SSE2__)
#include "fmt-ops-sse2.c"
#endif

static void
conv_copy8(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int i;
	for (i = 0; i < n_src; i++)
		memcpy(dst[i], src[i], n_samples);
}

static void
conv_copy16(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int i;
	for (i = 0; i < n_src; i++)
		memcpy(dst[i], src[i], n_samples * sizeof(int16_t));
}

static void
conv_copy24(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int i;
	for (i = 0; i < n_src; i++)
		memcpy(dst[i], src[i], n_samples * 3);
}

static void
conv_copy32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int i;
	for (i = 0; i < n_src; i++)
		memcpy(dst[i], src[i], n_samples * sizeof(int32_t));
}

#define U8_TO_F32(v)	(((v) * (1.0f / U8_OFFS)) - 1.0)

static void
conv_u8_to_f32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int i, j;

	for (i = 0; i < n_src; i++) {
		const uint8_t *s = src[i];
		float *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = U8_TO_F32(s[j]);
	}
}

static void
conv_u8_to_f32d(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const uint8_t *s = src[0];
	float **d = (float **) dst;
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_dst; i++)
			d[i][j] = U8_TO_F32(*s++);
	}
}

static void
conv_u8d_to_f32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const uint8_t **s = (const uint8_t **) src;
	float *d = dst[0];
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_src; i++)
			*d++ = U8_TO_F32(s[i][j]);
	}
}

#define S16_TO_F32(v)	((v) * (1.0f / S16_SCALE))

static void
conv_s16_to_f32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int i, j;

	for (i = 0; i < n_src; i++) {
		const int16_t *s = src[i];
		float *d = dst[i];
		for (j = 0; j < n_samples; j++)
			d[j] = S16_TO_F32(s[j]);
	}
}

static void
conv_s16_to_f32d(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const int16_t *s = src[0];
	float **d = (float **) dst;
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_dst; i++)
			d[i][j] = S16_TO_F32(*s++);
	}
}

static void
conv_s16d_to_f32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const int16_t **s = (const int16_t **) src;
	float *d = dst[0];
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_src; i++)
			*d++ = S16_TO_F32(s[i][j]);
	}
}

#define S32_TO_F32(v)	((v) * (1.0f / S32_SCALE))

static void
conv_s32_to_f32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int i, j;

	for (i = 0; i < n_src; i++) {
		const int32_t *s = src[i];
		float *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = S32_TO_F32(s[j]);
	}
}

static void
conv_s32_to_f32d(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const int32_t *s = src[0];
	float **d = (float **) dst;
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_dst; i++)
			d[i][j] = S32_TO_F32(*s++);
	}
}

static void
conv_s32d_to_f32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const int32_t **s = (const int32_t **) src;
	float *d = dst[0];
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_src; i++)
			*d++ = S32_TO_F32(s[i][j]);
	}
}


#define S24_TO_F32(v)	(((int32_t)(v)) * (1.0f / S24_SCALE))

static void
conv_s24_to_f32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int i, j;

	for (i = 0; i < n_src; i++) {
		const int8_t *s = src[i];
		float *d = dst[i];

		for (j = 0; j < n_samples; j++) {
			d[j] = S24_TO_F32(READ24(s));
			s += 3;
		}
	}
}

static void
conv_s24_to_f32d(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const uint8_t *s = src[0];
	float **d = (float **) dst;
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_dst; i++) {
			d[i][j] = S24_TO_F32(READ24(s));
			s += 3;
		}
	}
}

static void
conv_s24d_to_f32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const uint8_t **s = (const uint8_t **) src;
	float *d = dst[0];
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_src; i++) {
			*d++ = S24_TO_F32(READ24(s[i]));
			s += 3;
		}
	}
}

static void
conv_s24_32_to_f32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int i, j;

	for (i = 0; i < n_src; i++) {
		const int32_t *s = src[i];
		float *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = S24_TO_F32(s[j]);
	}
}

static void
conv_s24_32_to_f32d(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const int32_t *s = src[0];
	float **d = (float **) dst;
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_dst; i++)
			d[i][j] = S24_TO_F32(*s++);
	}
}

static void
conv_s24_32d_to_f32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const int32_t **s = (const int32_t **) src;
	float *d = dst[0];
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_src; i++)
			*d++ = S24_TO_F32(s[i][j]);
	}
}

#define F32_TO_U8(v)	((SPA_CLAMP(v, -1.0f, 1.0f) * U8_SCALE) + U8_OFFS)

static void
conv_f32_to_u8(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int i, j;

	for (i = 0; i < n_src; i++) {
		const float *s = src[i];
		uint8_t *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = F32_TO_U8(s[j]);
	}
}

static void
conv_f32_to_u8d(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const float *s = src[0];
	uint8_t **d = (uint8_t **) dst;
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_dst; i++)
			d[i][j] = F32_TO_U8(*s++);
	}
}

static void
conv_f32d_to_u8(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const float **s = (const float **) src;
	uint8_t *d = dst[0];
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_src; i++)
			*d++ = F32_TO_U8(s[i][j]);
	}
}

#define F32_TO_S16(v)	(SPA_CLAMP(v, -1.0f, 1.0f) * S16_SCALE)

static void
conv_f32_to_s16(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int i, j;

	for (i = 0; i < n_src; i++) {
		const float *s = src[i];
		int16_t *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = F32_TO_S16(s[j]);
	}
}

static void
conv_f32_to_s16d(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const float *s = src[0];
	int16_t **d = (int16_t **) dst;
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_dst; i++)
			d[i][j] = F32_TO_S16(*s++);
	}
}

static void
conv_f32d_to_s16(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const float **s = (const float **) src;
	int16_t *d = dst[0];
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_src; i++)
			*d++ = F32_TO_S16(s[i][j]);
	}
}

#define F32_TO_S32(v)		(SPA_CLAMP(v, -1.0f, 1.0f) * S32_SCALE)

static void
conv_f32_to_s32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int i, j;

	for (i = 0; i < n_src; i++) {
		const float *s = src[i];
		int32_t *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = F32_TO_S32(s[j]);
	}
}

static void
conv_f32_to_s32d(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const float *s = src[0];
	int32_t **d = (int32_t **) dst;
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_dst; i++)
			d[i][j] = F32_TO_S32(*s++);
	}
}

static void
conv_f32d_to_s32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const float **s = (const float **) src;
	int32_t *d = dst[0];
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_src; i++)
			*d++ = F32_TO_S32(s[i][j]);
	}
}


#define F32_TO_S24(v)	 (SPA_CLAMP(v, -1.0f, 1.0f) * S24_SCALE)

#define WRITE24(d,v)			\
({					\
	int32_t _v = (v);		\
	d[0] = (uint8_t) (_v >> 16);	\
	d[1] = (uint8_t) (_v >> 8);	\
	d[2] = (uint8_t) _v;		\
})

static void
conv_f32_to_s24(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int i, j;

	for (i = 0; i < n_src; i++) {
		const float *s = src[i];
		uint8_t *d = dst[i];

		for (j = 0; j < n_samples; j++)
			WRITE24(d, F32_TO_S24(s[j]));
			d += 3;
	}
}

static void
conv_f32_to_s24d(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const float *s = src[0];
	uint8_t **d = (uint8_t **) dst;
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_dst; i++) {
			WRITE24(d[i], F32_TO_S24(*s++));
			d[i] += 3;
		}
	}
}

static void
conv_f32d_to_s24(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const float **s = (const float **) src;
	uint8_t *d = dst[0];
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_src; i++) {
			WRITE24(d, F32_TO_S24(s[i][j]));
			d += 3;
		}
	}
}


static void
conv_f32_to_s24_32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	int i, j;

	for (i = 0; i < n_src; i++) {
		const float *s = src[i];
		int32_t *d = dst[i];

		for (j = 0; j < n_samples; j++)
			d[j] = F32_TO_S24(s[j]);
	}
}

static void
conv_f32_to_s24_32d(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const float *s = src[0];
	int32_t **d = (int32_t **) dst;
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_dst; i++)
			d[i][j] = F32_TO_S24(*s++);
	}
}

static void
conv_f32d_to_s24_32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const float **s = (const float **) src;
	int32_t *d = dst[0];
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_src; i++)
			*d++ = F32_TO_S24(s[i][j]);
	}
}

static void
deinterleave_8(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const uint8_t *s = src[0];
	uint8_t **d = (uint8_t **) dst;
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_dst; i++)
			d[i][j] = *s++;
	}
}

static void
deinterleave_16(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const uint16_t *s = src[0];
	uint16_t **d = (uint16_t **) dst;
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_dst; i++)
			d[i][j] = *s++;
	}
}

static void
deinterleave_24(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const uint8_t *s = src[0];
	uint8_t **d = (uint8_t **) dst;
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_dst; i++) {
			WRITE24(d[i], READ24(s));
			d += 3;
			s += 3;
		}
	}
}

static void
deinterleave_32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const uint32_t *s = src[0];
	uint32_t **d = (uint32_t **) dst;
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_dst; i++)
			d[i][j] = *s++;
	}
}

static void
interleave_8(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const int8_t **s = (const int8_t **) src;
	uint8_t *d = dst[0];
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_src; i++)
			*d++ = s[i][j];
	}
}

static void
interleave_16(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const int16_t **s = (const int16_t **) src;
	uint16_t *d = dst[0];
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_src; i++)
			*d++ = s[i][j];
	}
}

static void
interleave_24(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const int8_t **s = (const int8_t **) src;
	uint8_t *d = dst[0];
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_src; i++) {
			WRITE24(d, READ24(s[i]));
			d += 3;
			s += 3;
		}
	}
}

static void
interleave_32(void *data, int n_dst, void *dst[n_dst], int n_src, const void *src[n_src], int n_samples)
{
	const int32_t **s = (const int32_t **) src;
	uint32_t *d = dst[0];
	int i, j;

	for (j = 0; j < n_samples; j++) {
		for (i = 0; i < n_src; i++)
			*d++ = s[i][j];
	}
}


typedef void (*convert_func_t) (void *data, int n_dst, void *dst[n_dst],
				int n_src, const void *src[n_src], int n_samples);

static const struct conv_info {
	uint32_t src_fmt;
	uint32_t dst_fmt;
#define FEATURE_SSE2	SPA_CPU_FLAG_SSE2
	uint32_t features;

	convert_func_t func;
} conv_table[] =
{
	/* to f32 */
	{ SPA_AUDIO_FORMAT_U8, SPA_AUDIO_FORMAT_F32, 0, conv_u8_to_f32 },
	{ SPA_AUDIO_FORMAT_U8P, SPA_AUDIO_FORMAT_F32P, 0, conv_u8_to_f32 },
	{ SPA_AUDIO_FORMAT_U8, SPA_AUDIO_FORMAT_F32P, 0, conv_u8_to_f32d },
	{ SPA_AUDIO_FORMAT_U8P, SPA_AUDIO_FORMAT_F32, 0, conv_u8d_to_f32 },


	{ SPA_AUDIO_FORMAT_S16, SPA_AUDIO_FORMAT_F32, 0, conv_s16_to_f32 },
	{ SPA_AUDIO_FORMAT_S16P, SPA_AUDIO_FORMAT_F32P, 0, conv_s16_to_f32 },
#if defined (__SSE2__)
	{ SPA_AUDIO_FORMAT_S16, SPA_AUDIO_FORMAT_F32P, FEATURE_SSE2, conv_s16_to_f32d_sse2 },
#endif
	{ SPA_AUDIO_FORMAT_S16, SPA_AUDIO_FORMAT_F32P, 0, conv_s16_to_f32d },
	{ SPA_AUDIO_FORMAT_S16P, SPA_AUDIO_FORMAT_F32, 0, conv_s16d_to_f32 },

	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_F32, 0, conv_copy32 },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_F32P, 0, conv_copy32 },
	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_F32P, 0, deinterleave_32 },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_F32, 0, interleave_32 },

	{ SPA_AUDIO_FORMAT_S32, SPA_AUDIO_FORMAT_F32, 0, conv_s32_to_f32 },
	{ SPA_AUDIO_FORMAT_S32P, SPA_AUDIO_FORMAT_F32P, 0, conv_s32_to_f32 },
	{ SPA_AUDIO_FORMAT_S32, SPA_AUDIO_FORMAT_F32P, 0, conv_s32_to_f32d },
	{ SPA_AUDIO_FORMAT_S32P, SPA_AUDIO_FORMAT_F32, 0, conv_s32d_to_f32 },

	{ SPA_AUDIO_FORMAT_S24, SPA_AUDIO_FORMAT_F32, 0, conv_s24_to_f32 },
	{ SPA_AUDIO_FORMAT_S24P, SPA_AUDIO_FORMAT_F32P, 0, conv_s24_to_f32 },
#if defined (__SSE2__)
	{ SPA_AUDIO_FORMAT_S24, SPA_AUDIO_FORMAT_F32P, FEATURE_SSE2, conv_s24_to_f32d_sse2 },
#endif
	{ SPA_AUDIO_FORMAT_S24, SPA_AUDIO_FORMAT_F32P, 0, conv_s24_to_f32d },
	{ SPA_AUDIO_FORMAT_S24P, SPA_AUDIO_FORMAT_F32, 0, conv_s24d_to_f32 },

	{ SPA_AUDIO_FORMAT_S24_32, SPA_AUDIO_FORMAT_F32, 0, conv_s24_32_to_f32 },
	{ SPA_AUDIO_FORMAT_S24_32P, SPA_AUDIO_FORMAT_F32P, 0, conv_s24_32_to_f32 },
	{ SPA_AUDIO_FORMAT_S24_32, SPA_AUDIO_FORMAT_F32P, 0, conv_s24_32_to_f32d },
	{ SPA_AUDIO_FORMAT_S24_32P, SPA_AUDIO_FORMAT_F32, 0, conv_s24_32d_to_f32 },

	/* from f32 */
	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_U8, 0, conv_f32_to_u8 },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_U8P, 0, conv_f32_to_u8 },
	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_U8P, 0, conv_f32_to_u8d },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_U8, 0, conv_f32d_to_u8 },

	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S16, 0, conv_f32_to_s16 },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S16P, 0, conv_f32_to_s16 },
	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S16P, 0, conv_f32_to_s16d },
#if defined (__SSE2__)
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S16, FEATURE_SSE2, conv_f32d_to_s16_sse2 },
#endif
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S16, 0, conv_f32d_to_s16 },

	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S32, 0, conv_f32_to_s32 },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S32P, 0, conv_f32_to_s32 },
	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S32P, 0, conv_f32_to_s32d },
#if defined (__SSE2__)
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S32, FEATURE_SSE2, conv_f32d_to_s32_sse2 },
#endif
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S32, 0, conv_f32d_to_s32 },

	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S24, 0, conv_f32_to_s24 },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S24P, 0, conv_f32_to_s24 },
	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S24P, 0, conv_f32_to_s24d },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S24, 0, conv_f32d_to_s24 },

	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S24_32, 0, conv_f32_to_s24_32 },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S24_32P, 0, conv_f32_to_s24_32 },
	{ SPA_AUDIO_FORMAT_F32, SPA_AUDIO_FORMAT_S24_32P, 0, conv_f32_to_s24_32d },
	{ SPA_AUDIO_FORMAT_F32P, SPA_AUDIO_FORMAT_S24_32, 0, conv_f32d_to_s24_32 },

	/* u8 */
	{ SPA_AUDIO_FORMAT_U8, SPA_AUDIO_FORMAT_U8, 0, conv_copy8 },
	{ SPA_AUDIO_FORMAT_U8P, SPA_AUDIO_FORMAT_U8P, 0, conv_copy8 },
	{ SPA_AUDIO_FORMAT_U8, SPA_AUDIO_FORMAT_U8P, 0, deinterleave_8 },
	{ SPA_AUDIO_FORMAT_U8P, SPA_AUDIO_FORMAT_U8, 0, interleave_8 },

	/* s16 */
	{ SPA_AUDIO_FORMAT_S16, SPA_AUDIO_FORMAT_S16, 0, conv_copy16 },
	{ SPA_AUDIO_FORMAT_S16P, SPA_AUDIO_FORMAT_S16P, 0, conv_copy16 },
	{ SPA_AUDIO_FORMAT_S16, SPA_AUDIO_FORMAT_S16P, 0, deinterleave_16 },
	{ SPA_AUDIO_FORMAT_S16P, SPA_AUDIO_FORMAT_S16, 0, interleave_16 },

	/* s32 */
	{ SPA_AUDIO_FORMAT_S32, SPA_AUDIO_FORMAT_S32, 0, conv_copy32 },
	{ SPA_AUDIO_FORMAT_S32P, SPA_AUDIO_FORMAT_S32P, 0, conv_copy32 },
	{ SPA_AUDIO_FORMAT_S32, SPA_AUDIO_FORMAT_S32P, 0, deinterleave_32 },
	{ SPA_AUDIO_FORMAT_S32P, SPA_AUDIO_FORMAT_S32, 0, interleave_32 },

	/* s24 */
	{ SPA_AUDIO_FORMAT_S24, SPA_AUDIO_FORMAT_S24, 0, conv_copy24 },
	{ SPA_AUDIO_FORMAT_S24P, SPA_AUDIO_FORMAT_S24P, 0, conv_copy24 },
	{ SPA_AUDIO_FORMAT_S24, SPA_AUDIO_FORMAT_S24P, 0, deinterleave_24 },
	{ SPA_AUDIO_FORMAT_S24P, SPA_AUDIO_FORMAT_S24, 0, interleave_24 },

	/* s24_32 */
	{ SPA_AUDIO_FORMAT_S24_32, SPA_AUDIO_FORMAT_S24_32, 0, conv_copy32 },
	{ SPA_AUDIO_FORMAT_S24_32P, SPA_AUDIO_FORMAT_S24_32P, 0, conv_copy32 },
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
