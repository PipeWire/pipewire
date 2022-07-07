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

typedef struct {
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t v3;
	uint8_t v2;
	uint8_t v1;
#else
	uint8_t v1;
	uint8_t v2;
	uint8_t v3;
#endif
} __attribute__ ((packed)) uint24_t;

typedef struct {
#if __BYTE_ORDER == __LITTLE_ENDIAN
	uint8_t v3;
	uint8_t v2;
	int8_t v1;
#else
	int8_t v1;
	uint8_t v2;
	uint8_t v3;
#endif
} __attribute__ ((packed)) int24_t;

static inline uint32_t u24_to_u32(uint24_t src)
{
	return ((uint32_t)src.v1 << 16) | ((uint32_t)src.v2 << 8) | (uint32_t)src.v3;
}

#define U32_TO_U24(s) (uint24_t) { .v1 = (uint8_t)(((uint32_t)s) >> 16), \
	.v2 = (uint8_t)(((uint32_t)s) >> 8), .v3 = (uint8_t)((uint32_t)s) }

static inline uint24_t u32_to_u24(uint32_t src)
{
	return U32_TO_U24(src);
}

static inline int32_t s24_to_s32(int24_t src)
{
	return ((int32_t)src.v1 << 16) | ((uint32_t)src.v2 << 8) | (uint32_t)src.v3;
}

#define S32_TO_S24(s) (int24_t) { .v1 = (int8_t)(((int32_t)s) >> 16), \
	.v2 = (uint8_t)(((uint32_t)s) >> 8), .v3 = (uint8_t)((uint32_t)s) }

static inline int24_t s32_to_s24(int32_t src)
{
	return S32_TO_S24(src);
}


#define S8_MIN		-128
#define S8_MAX		127
#define S8_MIX(a,b)	(int8_t)(SPA_CLAMP((int16_t)(a) + (int16_t)(b), S8_MIN, S8_MAX))
#define U8_OFFS		128
#define U8_MIX(a,b)	(uint8_t)((int16_t)S8_MIX((int16_t)(a) - U8_OFFS, (int16_t)(b) - U8_OFFS) + U8_OFFS)

#define S16_MIN		-32768
#define S16_MAX		32767
#define S16_MIX(a,b)	(int16_t)(SPA_CLAMP((int32_t)(a) + (int32_t)(b), S16_MIN, S16_MAX))
#define U16_OFFS	32768
#define U16_MIX(a,b)	(uint16_t)((int32_t)S16_MIX((int32_t)(a) - U16_OFFS, (int32_t)(b) - U16_OFFS) + U16_OFFS)

#define S24_MIN		-8388608
#define S24_MAX		8388607
#define S24_MIX_32(a,b) (int32_t)(SPA_CLAMP((int32_t)(a) + (int32_t)(b), S24_MIN, S24_MAX))
#define S24_MIX(a,b)	s32_to_s24(S24_MIX_32(s24_to_s32(a), s24_to_s32(b)))
#define U24_OFFS	8388608
#define U24_MIX(a,b)	u32_to_u24(S24_MIX_32((int32_t)u24_to_u32(a) - U24_OFFS, (int32_t)u24_to_u32(b) - U24_OFFS) + U24_OFFS)

#define S32_MIN		-2147483648
#define S32_MAX		2147483647
#define S32_MIX(a,b)	(int32_t)(SPA_CLAMP((int64_t)(a) + (int64_t)(b), S32_MIN, S32_MAX))
#define U32_OFFS	2147483648
#define U32_MIX(a,b)	(uint32_t)((int64_t)S32_MIX((int64_t)(a) - U32_OFFS, (int64_t)(b) - U32_OFFS) + U32_OFFS)

#define S24_32_MIX(a,b)	(int32_t)(SPA_CLAMP((int32_t)(a) + (int32_t)(b), S24_MIN, S24_MAX))
#define U24_32_MIX(a,b)	(uint32_t)((int32_t)S24_32_MIX((int32_t)(a) - U24_OFFS, (int32_t)(b) - U24_OFFS) + U24_OFFS)

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
