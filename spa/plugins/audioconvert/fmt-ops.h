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
#if defined(__FreeBSD__) || defined(__MidnightBSD__)
#include <sys/endian.h>
#define bswap_16 bswap16
#define bswap_32 bswap32
#define bswap_64 bswap64
#else
#include <byteswap.h>
#endif

#include <spa/utils/defs.h>
#include <spa/utils/string.h>

#define FMT_OPS_MAX_ALIGN	32

#define U8_MIN			0u
#define U8_MAX			255u
#define U8_SCALE		128.f
#define U8_OFFS			128.f
#define U8_TO_F32(v)		((((uint8_t)(v)) * (1.0f / U8_SCALE)) - 1.0f)
#define F32_TO_U8(v)		(uint8_t)SPA_CLAMP((v) * U8_SCALE + U8_OFFS, U8_MIN, U8_MAX)
#define F32_TO_U8_D(v,d)	(uint8_t)SPA_CLAMP((v) * U8_SCALE + U8_OFFS + (d), U8_MIN, U8_MAX)

#define S8_MIN			-128
#define S8_MAX			127
#define S8_SCALE		128.0f
#define S8_TO_F32(v)		(((int8_t)(v)) * (1.0f / S8_SCALE))
#define F32_TO_S8(v)		(int8_t)SPA_CLAMP((v) * S8_SCALE, S8_MIN, S8_MAX)
#define F32_TO_S8_D(v,d)	(int8_t)SPA_CLAMP((v) * S8_SCALE + (d), S8_MIN, S8_MAX)

#define U16_MIN			0u
#define U16_MAX			65535u
#define U16_SCALE		32768.f
#define U16_OFFS		32768.f
#define U16_TO_F32(v)		((((uint16_t)(v)) * (1.0f / U16_SCALE)) - 1.0f)
#define U16S_TO_F32(v)		(((uint16_t)bswap_16((uint16_t)(v)) * (1.0f / U16_OFFS)) - 1.0f)
#define F32_TO_U16(v)		(uint16_t)SPA_CLAMP((v) * U16_SCALE + U16_OFFS, U16_MIN, U16_MAX)
#define F32_TO_U16_D(v,d)	(uint16_t)SPA_CLAMP((v) * U16_SCALE + U16_OFFS + (d), U16_MIN, U16_MAX)
#define F32_TO_U16S(v)		bswap_16(F32_TO_U16(v))
#define F32_TO_U16S_D(v,d)	bswap_16(F32_TO_U16_D(v,d))

#define S16_MIN			-32768
#define S16_MAX			32767
#define S16_SCALE		32768.0f
#define S16_TO_F32(v)		(((int16_t)(v)) * (1.0f / S16_SCALE))
#define S16S_TO_F32(v)		(((int16_t)bswap_16(v)) * (1.0f / S16_SCALE))
#define F32_TO_S16(v)		(int16_t)SPA_CLAMP((v) * S16_SCALE, S16_MIN, S16_MAX)
#define F32_TO_S16_D(v,d)	(int16_t)SPA_CLAMP((v) * S16_SCALE + (d), S16_MIN, S16_MAX)
#define F32_TO_S16S(v)		bswap_16(F32_TO_S16(v))
#define F32_TO_S16S_D(v,d)	bswap_16(F32_TO_S16_D(v,d))

#define U24_MIN			0u
#define U24_MAX			16777215u
#define U24_SCALE		8388608.f
#define U24_OFFS		8388608.f
#define U24_TO_F32(v)		((u24_to_u32(v) * (1.0f / U24_SCALE)) - 1.0f)
#define F32_TO_U24(v)		u32_to_u24(SPA_CLAMP((v) * U24_SCALE + U24_OFFS, U24_MIN, U24_MAX))
#define F32_TO_U24_D(v,d)	u32_to_u24(SPA_CLAMP((v) * U24_SCALE + U24_OFFS + (d), U24_MIN, U24_MAX))

