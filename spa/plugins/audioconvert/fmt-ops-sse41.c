/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "fmt-ops.h"

#include <smmintrin.h>

#define spa_read_unaligned(ptr, type) \
__extension__ ({ \
	__typeof__(type) _val; \
	memcpy(&_val, (ptr), sizeof(_val)); \
	_val; \
})

static void
conv_s24_to_f32d_1s_sse41(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	const int24_t *s = src;
	float *d0 = dst[0];
	uint32_t n, unrolled;
	__m128i in = _mm_setzero_si128();
	__m128 out, factor = _mm_set1_ps(1.0f / S24_SCALE);

	if (SPA_IS_ALIGNED(d0, 16))
		unrolled = n_samples & ~3;
	else
		unrolled = 0;

	for(n = 0; n < unrolled; n += 4) {
		in = _mm_insert_epi32(in, spa_read_unaligned(&s[0 * n_channels], uint32_t), 0);
		in = _mm_insert_epi32(in, spa_read_unaligned(&s[1 * n_channels], uint32_t), 1);
		in = _mm_insert_epi32(in, spa_read_unaligned(&s[2 * n_channels], uint32_t), 2);
		in = _mm_insert_epi32(in, spa_read_unaligned(&s[3 * n_channels], uint32_t), 3);
		in = _mm_slli_epi32(in, 8);
		in = _mm_srai_epi32(in, 8);
		out = _mm_cvtepi32_ps(in);
		out = _mm_mul_ps(out, factor);
		_mm_store_ps(&d0[n], out);
		s += 4 * n_channels;
	}
	for(; n < n_samples; n++) {
		out = _mm_cvtsi32_ss(factor, s24_to_s32(*s));
		out = _mm_mul_ss(out, factor);
		_mm_store_ss(&d0[n], out);
		s += n_channels;
	}
}

extern void conv_s24_to_f32d_2s_sse2(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples);
extern void conv_s24_to_f32d_4s_ssse3(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples);

void
conv_s24_to_f32d_sse41(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int8_t *s = src[0];
	uint32_t i = 0, n_channels = conv->n_channels;

#if defined (HAVE_SSSE3)
	for(; i + 3 < n_channels; i += 4)
		conv_s24_to_f32d_4s_ssse3(conv, &dst[i], &s[3*i], n_channels, n_samples);
#endif
#if defined (HAVE_SSE2)
	for(; i + 1 < n_channels; i += 2)
		conv_s24_to_f32d_2s_sse2(conv, &dst[i], &s[3*i], n_channels, n_samples);
#endif
	for(; i < n_channels; i++)
		conv_s24_to_f32d_1s_sse41(conv, &dst[i], &s[3*i], n_channels, n_samples);
}
