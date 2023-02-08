/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <spa/support/cpu.h>
#include <spa/utils/defs.h>
#include <spa/param/audio/format-utils.h>

#include "fmt-ops.h"

#define NOISE_SIZE	(1<<10)
#define RANDOM_SIZE	(16)

typedef void (*convert_func_t) (struct convert *conv, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples);

struct conv_info {
	uint32_t src_fmt;
	uint32_t dst_fmt;
	uint32_t n_channels;

	convert_func_t process;
	const char *name;

	uint32_t cpu_flags;
#define CONV_NOISE	(1<<0)
#define CONV_SHAPE	(1<<1)
	uint32_t conv_flags;
};

#define MAKE(fmt1,fmt2,chan,func,...) \
	{  SPA_AUDIO_FORMAT_ ##fmt1, SPA_AUDIO_FORMAT_ ##fmt2, chan, func, #func , __VA_ARGS__ }

static struct conv_info conv_table[] =
{
	/* to f32 */
	MAKE(U8, F32, 0, conv_u8_to_f32_c),
	MAKE(U8, F32, 0, conv_u8_to_f32_c),
	MAKE(U8P, F32P, 0, conv_u8d_to_f32d_c),
	MAKE(U8, F32P, 0, conv_u8_to_f32d_c),
	MAKE(U8P, F32, 0, conv_u8d_to_f32_c),

	MAKE(S8, F32, 0, conv_s8_to_f32_c),
	MAKE(S8P, F32P, 0, conv_s8d_to_f32d_c),
	MAKE(S8, F32P, 0, conv_s8_to_f32d_c),
	MAKE(S8P, F32, 0, conv_s8d_to_f32_c),

	MAKE(ALAW, F32P, 0, conv_alaw_to_f32d_c),
	MAKE(ULAW, F32P, 0, conv_ulaw_to_f32d_c),

	MAKE(U16, F32, 0, conv_u16_to_f32_c),
	MAKE(U16, F32P, 0, conv_u16_to_f32d_c),

	MAKE(S16, F32, 0, conv_s16_to_f32_c),
	MAKE(S16P, F32P, 0, conv_s16d_to_f32d_c),
#if defined (HAVE_NEON)
	MAKE(S16, F32P, 2, conv_s16_to_f32d_2_neon, SPA_CPU_FLAG_NEON),
	MAKE(S16, F32P, 0, conv_s16_to_f32d_neon, SPA_CPU_FLAG_NEON),
#endif
#if defined (HAVE_AVX2)
	MAKE(S16, F32P, 2, conv_s16_to_f32d_2_avx2, SPA_CPU_FLAG_AVX2),
	MAKE(S16, F32P, 0, conv_s16_to_f32d_avx2, SPA_CPU_FLAG_AVX2),
#endif
#if defined (HAVE_SSE2)
	MAKE(S16, F32P, 2, conv_s16_to_f32d_2_sse2, SPA_CPU_FLAG_SSE2),
	MAKE(S16, F32P, 0, conv_s16_to_f32d_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(S16, F32P, 0, conv_s16_to_f32d_c),
	MAKE(S16P, F32, 0, conv_s16d_to_f32_c),

	MAKE(S16_OE, F32P, 0, conv_s16s_to_f32d_c),

	MAKE(F32, F32, 0, conv_copy32_c),
	MAKE(F32P, F32P, 0, conv_copy32d_c),
#if defined (HAVE_SSE2)
	MAKE(F32, F32P, 0, conv_32_to_32d_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(F32, F32P, 0, conv_32_to_32d_c),
#if defined (HAVE_SSE2)
	MAKE(F32P, F32, 0, conv_32d_to_32_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(F32P, F32, 0, conv_32d_to_32_c),

#if defined (HAVE_SSE2)
	MAKE(F32_OE, F32P, 0, conv_32s_to_32d_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(F32_OE, F32P, 0, conv_32s_to_32d_c),
#if defined (HAVE_SSE2)
	MAKE(F32P, F32_OE, 0, conv_32d_to_32s_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(F32P, F32_OE, 0, conv_32d_to_32s_c),

	MAKE(U32, F32, 0, conv_u32_to_f32_c),
	MAKE(U32, F32P, 0, conv_u32_to_f32d_c),

#if defined (HAVE_AVX2)
	MAKE(S32, F32P, 0, conv_s32_to_f32d_avx2, SPA_CPU_FLAG_AVX2),
#endif
#if defined (HAVE_SSE2)
	MAKE(S32, F32P, 0, conv_s32_to_f32d_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(S32, F32, 0, conv_s32_to_f32_c),
	MAKE(S32P, F32P, 0, conv_s32d_to_f32d_c),
	MAKE(S32, F32P, 0, conv_s32_to_f32d_c),
	MAKE(S32P, F32, 0, conv_s32d_to_f32_c),

	MAKE(S32_OE, F32P, 0, conv_s32s_to_f32d_c),

	MAKE(U24, F32, 0, conv_u24_to_f32_c),
	MAKE(U24, F32P, 0, conv_u24_to_f32d_c),

	MAKE(S24, F32, 0, conv_s24_to_f32_c),
	MAKE(S24P, F32P, 0, conv_s24d_to_f32d_c),
#if defined (HAVE_AVX2)
	MAKE(S24, F32P, 0, conv_s24_to_f32d_avx2, SPA_CPU_FLAG_AVX2),
#endif
#if defined (HAVE_SSSE3)
//	MAKE(S24, F32P, 0, conv_s24_to_f32d_ssse3, SPA_CPU_FLAG_SSSE3),
#endif
#if defined (HAVE_SSE41)
	MAKE(S24, F32P, 0, conv_s24_to_f32d_sse41, SPA_CPU_FLAG_SSE41),
#endif
#if defined (HAVE_SSE2)
	MAKE(S24, F32P, 0, conv_s24_to_f32d_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(S24, F32P, 0, conv_s24_to_f32d_c),
	MAKE(S24P, F32, 0, conv_s24d_to_f32_c),

	MAKE(S24_OE, F32P, 0, conv_s24s_to_f32d_c),

	MAKE(U24_32, F32, 0, conv_u24_32_to_f32_c),
	MAKE(U24_32, F32P, 0, conv_u24_32_to_f32d_c),

	MAKE(S24_32, F32, 0, conv_s24_32_to_f32_c),
	MAKE(S24_32P, F32P, 0, conv_s24_32d_to_f32d_c),
	MAKE(S24_32, F32P, 0, conv_s24_32_to_f32d_c),
	MAKE(S24_32P, F32, 0, conv_s24_32d_to_f32_c),

	MAKE(S24_32_OE, F32P, 0, conv_s24_32s_to_f32d_c),

	MAKE(F64, F32, 0, conv_f64_to_f32_c),
	MAKE(F64P, F32P, 0, conv_f64d_to_f32d_c),
	MAKE(F64, F32P, 0, conv_f64_to_f32d_c),
	MAKE(F64P, F32, 0, conv_f64d_to_f32_c),

	MAKE(F64_OE, F32P, 0, conv_f64s_to_f32d_c),

	/* from f32 */
	MAKE(F32, U8, 0, conv_f32_to_u8_c),
	MAKE(F32P, U8P, 0, conv_f32d_to_u8d_shaped_c, 0, CONV_SHAPE),
	MAKE(F32P, U8P, 0, conv_f32d_to_u8d_noise_c, 0, CONV_NOISE),
	MAKE(F32P, U8P, 0, conv_f32d_to_u8d_c),
	MAKE(F32, U8P, 0, conv_f32_to_u8d_c),
	MAKE(F32P, U8, 0, conv_f32d_to_u8_shaped_c, 0, CONV_SHAPE),
	MAKE(F32P, U8, 0, conv_f32d_to_u8_noise_c, 0, CONV_NOISE),
	MAKE(F32P, U8, 0, conv_f32d_to_u8_c),

	MAKE(F32, S8, 0, conv_f32_to_s8_c),
	MAKE(F32P, S8P, 0, conv_f32d_to_s8d_shaped_c, 0, CONV_SHAPE),
	MAKE(F32P, S8P, 0, conv_f32d_to_s8d_noise_c, 0, CONV_NOISE),
	MAKE(F32P, S8P, 0, conv_f32d_to_s8d_c),
	MAKE(F32, S8P, 0, conv_f32_to_s8d_c),
	MAKE(F32P, S8, 0, conv_f32d_to_s8_shaped_c, 0, CONV_SHAPE),
	MAKE(F32P, S8, 0, conv_f32d_to_s8_noise_c, 0, CONV_NOISE),
	MAKE(F32P, S8, 0, conv_f32d_to_s8_c),

	MAKE(F32P, ALAW, 0, conv_f32d_to_alaw_c),
	MAKE(F32P, ULAW, 0, conv_f32d_to_ulaw_c),

	MAKE(F32, U16, 0, conv_f32_to_u16_c),
	MAKE(F32P, U16, 0, conv_f32d_to_u16_c),

#if defined (HAVE_SSE2)
	MAKE(F32, S16, 0, conv_f32_to_s16_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(F32, S16, 0, conv_f32_to_s16_c),

	MAKE(F32P, S16P, 0, conv_f32d_to_s16d_shaped_c, 0, CONV_SHAPE),
#if defined (HAVE_SSE2)
	MAKE(F32P, S16P, 0, conv_f32d_to_s16d_noise_sse2, SPA_CPU_FLAG_SSE2, CONV_NOISE),
#endif
	MAKE(F32P, S16P, 0, conv_f32d_to_s16d_noise_c, 0, CONV_NOISE),
#if defined (HAVE_SSE2)
	MAKE(F32P, S16P, 0, conv_f32d_to_s16d_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(F32P, S16P, 0, conv_f32d_to_s16d_c),

	MAKE(F32, S16P, 0, conv_f32_to_s16d_c),

	MAKE(F32P, S16, 0, conv_f32d_to_s16_shaped_c, 0, CONV_SHAPE),
#if defined (HAVE_SSE2)
	MAKE(F32P, S16, 0, conv_f32d_to_s16_noise_sse2, SPA_CPU_FLAG_SSE2, CONV_NOISE),
#endif
	MAKE(F32P, S16, 0, conv_f32d_to_s16_noise_c, 0, CONV_NOISE),
#if defined (HAVE_NEON)
	MAKE(F32P, S16, 0, conv_f32d_to_s16_neon, SPA_CPU_FLAG_NEON),
#endif
#if defined (HAVE_AVX2)
	MAKE(F32P, S16, 4, conv_f32d_to_s16_4_avx2, SPA_CPU_FLAG_AVX2),
	MAKE(F32P, S16, 2, conv_f32d_to_s16_2_avx2, SPA_CPU_FLAG_AVX2),
	MAKE(F32P, S16, 0, conv_f32d_to_s16_avx2, SPA_CPU_FLAG_AVX2),
#endif
#if defined (HAVE_SSE2)
	MAKE(F32P, S16, 2, conv_f32d_to_s16_2_sse2, SPA_CPU_FLAG_SSE2),
	MAKE(F32P, S16, 0, conv_f32d_to_s16_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(F32P, S16, 0, conv_f32d_to_s16_c),

	MAKE(F32P, S16_OE, 0, conv_f32d_to_s16s_shaped_c, 0, CONV_SHAPE),
	MAKE(F32P, S16_OE, 0, conv_f32d_to_s16s_noise_c, 0, CONV_NOISE),
	MAKE(F32P, S16_OE, 0, conv_f32d_to_s16s_c),

	MAKE(F32, U32, 0, conv_f32_to_u32_c),
	MAKE(F32P, U32, 0, conv_f32d_to_u32_c),

	MAKE(F32, S32, 0, conv_f32_to_s32_c),
	MAKE(F32P, S32P, 0, conv_f32d_to_s32d_noise_c, 0, CONV_NOISE),
	MAKE(F32P, S32P, 0, conv_f32d_to_s32d_c),
	MAKE(F32, S32P, 0, conv_f32_to_s32d_c),

#if defined (HAVE_SSE2)
	MAKE(F32P, S32, 0, conv_f32d_to_s32_noise_sse2, SPA_CPU_FLAG_SSE2, CONV_NOISE),
#endif
	MAKE(F32P, S32, 0, conv_f32d_to_s32_noise_c, 0, CONV_NOISE),

#if defined (HAVE_AVX2)
	MAKE(F32P, S32, 0, conv_f32d_to_s32_avx2, SPA_CPU_FLAG_AVX2),
#endif
#if defined (HAVE_SSE2)
	MAKE(F32P, S32, 0, conv_f32d_to_s32_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(F32P, S32, 0, conv_f32d_to_s32_c),

	MAKE(F32P, S32_OE, 0, conv_f32d_to_s32s_noise_c, 0, CONV_NOISE),
	MAKE(F32P, S32_OE, 0, conv_f32d_to_s32s_c),

	MAKE(F32, U24, 0, conv_f32_to_u24_c),
	MAKE(F32P, U24, 0, conv_f32d_to_u24_c),

	MAKE(F32, S24, 0, conv_f32_to_s24_c),
	MAKE(F32P, S24P, 0, conv_f32d_to_s24d_noise_c, 0, CONV_NOISE),
	MAKE(F32P, S24P, 0, conv_f32d_to_s24d_c),
	MAKE(F32, S24P, 0, conv_f32_to_s24d_c),
	MAKE(F32P, S24, 0, conv_f32d_to_s24_noise_c, 0, CONV_NOISE),
	MAKE(F32P, S24, 0, conv_f32d_to_s24_c),

	MAKE(F32P, S24_OE, 0, conv_f32d_to_s24s_noise_c, 0, CONV_NOISE),
	MAKE(F32P, S24_OE, 0, conv_f32d_to_s24s_c),

	MAKE(F32, U24_32, 0, conv_f32_to_u24_32_c),
	MAKE(F32P, U24_32, 0, conv_f32d_to_u24_32_c),

	MAKE(F32, S24_32, 0, conv_f32_to_s24_32_c),
	MAKE(F32P, S24_32P, 0, conv_f32d_to_s24_32d_noise_c, 0, CONV_NOISE),
	MAKE(F32P, S24_32P, 0, conv_f32d_to_s24_32d_c),
	MAKE(F32, S24_32P, 0, conv_f32_to_s24_32d_c),
	MAKE(F32P, S24_32, 0, conv_f32d_to_s24_32_noise_c, 0, CONV_NOISE),
	MAKE(F32P, S24_32, 0, conv_f32d_to_s24_32_c),

	MAKE(F32P, S24_32_OE, 0, conv_f32d_to_s24_32s_noise_c, 0, CONV_NOISE),
	MAKE(F32P, S24_32_OE, 0, conv_f32d_to_s24_32s_c),

	MAKE(F32, F64, 0, conv_f32_to_f64_c),
	MAKE(F32P, F64P, 0, conv_f32d_to_f64d_c),
	MAKE(F32, F64P, 0, conv_f32_to_f64d_c),
	MAKE(F32P, F64, 0, conv_f32d_to_f64_c),

	MAKE(F32P, F64_OE, 0, conv_f32d_to_f64s_c),

	/* u8 */
	MAKE(U8, U8, 0, conv_copy8_c),
	MAKE(U8P, U8P, 0, conv_copy8d_c),
	MAKE(U8, U8P, 0, conv_8_to_8d_c),
	MAKE(U8P, U8, 0, conv_8d_to_8_c),

	/* s8 */
	MAKE(S8, S8, 0, conv_copy8_c),
	MAKE(S8P, S8P, 0, conv_copy8d_c),
	MAKE(S8, S8P, 0, conv_8_to_8d_c),
	MAKE(S8P, S8, 0, conv_8d_to_8_c),

	/* alaw */
	MAKE(ALAW, ALAW, 0, conv_copy8_c),
	/* ulaw */
	MAKE(ULAW, ULAW, 0, conv_copy8_c),

	/* s16 */
	MAKE(S16, S16, 0, conv_copy16_c),
	MAKE(S16P, S16P, 0, conv_copy16d_c),
	MAKE(S16, S16P, 0, conv_16_to_16d_c),
	MAKE(S16P, S16, 0, conv_16d_to_16_c),

	/* s32 */
	MAKE(S32, S32, 0, conv_copy32_c),
	MAKE(S32P, S32P, 0, conv_copy32d_c),
#if defined (HAVE_SSE2)
	MAKE(S32, S32P, 0, conv_32_to_32d_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(S32, S32P, 0, conv_32_to_32d_c),
#if defined (HAVE_SSE2)
	MAKE(S32P, S32, 0, conv_32d_to_32_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(S32P, S32, 0, conv_32d_to_32_c),

	/* s24 */
	MAKE(S24, S24, 0, conv_copy24_c),
	MAKE(S24P, S24P, 0, conv_copy24d_c),
	MAKE(S24, S24P, 0, conv_24_to_24d_c),
	MAKE(S24P, S24, 0, conv_24d_to_24_c),

	/* s24_32 */
	MAKE(S24_32, S24_32, 0, conv_copy32_c),
	MAKE(S24_32P, S24_32P, 0, conv_copy32d_c),
#if defined (HAVE_SSE2)
	MAKE(S24_32, S24_32P, 0, conv_32_to_32d_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(S24_32, S24_32P, 0, conv_32_to_32d_c),
#if defined (HAVE_SSE2)
	MAKE(S24_32P, S24_32, 0, conv_32d_to_32_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(S24_32P, S24_32, 0, conv_32d_to_32_c),

	/* F64 */
	MAKE(F64, F64, 0, conv_copy64_c),
	MAKE(F64P, F64P, 0, conv_copy64d_c),
	MAKE(F64, F64P, 0, conv_64_to_64d_c),
	MAKE(F64P, F64, 0, conv_64d_to_64_c),
};
#undef MAKE

#define MATCH_CHAN(a,b)		((a) == 0 || (a) == (b))
#define MATCH_CPU_FLAGS(a,b)	((a) == 0 || ((a) & (b)) == a)
#define MATCH_DITHER(a,b)	((a) == 0 || ((a) & (b)) == a)

static const struct conv_info *find_conv_info(uint32_t src_fmt, uint32_t dst_fmt,
		uint32_t n_channels, uint32_t cpu_flags, uint32_t conv_flags)
{
	SPA_FOR_EACH_ELEMENT_VAR(conv_table, c) {
		if (c->src_fmt == src_fmt &&
		    c->dst_fmt == dst_fmt &&
		    MATCH_CHAN(c->n_channels, n_channels) &&
		    MATCH_CPU_FLAGS(c->cpu_flags, cpu_flags) &&
		    MATCH_DITHER(c->conv_flags, conv_flags))
			return c;
	}
	return NULL;
}

typedef void (*noise_func_t) (struct convert *conv, float * noise, uint32_t n_samples);

struct noise_info {
	uint32_t method;

	noise_func_t noise;
	const char *name;

	uint32_t cpu_flags;
};

#define MAKE(method,func,...) \
	{  NOISE_METHOD_ ##method, func, #func , __VA_ARGS__ }

static struct noise_info noise_table[] =
{
#if defined (HAVE_SSE2)
	MAKE(RECTANGULAR, conv_noise_rect_sse2, SPA_CPU_FLAG_SSE2),
	MAKE(TRIANGULAR, conv_noise_tri_sse2, SPA_CPU_FLAG_SSE2),
	MAKE(TRIANGULAR_HF, conv_noise_tri_hf_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(NONE, conv_noise_none_c),
	MAKE(RECTANGULAR, conv_noise_rect_c),
	MAKE(TRIANGULAR, conv_noise_tri_c),
	MAKE(TRIANGULAR_HF, conv_noise_tri_hf_c),
	MAKE(PATTERN, conv_noise_pattern_c),
};
#undef MAKE

static const struct noise_info *find_noise_info(uint32_t method,
		uint32_t cpu_flags)
{
	SPA_FOR_EACH_ELEMENT_VAR(noise_table, t) {
		if (t->method == method &&
		    MATCH_CPU_FLAGS(t->cpu_flags, cpu_flags))
			return t;
	}
	return NULL;
}

static void impl_convert_free(struct convert *conv)
{
	conv->process = NULL;
	free(conv->data);
	conv->data = NULL;
}

static bool need_dither(uint32_t format)
{
	switch (format) {
	case SPA_AUDIO_FORMAT_U8:
	case SPA_AUDIO_FORMAT_U8P:
	case SPA_AUDIO_FORMAT_S8:
	case SPA_AUDIO_FORMAT_S8P:
	case SPA_AUDIO_FORMAT_ULAW:
	case SPA_AUDIO_FORMAT_ALAW:
	case SPA_AUDIO_FORMAT_S16P:
	case SPA_AUDIO_FORMAT_S16:
	case SPA_AUDIO_FORMAT_S16_OE:
		return true;
	}
	return false;
}

/* filters based on F-weighted curves
 * from 'Psychoacoustically Optimal Noise Shaping' (**)
 * this filter is the "F-Weighted" noise filter described by Wannamaker
 * It is designed to produce minimum audibility: */
static const float wan3[] = { /* Table 3; 3 Coefficients */
	1.623f, -0.982f, 0.109f
};
/* Noise shaping coefficients from[1], moves most power of the
 * error noise into inaudible frequency ranges.
 *
 * [1]
 * "Minimally Audible Noise Shaping", Stanley P. Lipshitz,
 * John Vanderkooy, and Robert A. Wannamaker,
 * J. Audio Eng. Soc., Vol. 39, No. 11, November 1991. */
static const float lips44[] = { /* improved E-weighted (appendix: 5) */
	2.033f, -2.165f, 1.959f, -1.590f, 0.6149f
};

static const struct dither_info {
	uint32_t method;
	uint32_t noise_method;
	uint32_t rate;
	const float *ns;
	uint32_t n_ns;
} dither_info[] = {
	{ DITHER_METHOD_NONE, NOISE_METHOD_NONE, },
	{ DITHER_METHOD_RECTANGULAR, NOISE_METHOD_RECTANGULAR, },
	{ DITHER_METHOD_TRIANGULAR, NOISE_METHOD_TRIANGULAR, },
	{ DITHER_METHOD_TRIANGULAR_HF, NOISE_METHOD_TRIANGULAR_HF, },
	{ DITHER_METHOD_WANNAMAKER_3, NOISE_METHOD_TRIANGULAR_HF, 44100, wan3, SPA_N_ELEMENTS(wan3) },
	{ DITHER_METHOD_LIPSHITZ, NOISE_METHOD_TRIANGULAR, 44100, lips44, SPA_N_ELEMENTS(lips44) }
};

static const struct dither_info *find_dither_info(uint32_t method, uint32_t rate)
{
	SPA_FOR_EACH_ELEMENT_VAR(dither_info, di) {
		if (di->method != method)
			continue;
		/* don't use shaped for too low rates, it moves the noise to
		 * audible ranges */
		if (di->ns != NULL && rate < di->rate * 3 / 4)
			return find_dither_info(DITHER_METHOD_TRIANGULAR_HF, rate);
		return di;
	}
	return NULL;
}

int convert_init(struct convert *conv)
{
	const struct conv_info *info;
	const struct dither_info *dinfo;
	const struct noise_info *ninfo;
	uint32_t i, conv_flags, data_size[3];

	conv->scale = 1.0f / (float)(INT32_MAX);

	/* disable dither if not needed */
	if (!need_dither(conv->dst_fmt))
		conv->method = DITHER_METHOD_NONE;

	dinfo = find_dither_info(conv->method, conv->rate);
	if (dinfo == NULL)
		return -EINVAL;

	conv->noise_method = dinfo->noise_method;
	if (conv->noise_bits > 0) {
		switch (conv->noise_method) {
		case NOISE_METHOD_NONE:
			conv->noise_method = NOISE_METHOD_PATTERN;
			conv->scale = -1.0f * (1 << (conv->noise_bits-1));
			break;
		case NOISE_METHOD_RECTANGULAR:
			conv->noise_method = NOISE_METHOD_TRIANGULAR;
			SPA_FALLTHROUGH;
		case NOISE_METHOD_TRIANGULAR:
		case NOISE_METHOD_TRIANGULAR_HF:
			conv->scale *= (1 << (conv->noise_bits-1));
			break;
		}
	}
	if (conv->noise_method < NOISE_METHOD_TRIANGULAR)
		conv->scale *= 0.5f;

	conv_flags = 0;
	if (conv->noise_method != NOISE_METHOD_NONE)
		conv_flags |= CONV_NOISE;
	if (dinfo->n_ns > 0) {
		conv_flags |= CONV_SHAPE;
		conv->n_ns = dinfo->n_ns;
		conv->ns = dinfo->ns;
	}

	info = find_conv_info(conv->src_fmt, conv->dst_fmt, conv->n_channels,
			conv->cpu_flags, conv_flags);
	if (info == NULL)
		return -ENOTSUP;

	ninfo = find_noise_info(conv->noise_method, conv->cpu_flags);
	if (ninfo == NULL)
		return -ENOTSUP;

	conv->noise_size = NOISE_SIZE;

	data_size[0] = SPA_ROUND_UP(conv->noise_size * sizeof(float), FMT_OPS_MAX_ALIGN);
	data_size[1] = SPA_ROUND_UP(RANDOM_SIZE * sizeof(uint32_t), FMT_OPS_MAX_ALIGN);
	data_size[2] = SPA_ROUND_UP(RANDOM_SIZE * sizeof(int32_t), FMT_OPS_MAX_ALIGN);

	conv->data = calloc(FMT_OPS_MAX_ALIGN +
			data_size[0] + data_size[1] + data_size[2], 1);
	if (conv->data == NULL)
		return -errno;

	conv->noise = SPA_PTR_ALIGN(conv->data, FMT_OPS_MAX_ALIGN, float);
	conv->random = SPA_PTROFF(conv->noise, data_size[0], uint32_t);
	conv->prev = SPA_PTROFF(conv->random, data_size[1], int32_t);

	for (i = 0; i < RANDOM_SIZE; i++)
		conv->random[i] = random();

	conv->is_passthrough = conv->src_fmt == conv->dst_fmt;
	conv->cpu_flags = info->cpu_flags;
	conv->update_noise = ninfo->noise;
	conv->process = info->process;
	conv->free = impl_convert_free;
	conv->func_name = info->name;

	return 0;
}