#define S24_MIN			-8388608
#define S24_MAX			8388607
#define S24_SCALE		8388608.0f
#define S24_TO_F32(v)		(s24_to_s32(v) * (1.0f / S24_SCALE))
#define S24S_TO_F32(v)		(s24_to_s32(bswap_s24(v)) * (1.0f / S24_SCALE))
#define F32_TO_S24(v)		s32_to_s24(SPA_CLAMP((v) * S24_SCALE, S24_MIN, S24_MAX))
#define F32_TO_S24S(v)		bswap_s24(F32_TO_S24(v))
#define F32_TO_S24_D(v,d)	s32_to_s24(SPA_CLAMP((v) * S24_SCALE + (d), S24_MIN, S24_MAX))

#define U32_MIN			0u
#define U32_MAX			4294967295
#define U32_SCALE		2147483648.f
#define U32_OFFS		2147483648.f
#define U32_TO_F32(v)		((((uint32_t)(v)) * (1.0f / U32_SCALE)) - 1.0f)
#define F32_TO_U32(v)		(uint32_t)SPA_CLAMP((v) * U32_SCALE + U32_OFFS, U32_MIN, U32_MAX)
#define F32_TO_U32_D(v,d)	(uint32_t)SPA_CLAMP((v) * U32_SCALE + U32_OFFS + (d), U32_MIN, U32_MAX)

#define S32_MIN			-2147483648
#define S32_MAX			2147483520
#define S32_SCALE		2147483648.f
#define S32_TO_F32(v)		(((int32_t)(v)) * (1.0f / S32_SCALE))
#define S32S_TO_F32(v)		(((int32_t)bswap_32(v)) * (1.0f / S32_SCALE))
#define F32_TO_S32(v)		(int32_t)SPA_CLAMP((v) * S32_SCALE, S32_MIN, S32_MAX)
#define F32_TO_S32_D(v,d)	(int32_t)SPA_CLAMP((v) * S32_SCALE + (d), S32_MIN, S32_MAX)
#define F32_TO_S32S(v)		bswap_32(F32_TO_S32(v))
#define F32_TO_S32S_D(v,d)	bswap_32(F32_TO_S32_D(v,d))

#define U24_32_TO_F32(v)	U32_TO_F32((v)<<8)
#define U24_32S_TO_F32(v)	U32_TO_F32(((int32_t)bswap_32(v))<<8)
#define F32_TO_U24_32(v)	(uint32_t)SPA_CLAMP((v) * U24_SCALE + U24_OFFS, U24_MIN, U24_MAX)
#define F32_TO_U24_32S(v)	bswap_32(F32_TO_U24_32(v))
#define F32_TO_U24_32_D(v,d)	(uint32_t)SPA_CLAMP((v) * U24_SCALE + U24_OFFS + (d), U24_MIN, U24_MAX)
#define F32_TO_U24_32S_D(v,d)	bswap_32(F32_TO_U24_32_D(v,d))

#define S24_32_TO_F32(v)	S32_TO_F32((v)<<8)
#define S24_32S_TO_F32(v)	S32_TO_F32(((int32_t)bswap_32(v))<<8)
#define F32_TO_S24_32(v)	(int32_t)SPA_CLAMP((v) * S24_SCALE, S24_MIN, S24_MAX)
#define F32_TO_S24_32S(v)	bswap_32(F32_TO_S24_32(v))
#define F32_TO_S24_32_D(v,d)	(int32_t)SPA_CLAMP((v) * S24_SCALE + (d), S24_MIN, S24_MAX)
#define F32_TO_S24_32S_D(v,d)	bswap_32(F32_TO_S24_32_D(v,d))

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

static inline uint24_t bswap_u24(uint24_t src)
{
	return (uint24_t) { .v1 = src.v3, .v2 = src.v2, .v3 = src.v1 };
}
static inline int24_t bswap_s24(int24_t src)
{
	return (int24_t) { .v1 = src.v3, .v2 = src.v2, .v3 = src.v1 };
}

#define NS_MAX	8
#define NS_MASK	(NS_MAX-1)

struct shaper {
	float e[NS_MAX];
	uint32_t idx;
	float r;
};

struct convert {
	uint32_t noise;
#define DITHER_METHOD_NONE		0
#define DITHER_METHOD_RECTANGULAR	1
#define DITHER_METHOD_TRIANGULAR	2
#define DITHER_METHOD_SHAPED_5		3
	uint32_t method;

