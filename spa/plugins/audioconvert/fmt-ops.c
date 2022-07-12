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

#define DITHER_SIZE	(1<<10)

typedef void (*convert_func_t) (struct convert *conv, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples);

struct conv_info {
	uint32_t src_fmt;
	uint32_t dst_fmt;
	uint32_t n_channels;

	convert_func_t process;
	const char *name;

	uint32_t cpu_flags;
#define CONV_DITHER	(1<<0)
#define CONV_SHAPE	(1<<1)
	uint32_t dither_flags;
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
	MAKE(F32P, U8P, 0, conv_f32d_to_u8d_dither_c, 0, CONV_DITHER),
	MAKE(F32P, U8P, 0, conv_f32d_to_u8d_c),
	MAKE(F32, U8P, 0, conv_f32_to_u8d_c),
	MAKE(F32P, U8, 0, conv_f32d_to_u8_shaped_c, 0, CONV_SHAPE),
	MAKE(F32P, U8, 0, conv_f32d_to_u8_dither_c, 0, CONV_DITHER),
	MAKE(F32P, U8, 0, conv_f32d_to_u8_c),

	MAKE(F32, S8, 0, conv_f32_to_s8_c),
	MAKE(F32P, S8P, 0, conv_f32d_to_s8d_shaped_c, 0, CONV_SHAPE),
	MAKE(F32P, S8P, 0, conv_f32d_to_s8d_dither_c, 0, CONV_DITHER),
	MAKE(F32P, S8P, 0, conv_f32d_to_s8d_c),
	MAKE(F32, S8P, 0, conv_f32_to_s8d_c),
	MAKE(F32P, S8, 0, conv_f32d_to_s8_shaped_c, 0, CONV_SHAPE),
	MAKE(F32P, S8, 0, conv_f32d_to_s8_dither_c, 0, CONV_DITHER),
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
	MAKE(F32P, S16P, 0, conv_f32d_to_s16d_dither_sse2, SPA_CPU_FLAG_SSE2, CONV_DITHER),
#endif
	MAKE(F32P, S16P, 0, conv_f32d_to_s16d_dither_c, 0, CONV_DITHER),
#if defined (HAVE_SSE2)
	MAKE(F32P, S16P, 0, conv_f32d_to_s16d_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(F32P, S16P, 0, conv_f32d_to_s16d_c),

	MAKE(F32, S16P, 0, conv_f32_to_s16d_c),

	MAKE(F32P, S16, 0, conv_f32d_to_s16_shaped_c, 0, CONV_SHAPE),
#if defined (HAVE_SSE2)
	MAKE(F32P, S16, 0, conv_f32d_to_s16_dither_sse2, SPA_CPU_FLAG_SSE2, CONV_DITHER),
#endif
	MAKE(F32P, S16, 0, conv_f32d_to_s16_dither_c, 0, CONV_DITHER),
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
	MAKE(F32P, S16_OE, 0, conv_f32d_to_s16s_dither_c, 0, CONV_DITHER),
	MAKE(F32P, S16_OE, 0, conv_f32d_to_s16s_c),

	MAKE(F32, U32, 0, conv_f32_to_u32_c),
	MAKE(F32P, U32, 0, conv_f32d_to_u32_c),

	MAKE(F32, S32, 0, conv_f32_to_s32_c),
	MAKE(F32P, S32P, 0, conv_f32d_to_s32d_dither_c, 0, CONV_DITHER),
	MAKE(F32P, S32P, 0, conv_f32d_to_s32d_c),
	MAKE(F32, S32P, 0, conv_f32_to_s32d_c),

#if defined (HAVE_SSE2)
	MAKE(F32P, S32, 0, conv_f32d_to_s32_dither_sse2, SPA_CPU_FLAG_SSE2, CONV_DITHER),
#endif
	MAKE(F32P, S32, 0, conv_f32d_to_s32_dither_c, 0, CONV_DITHER),

#if defined (HAVE_AVX2)
	MAKE(F32P, S32, 0, conv_f32d_to_s32_avx2, SPA_CPU_FLAG_AVX2),
#endif
#if defined (HAVE_SSE2)
	MAKE(F32P, S32, 0, conv_f32d_to_s32_sse2, SPA_CPU_FLAG_SSE2),
#endif
	MAKE(F32P, S32, 0, conv_f32d_to_s32_c),

	MAKE(F32P, S32_OE, 0, conv_f32d_to_s32s_dither_c, 0, CONV_DITHER),
	MAKE(F32P, S32_OE, 0, conv_f32d_to_s32s_c),

	MAKE(F32, U24, 0, conv_f32_to_u24_c),
	MAKE(F32P, U24, 0, conv_f32d_to_u24_c),

	MAKE(F32, S24, 0, conv_f32_to_s24_c),
	MAKE(F32P, S24P, 0, conv_f32d_to_s24d_dither_c, 0, CONV_DITHER),
	MAKE(F32P, S24P, 0, conv_f32d_to_s24d_c),
	MAKE(F32, S24P, 0, conv_f32_to_s24d_c),
	MAKE(F32P, S24, 0, conv_f32d_to_s24_dither_c, 0, CONV_DITHER),
	MAKE(F32P, S24, 0, conv_f32d_to_s24_c),

	MAKE(F32P, S24_OE, 0, conv_f32d_to_s24s_dither_c, 0, CONV_DITHER),
	MAKE(F32P, S24_OE, 0, conv_f32d_to_s24s_c),

	MAKE(F32, U24_32, 0, conv_f32_to_u24_32_c),
	MAKE(F32P, U24_32, 0, conv_f32d_to_u24_32_c),

	MAKE(F32, S24_32, 0, conv_f32_to_s24_32_c),
	MAKE(F32P, S24_32P, 0, conv_f32d_to_s24_32d_dither_c, 0, CONV_DITHER),
	MAKE(F32P, S24_32P, 0, conv_f32d_to_s24_32d_c),
	MAKE(F32, S24_32P, 0, conv_f32_to_s24_32d_c),
	MAKE(F32P, S24_32, 0, conv_f32d_to_s24_32_dither_c, 0, CONV_DITHER),
	MAKE(F32P, S24_32, 0, conv_f32d_to_s24_32_c),

	MAKE(F32P, S24_32_OE, 0, conv_f32d_to_s24_32s_dither_c, 0, CONV_DITHER),
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
		uint32_t n_channels, uint32_t cpu_flags, uint32_t dither_flags)
{
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(conv_table); i++) {
		if (conv_table[i].src_fmt == src_fmt &&
		    conv_table[i].dst_fmt == dst_fmt &&
		    MATCH_CHAN(conv_table[i].n_channels, n_channels) &&
		    MATCH_CPU_FLAGS(conv_table[i].cpu_flags, cpu_flags) &&
		    MATCH_DITHER(conv_table[i].dither_flags, dither_flags))
			return &conv_table[i];
	}
	return NULL;
}

