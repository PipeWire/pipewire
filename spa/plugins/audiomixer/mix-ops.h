/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

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


#define S8_MIN			-128
#define S8_MAX			127
#define S8_ACCUM(a,b)		((a) + (int16_t)(b))
#define S8_CLAMP(a)		(int8_t)(SPA_CLAMP((a), S8_MIN, S8_MAX))
#define U8_OFFS			128
#define U8_ACCUM(a,b)		((a) + ((int16_t)(b) - U8_OFFS))
#define U8_CLAMP(a)		(uint8_t)(SPA_CLAMP((a), S8_MIN, S8_MAX) + U8_OFFS)

#define S16_MIN			-32768
#define S16_MAX			32767
#define S16_ACCUM(a,b)		((a) + (int32_t)(b))
#define S16_CLAMP(a)		(int16_t)(SPA_CLAMP((a), S16_MIN, S16_MAX))
#define U16_OFFS		32768
#define U16_ACCUM(a,b)		((a) + ((int32_t)(b) - U16_OFFS))
#define U16_CLAMP(a)		(uint16_t)(SPA_CLAMP((a), S16_MIN, S16_MAX) + U16_OFFS)

#define S24_32_MIN		-8388608
#define S24_32_MAX		8388607
#define S24_32_ACCUM(a,b)	((a) + (int32_t)(b))
#define S24_32_CLAMP(a)		(int32_t)(SPA_CLAMP((a), S24_32_MIN, S24_32_MAX))
#define U24_32_OFFS		8388608
#define U24_32_ACCUM(a,b)	((a) + ((int32_t)(b) - U24_32_OFFS))
#define U24_32_CLAMP(a)		(uint32_t)(SPA_CLAMP((a), S24_32_MIN, S24_32_MAX) + U24_32_OFFS)

#define S24_ACCUM(a,b)		S24_32_ACCUM(a, s24_to_s32(b))
#define S24_CLAMP(a)		s32_to_s24(S24_32_CLAMP(a))
#define U24_ACCUM(a,b)		U24_32_ACCUM(a, u24_to_u32(b))
#define U24_CLAMP(a)		u32_to_u24(U24_32_CLAMP(a))

#define S32_MIN			-2147483648
#define S32_MAX			2147483647
#define S32_ACCUM(a,b)		((a) + (int64_t)(b))
#define S32_CLAMP(a)		(int32_t)(SPA_CLAMP((a), S32_MIN, S32_MAX))
#define U32_OFFS		2147483648
#define U32_ACCUM(a,b)		((a) + ((int64_t)(b) - U32_OFFS))
#define U32_CLAMP(a)		(uint32_t)(SPA_CLAMP((a), S32_MIN, S32_MAX) + U32_OFFS)

#define F32_ACCUM(a,b)		((a) + (b))
#define F32_CLAMP(a)		(a)
#define F64_ACCUM(a,b)		((a) + (b))
#define F64_CLAMP(a)		(a)

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