	uint32_t src_fmt;
	uint32_t dst_fmt;
	uint32_t n_channels;
	uint32_t rate;
	uint32_t cpu_flags;
	const char *func_name;

	unsigned int is_passthrough:1;

	float scale;
	uint32_t random[16 + FMT_OPS_MAX_ALIGN/4];
	float *dither;
	uint32_t dither_size;
	struct shaper shaper[64];

	void (*process) (struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
			uint32_t n_samples);
	void (*free) (struct convert *conv);
};

int convert_init(struct convert *conv);

static const struct dither_method_info {
	const char *label;
	const char *description;
	uint32_t method;
} dither_method_info[] = {
	[DITHER_METHOD_NONE] = { "none", "Disabled", DITHER_METHOD_NONE },
	[DITHER_METHOD_RECTANGULAR] = { "rectangular", "Rectangular dithering", DITHER_METHOD_RECTANGULAR },
	[DITHER_METHOD_TRIANGULAR] = { "triangular", "Triangular dithering", DITHER_METHOD_TRIANGULAR },
	[DITHER_METHOD_SHAPED_5] = { "shaped5", "Shaped 5 dithering", DITHER_METHOD_SHAPED_5 }
};

static inline uint32_t dither_method_from_label(const char *label)
{
	uint32_t i;
	for (i = 0; i < SPA_N_ELEMENTS(dither_method_info); i++) {
		if (spa_streq(dither_method_info[i].label, label))
			return dither_method_info[i].method;
	}
	return DITHER_METHOD_NONE;
}

#define convert_process(conv,...)	(conv)->process(conv, __VA_ARGS__)
#define convert_free(conv)		(conv)->free(conv)

#define DEFINE_FUNCTION(name,arch) \
void conv_##name##_##arch(struct convert *conv, void * SPA_RESTRICT dst[],	\
		const void * SPA_RESTRICT src[], uint32_t n_samples)		\