static void impl_convert_free(struct convert *conv)
{
	conv->process = NULL;
	free(conv->dither);
	conv->dither = NULL;
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

int convert_init(struct convert *conv)
{
	const struct conv_info *info;
	uint32_t i, dither_flags;

	conv->scale = 1.0f / (float)(INT32_MAX);

	if (conv->noise > 0)
		conv->scale *= (1 << (conv->noise + 1));

	/* disable dither if not needed */
	if (!need_dither(conv->dst_fmt))
		conv->method = DITHER_METHOD_NONE;

	/* don't use shaped for too low rates, it moves the noise to
	 * audible ranges */
	if (conv->method == DITHER_METHOD_SHAPED_5 && conv->rate < 32000)
		conv->method = DITHER_METHOD_TRIANGULAR;

	if (conv->method < DITHER_METHOD_TRIANGULAR)
		conv->scale *= 0.5f;

	dither_flags = 0;
	if (conv->method != DITHER_METHOD_NONE || conv->noise)
		dither_flags |= CONV_DITHER;
	if (conv->method == DITHER_METHOD_SHAPED_5)
		dither_flags |= CONV_SHAPE;

	info = find_conv_info(conv->src_fmt, conv->dst_fmt, conv->n_channels,
			conv->cpu_flags, dither_flags);
	if (info == NULL)
		return -ENOTSUP;

	conv->dither_size = DITHER_SIZE;
	conv->dither = calloc(conv->dither_size + 16 +
			FMT_OPS_MAX_ALIGN / sizeof(float), sizeof(float));
	if (conv->dither == NULL)
		return -errno;

	for (i = 0; i < SPA_N_ELEMENTS(conv->random); i++)
		conv->random[i] = random();

	conv->is_passthrough = conv->src_fmt == conv->dst_fmt;
	conv->cpu_flags = info->cpu_flags;
	conv->process = info->process;
	conv->free = impl_convert_free;
	conv->func_name = info->name;

	return 0;
}
