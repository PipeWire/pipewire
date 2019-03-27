/* Spa
 *
 * Copyright © 2019 Wim Taymans
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

#include <math.h>

#include <spa/utils/defs.h>

#define U8_MIN		0
#define U8_MAX		255
#define U8_SCALE	127.5f
#define U8_OFFS		128
#define U8_TO_F32(v)	((((uint8_t)(v)) * (1.0f / U8_OFFS)) - 1.0)
#define F32_TO_U8(v)	(uint8_t)((SPA_CLAMP(v, -1.0f, 1.0f) * U8_SCALE) + U8_OFFS)

#define S16_MIN		-32767
#define S16_MAX		32767
#define S16_MAX_F	32767.0f
#define S16_SCALE	32767.0f
#define S16_TO_F32(v)	(((int16_t)(v)) * (1.0f / S16_SCALE))
#define F32_TO_S16(v)	(int16_t)(SPA_CLAMP(v, -1.0f, 1.0f) * S16_SCALE)

#define S24_MIN		-8388607
#define S24_MAX		8388607
#define S24_MAX_F	8388607.0f
#define S24_SCALE	8388607.0f
#define S24_TO_F32(v)	(((int32_t)(v)) * (1.0f / S24_SCALE))
#define F32_TO_S24(v)	(int32_t)(SPA_CLAMP(v, -1.0f, 1.0f) * S24_SCALE)

#define S32_SCALE	2147483648.0f
#define S32_MIN		2147483520.0f

#define S32_TO_F32(v)	S24_TO_F32((v) >> 8)
#define F32_TO_S32(v)	(F32_TO_S24(v) << 8)

static inline int32_t read_s24(const void *src)
{
	const int8_t *s = src;
#if __BYTE_ORDER == __LITTLE_ENDIAN
	return (((int32_t)s[2] << 16) | ((uint32_t)(uint8_t)s[1] << 8) | (uint32_t)(uint8_t)s[0]);
#else
	return (((int32_t)s[0] << 16) | ((uint32_t)(uint8_t)s[1] << 8) | (uint32_t)(uint8_t)s[2]);
#endif
}

static inline void write_s24(void *dst, int32_t val)
{
	uint8_t *d = dst;
#if __BYTE_ORDER == __LITTLE_ENDIAN
	d[0] = (uint8_t) (val);
	d[1] = (uint8_t) (val >> 8);
	d[2] = (uint8_t) (val >> 16);
#else
	d[0] = (uint8_t) (val >> 16);
	d[1] = (uint8_t) (val >> 8);
	d[2] = (uint8_t) (val);
#endif
}

typedef void (*convert_func_t) (void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples);

#if defined(HAVE_SSE2)
void conv_s16_to_f32d_sse2(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
                uint32_t n_channels, uint32_t n_samples);
void conv_s24_to_f32d_sse2(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples);
void conv_f32d_to_s32_sse2(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples);
void conv_f32d_to_s16_sse2(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples);
#endif
#if defined(HAVE_SSSE3)
void conv_s24_to_f32d_ssse3(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples);
#endif
#if defined(HAVE_SSE41)
void conv_s24_to_f32d_sse41(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
                uint32_t n_channels, uint32_t n_samples);

#endif