DEFINE_FUNCTION(copy8d, c);
DEFINE_FUNCTION(copy8, c);
DEFINE_FUNCTION(copy16d, c);
DEFINE_FUNCTION(copy16, c);
DEFINE_FUNCTION(copy24d, c);
DEFINE_FUNCTION(copy24, c);
DEFINE_FUNCTION(copy32d, c);
DEFINE_FUNCTION(copy32, c);
DEFINE_FUNCTION(copy64d, c);
DEFINE_FUNCTION(copy64, c);
DEFINE_FUNCTION(u8d_to_f32d, c);
DEFINE_FUNCTION(u8_to_f32, c);
DEFINE_FUNCTION(u8_to_f32d, c);
DEFINE_FUNCTION(u8d_to_f32, c);
DEFINE_FUNCTION(s8d_to_f32d, c);
DEFINE_FUNCTION(s8_to_f32, c);
DEFINE_FUNCTION(s8_to_f32d, c);
DEFINE_FUNCTION(s8d_to_f32, c);
DEFINE_FUNCTION(ulaw_to_f32d, c);
DEFINE_FUNCTION(alaw_to_f32d, c);
DEFINE_FUNCTION(u16_to_f32, c);
DEFINE_FUNCTION(u16_to_f32d, c);
DEFINE_FUNCTION(s16d_to_f32d, c);
DEFINE_FUNCTION(s16_to_f32, c);
DEFINE_FUNCTION(s16_to_f32d, c);
DEFINE_FUNCTION(s16s_to_f32d, c);
DEFINE_FUNCTION(s16d_to_f32, c);
DEFINE_FUNCTION(u32_to_f32, c);
DEFINE_FUNCTION(u32_to_f32d, c);
DEFINE_FUNCTION(s32d_to_f32d, c);
DEFINE_FUNCTION(s32_to_f32, c);
DEFINE_FUNCTION(s32_to_f32d, c);
DEFINE_FUNCTION(s32s_to_f32d, c);
DEFINE_FUNCTION(s32d_to_f32, c);
DEFINE_FUNCTION(u24_to_f32, c);
DEFINE_FUNCTION(u24_to_f32d, c);
DEFINE_FUNCTION(s24d_to_f32d, c);
DEFINE_FUNCTION(s24_to_f32, c);
DEFINE_FUNCTION(s24_to_f32d, c);
DEFINE_FUNCTION(s24s_to_f32d, c);
DEFINE_FUNCTION(s24d_to_f32, c);
DEFINE_FUNCTION(u24_32_to_f32, c);
DEFINE_FUNCTION(u24_32_to_f32d, c);
DEFINE_FUNCTION(s24_32d_to_f32d, c);
DEFINE_FUNCTION(s24_32_to_f32, c);
DEFINE_FUNCTION(s24_32_to_f32d, c);
DEFINE_FUNCTION(s24_32s_to_f32d, c);
DEFINE_FUNCTION(s24_32d_to_f32, c);
DEFINE_FUNCTION(f64d_to_f32d, c);
DEFINE_FUNCTION(f64_to_f32, c);
DEFINE_FUNCTION(f64_to_f32d, c);
DEFINE_FUNCTION(f64s_to_f32d, c);
DEFINE_FUNCTION(f64d_to_f32, c);
DEFINE_FUNCTION(f32d_to_u8d, c);
DEFINE_FUNCTION(f32d_to_u8d_dither, c);
DEFINE_FUNCTION(f32d_to_u8d_shaped, c);
DEFINE_FUNCTION(f32_to_u8, c);
DEFINE_FUNCTION(f32_to_u8d, c);
DEFINE_FUNCTION(f32d_to_u8, c);
DEFINE_FUNCTION(f32d_to_u8_dither, c);
DEFINE_FUNCTION(f32d_to_u8_shaped, c);
DEFINE_FUNCTION(f32d_to_s8d, c);
DEFINE_FUNCTION(f32d_to_s8d_dither, c);
DEFINE_FUNCTION(f32d_to_s8d_shaped, c);
DEFINE_FUNCTION(f32_to_s8, c);
DEFINE_FUNCTION(f32_to_s8d, c);
DEFINE_FUNCTION(f32d_to_s8, c);
DEFINE_FUNCTION(f32d_to_s8_dither, c);
DEFINE_FUNCTION(f32d_to_s8_shaped, c);
DEFINE_FUNCTION(f32d_to_alaw, c);
DEFINE_FUNCTION(f32d_to_ulaw, c);
DEFINE_FUNCTION(f32_to_u16, c);
DEFINE_FUNCTION(f32d_to_u16, c);
DEFINE_FUNCTION(f32d_to_s16d, c);
DEFINE_FUNCTION(f32d_to_s16d_dither, c);
DEFINE_FUNCTION(f32d_to_s16d_shaped, c);
DEFINE_FUNCTION(f32_to_s16, c);
DEFINE_FUNCTION(f32_to_s16d, c);
DEFINE_FUNCTION(f32d_to_s16, c);
DEFINE_FUNCTION(f32d_to_s16_dither, c);
DEFINE_FUNCTION(f32d_to_s16_shaped, c);
DEFINE_FUNCTION(f32d_to_s16s, c);
DEFINE_FUNCTION(f32d_to_s16s_dither, c);
DEFINE_FUNCTION(f32d_to_s16s_shaped, c);
DEFINE_FUNCTION(f32_to_u32, c);
DEFINE_FUNCTION(f32d_to_u32, c);
DEFINE_FUNCTION(f32d_to_s32d, c);
DEFINE_FUNCTION(f32d_to_s32d_dither, c);
DEFINE_FUNCTION(f32_to_s32, c);
DEFINE_FUNCTION(f32_to_s32d, c);
DEFINE_FUNCTION(f32d_to_s32, c);
DEFINE_FUNCTION(f32d_to_s32_dither, c);
DEFINE_FUNCTION(f32d_to_s32s, c);
DEFINE_FUNCTION(f32d_to_s32s_dither, c);
DEFINE_FUNCTION(f32_to_u24, c);
DEFINE_FUNCTION(f32d_to_u24, c);
DEFINE_FUNCTION(f32d_to_s24d, c);
DEFINE_FUNCTION(f32d_to_s24d_dither, c);
DEFINE_FUNCTION(f32_to_s24, c);
DEFINE_FUNCTION(f32_to_s24d, c);
DEFINE_FUNCTION(f32d_to_s24, c);
DEFINE_FUNCTION(f32d_to_s24_dither, c);
DEFINE_FUNCTION(f32d_to_s24s, c);
DEFINE_FUNCTION(f32d_to_s24s_dither, c);
DEFINE_FUNCTION(f32_to_u24_32, c);
DEFINE_FUNCTION(f32d_to_u24_32, c);
DEFINE_FUNCTION(f32d_to_s24_32d, c);
DEFINE_FUNCTION(f32d_to_s24_32d_dither, c);
DEFINE_FUNCTION(f32_to_s24_32, c);
DEFINE_FUNCTION(f32_to_s24_32d, c);
DEFINE_FUNCTION(f32d_to_s24_32, c);
DEFINE_FUNCTION(f32d_to_s24_32_dither, c);
DEFINE_FUNCTION(f32d_to_s24_32s, c);
DEFINE_FUNCTION(f32d_to_s24_32s_dither, c);
DEFINE_FUNCTION(f32d_to_f64d, c);
DEFINE_FUNCTION(f32_to_f64, c);
DEFINE_FUNCTION(f32_to_f64d, c);
DEFINE_FUNCTION(f32d_to_f64, c);
DEFINE_FUNCTION(f32d_to_f64s, c);
DEFINE_FUNCTION(8_to_8d, c);
DEFINE_FUNCTION(16_to_16d, c);
DEFINE_FUNCTION(24_to_24d, c);
DEFINE_FUNCTION(32_to_32d, c);
DEFINE_FUNCTION(32s_to_32sd, c);
DEFINE_FUNCTION(64_to_64d, c);
DEFINE_FUNCTION(64s_to_64sd, c);
DEFINE_FUNCTION(8d_to_8, c);
DEFINE_FUNCTION(16d_to_16, c);
DEFINE_FUNCTION(24d_to_24, c);
DEFINE_FUNCTION(32d_to_32, c);
DEFINE_FUNCTION(32sd_to_32s, c);
DEFINE_FUNCTION(64d_to_64, c);
DEFINE_FUNCTION(64sd_to_64s, c);

