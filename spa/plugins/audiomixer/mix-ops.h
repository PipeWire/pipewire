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

#include <spa/utils/defs.h>

static inline uint32_t read_u24(const void *src)
{
	const uint8_t *s = src;
#if __BYTE_ORDER == __LITTLE_ENDIAN
	return (((uint32_t)s[2] << 16) | ((uint32_t)(uint8_t)s[1] << 8) | (uint32_t)(uint8_t)s[0]);
#else
	return (((uint32_t)s[0] << 16) | ((uint32_t)(uint8_t)s[1] << 8) | (uint32_t)(uint8_t)s[2]);
#endif
}

static inline int32_t read_s24(const void *src)
{
	const int8_t *s = src;
#if __BYTE_ORDER == __LITTLE_ENDIAN
	return (((int32_t)s[2] << 16) | ((uint32_t)(uint8_t)s[1] << 8) | (uint32_t)(uint8_t)s[0]);
#else
	return (((int32_t)s[0] << 16) | ((uint32_t)(uint8_t)s[1] << 8) | (uint32_t)(uint8_t)s[2]);
#endif
}

static inline void write_u24(void *dst, uint32_t val)
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

#define S8_MIN		-127
#define S8_MAX		127
#define S8_MIX(a, b)    (int8_t)(SPA_CLAMP((int16_t)(a) + (int16_t)(b), S8_MIN, S8_MAX))
#define U8_MIX(a, b)    (uint8_t)((int16_t)S8_MIX((int16_t)(a) - S8_MAX, (int16_t)(b) - S8_MAX) + S8_MAX)

#define S16_MIN		-32767
#define S16_MAX		32767
#define S16_MIX(a, b)   (int16_t)(SPA_CLAMP((int32_t)(a) + (int32_t)(b), S16_MIN, S16_MAX))
#define U16_MIX(a, b)   (uint16_t)((int32_t)S16_MIX((int32_t)(a) - S16_MAX, (int32_t)(b) - S16_MAX) + S16_MAX)

#define S24_MIN		-8388607
#define S24_MAX		8388607
#define S24_MIX(a, b)   (int32_t)(SPA_CLAMP((int32_t)(a) + (int32_t)(b), S24_MIN, S24_MAX))
#define U24_MIX(a, b)   (uint32_t)((int32_t)S24_MIX((int32_t)(a) - S24_MAX, (int32_t)(b) - S24_MAX) + S24_MAX)

#define S32_MIN		-2147483647
#define S32_MAX		2147483647
#define S32_MIX(a, b)   (int32_t)(SPA_CLAMP((int64_t)(a) + (int64_t)(b), S32_MIN, S32_MAX))
#define U32_MIX(a, b)   (uint32_t)((int64_t)S32_MIX((int64_t)(a) - S32_MAX, (int64_t)(b) - S32_MAX) + S32_MAX)

#define S24_32_MIX(a, b) S24_MIX (a, b)
#define U24_32_MIX(a, b) U24_MIX (a, b)

#define F32_MIX(a, b)   (float)((float)(a) + (float)(b))

#define F64_MIX(a, b)   (double)((double)(a) + (double)(b))

struct mix_ops {
	uint32_t fmt;
	uint32_t n_channels;
	uint32_t cpu_flags;

	void (*clear) (struct mix_ops *ops, void * SPA_RESTRICT dst, uint32_t n_samples);
	void (*process) (struct mix_ops *ops,
			void * SPA_RESTRICT dst,
			const void * SPA_RESTRICT src[], uint32_t n_src,
			uint32_t n_samples);
	void (*free) (struct mix_ops *ops);

	const void *priv;
};

int mix_ops_init(struct mix_ops *ops);

#define mix_ops_clear(ops,...)		(ops)->clear(ops, __VA_ARGS__)
#define mix_ops_process(ops,...)	(ops)->process(ops, __VA_ARGS__)
#define mix_ops_free(ops)		(ops)->free(ops)

#define DEFINE_FUNCTION(name,arch) \
void mix_##name##_##arch(struct mix_ops *ops, void * SPA_RESTRICT dst,	\
		const void * SPA_RESTRICT src[], uint32_t n_src,		\
		uint32_t n_samples)						\

#define MIX_OPS_MAX_ALIGN	32

DEFINE_FUNCTION(s8, c);
DEFINE_FUNCTION(u8, c);
DEFINE_FUNCTION(s16, c);
DEFINE_FUNCTION(u16, c);
DEFINE_FUNCTION(s24, c);
DEFINE_FUNCTION(u24, c);
DEFINE_FUNCTION(s32, c);
DEFINE_FUNCTION(u32, c);
DEFINE_FUNCTION(s24_32, c);
DEFINE_FUNCTION(u24_32, c);
DEFINE_FUNCTION(f32, c);
DEFINE_FUNCTION(f64, c);

#if defined(HAVE_SSE)
DEFINE_FUNCTION(f32, sse);
#endif
#if defined(HAVE_SSE2)
DEFINE_FUNCTION(f64, sse2);
#endif
#if defined(HAVE_AVX)
DEFINE_FUNCTION(f32, avx);
#endif