#if defined(HAVE_NEON)
DEFINE_FUNCTION(s16_to_f32d_2, neon);
DEFINE_FUNCTION(s16_to_f32d, neon);
DEFINE_FUNCTION(f32d_to_s16, neon);
#endif
#if defined(HAVE_SSE2)
DEFINE_FUNCTION(s16_to_f32d_2, sse2);
DEFINE_FUNCTION(s16_to_f32d, sse2);
DEFINE_FUNCTION(s24_to_f32d, sse2);
DEFINE_FUNCTION(s32_to_f32d, sse2);
DEFINE_FUNCTION(f32d_to_s32, sse2);
DEFINE_FUNCTION(f32d_to_s32_dither, sse2);
DEFINE_FUNCTION(f32_to_s16, sse2);
DEFINE_FUNCTION(f32d_to_s16_2, sse2);
DEFINE_FUNCTION(f32d_to_s16, sse2);
DEFINE_FUNCTION(f32d_to_s16d, sse2);
DEFINE_FUNCTION(32_to_32d, sse2);
DEFINE_FUNCTION(32s_to_32sd, sse2);
DEFINE_FUNCTION(32d_to_32, sse2);
DEFINE_FUNCTION(32sd_to_32s, sse2);
#endif
#if defined(HAVE_SSSE3)
DEFINE_FUNCTION(s24_to_f32d, ssse3);
#endif
#if defined(HAVE_SSE41)
DEFINE_FUNCTION(s24_to_f32d, sse41);
#endif
#if defined(HAVE_AVX2)
DEFINE_FUNCTION(s16_to_f32d_2, avx2);
DEFINE_FUNCTION(s16_to_f32d, avx2);
DEFINE_FUNCTION(s24_to_f32d, avx2);
DEFINE_FUNCTION(s32_to_f32d, avx2);
DEFINE_FUNCTION(f32d_to_s32, avx2);
DEFINE_FUNCTION(f32d_to_s16_4, avx2);
DEFINE_FUNCTION(f32d_to_s16_2, avx2);
DEFINE_FUNCTION(f32d_to_s16, avx2);
#endif

#undef DEFINE_FUNCTION
