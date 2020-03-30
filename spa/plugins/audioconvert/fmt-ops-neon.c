/* Spa
 *
 * Copyright © 2020 Wim Taymans
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

#include "fmt-ops.h"

static void
conv_s16_to_f32d_2s_neon(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	const int16_t *s = src;
	float *d0 = dst[0], *d1 = dst[1];

#ifdef __aarch64__
	uint32_t stride = n_channels << 1;
	unsigned int remainder = n_samples & 3;
	n_samples -= remainder;

	asm volatile(
		"      cmp %[n_samples], #0\n"
		"      beq 2f\n"
		"1:"
		"      ld2 { v0.h, v1.h }[0], [%[s]], %[stride]\n"
		"      ld2 { v0.h, v1.h }[1], [%[s]], %[stride]\n"
		"      ld2 { v0.h, v1.h }[2], [%[s]], %[stride]\n"
		"      ld2 { v0.h, v1.h }[3], [%[s]], %[stride]\n"
		"      subs %[n_samples], %[n_samples], #4\n"
		"      sshll v2.4s, v0.4h, #0\n"
		"      sshll v3.4s, v1.4h, #0\n"
		"      scvtf v0.4s, v2.4s, #15\n"
		"      scvtf v1.4s, v3.4s, #15\n"
		"      st1 { v0.4s }, [%[d0]], #16\n"
		"      st1 { v1.4s }, [%[d1]], #16\n"
		"      bne 1b\n"
		"2:"
		"      cmp %[remainder], #0\n"
		"      beq 4f\n"
		"3:"
		"      ld2 { v0.h, v1.h }[0], [%[s]], %[stride]\n"
		"      subs %[remainder], %[remainder], #1\n"
		"      sshll v2.4s, v0.4h, #0\n"
		"      sshll v3.4s, v1.4h, #0\n"
		"      scvtf v0.4s, v2.4s, #15\n"
		"      scvtf v1.4s, v3.4s, #15\n"
		"      st1 { v0.s }[0], [%[d0]], #4\n"
		"      st1 { v1.s }[0], [%[d1]], #4\n"
		"      bne 3b\n"
		"4:"
		: [d0] "+r" (d0), [d1] "+r" (d1), [s] "+r" (s), [n_samples] "+r" (n_samples),
		  [remainder] "+r" (remainder)
		: [stride] "r" (stride)
		: "cc", "v0", "v1", "v2", "v3");
#else
	uint32_t n;
	for(n = 0; n < n_samples; n++) {
		*d0++ = S16_TO_F32(s[0]);
		*d1++ = S16_TO_F32(s[1]);
		s += n_channels;
	}
#endif
}

static void
conv_s16_to_f32d_1s_neon(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	const int16_t *s = src;
	float *d = dst[0];
#ifdef __aarch64__
	uint32_t stride = n_channels << 1;
	uint32_t remainder = n_samples & 3;
	n_samples -= remainder;

	asm volatile(
		"      cmp %[n_samples], #0\n"
		"      beq 2f\n"
		"1:"
		"      ld1 { v0.h }[0], [%[s]], %[stride]\n"
		"      ld1 { v0.h }[1], [%[s]], %[stride]\n"
		"      ld1 { v0.h }[2], [%[s]], %[stride]\n"
		"      ld1 { v0.h }[3], [%[s]], %[stride]\n"
		"      subs %[n_samples], %[n_samples], #4\n"
		"      sshll v1.4s, v0.4h, #0\n"
		"      scvtf v0.4s, v1.4s, #15\n"
		"      st1 { v0.4s }, [%[d]], #16\n"
		"      bne 1b\n"
		"2:"
		"      cmp %[remainder], #0\n"
		"      beq 4f\n"
		"3:"
		"      ld1 { v0.h }[0], [%[s]], %[stride]\n"
		"      subs %[remainder], %[remainder], #1\n"
		"      sshll v1.4s, v0.4h, #0\n"
		"      scvtf v0.4s, v1.4s, #15\n"
		"      st1 { v0.s }[0], [%[d]], #4\n"
		"      bne 3b\n"
		"4:"
		: [d] "+r" (d), [s] "+r" (s), [n_samples] "+r" (n_samples),
		  [remainder] "+r" (remainder)
		: [stride] "r" (stride)
		: "cc", "v0", "v1");
#else
	uint32_t n;
	for(n = 0; n < n_samples; n++) {
		*d++ = S16_TO_F32(s[0]);
		s += n_channels;
	}
#endif
}

void
conv_s16_to_f32d_neon(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int16_t *s = src[0];
	uint32_t i = 0, n_channels = conv->n_channels;

	for(; i + 1 < n_channels; i += 2)
		conv_s16_to_f32d_2s_neon(conv, &dst[i], &s[i], n_channels, n_samples);
	for(; i < n_channels; i++)
		conv_s16_to_f32d_1s_neon(conv, &dst[i], &s[i], n_channels, n_samples);
}

static void
conv_f32d_to_s16_2s_neon(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s0 = src[0], *s1 = src[1];
	int16_t *d = dst;

#ifdef __aarch64__
	uint32_t stride = n_channels << 1;
	uint32_t remainder = n_samples & 3;
	n_samples -= remainder;

	asm volatile(
		"      cmp %[n_samples], #0\n"
		"      beq 2f\n"
		"1:"
		"      ld1 { v0.4s }, [%[s0]], #16\n"
		"      ld1 { v1.4s }, [%[s1]], #16\n"
		"      subs %[n_samples], %[n_samples], #4\n"
		"      fcvtzs v0.4s, v0.4s, #31\n"
		"      fcvtzs v1.4s, v1.4s, #31\n"
		"      sqrshrn v0.4h, v0.4s, #16\n"
		"      sqrshrn v1.4h, v1.4s, #16\n"
		"      st2 { v0.h, v1.h }[0], [%[d]], %[stride]\n"
		"      st2 { v0.h, v1.h }[1], [%[d]], %[stride]\n"
		"      st2 { v0.h, v1.h }[2], [%[d]], %[stride]\n"
		"      st2 { v0.h, v1.h }[3], [%[d]], %[stride]\n"
		"      bne 1b\n"
		"2:"
		"      cmp %[remainder], #0\n"
		"      beq 4f\n"
		"3:"
		"      ld1 { v0.s }[0], [%[s0]], #4\n"
		"      ld1 { v2.s }[0], [%[s1]], #4\n"
		"      subs %[remainder], %[remainder], #1\n"
		"      fcvtzs v0.4s, v0.4s, #31\n"
		"      fcvtzs v1.4s, v1.4s, #31\n"
		"      sqrshrn v0.4h, v0.4s, #16\n"
		"      sqrshrn v1.4h, v1.4s, #16\n"
		"      st2 { v0.h, v1.h }[0], [%[d]], %[stride]\n"
		"      bne 3b\n"
		"4:"
		: [d] "+r" (d), [s0] "+r" (s0), [s1] "+r" (s1), [n_samples] "+r" (n_samples),
		  [remainder] "+r" (remainder)
		: [stride] "r" (stride)
		: "cc", "v0", "v1");
#else
	uint32_t n;
	for(n = 0; n < n_samples; n++) {
		d[0] = F32_TO_S16(s0[n]);
		d[1] = F32_TO_S16(s1[n]);
		d += n_channels;
	}
#endif
}

static void
conv_f32d_to_s16_1s_neon(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s = src[0];
	int16_t *d = dst;

#ifdef __aarch64__
	uint32_t stride = n_channels << 1;
	uint32_t remainder = n_samples & 3;
	n_samples -= remainder;

	asm volatile(
		"      cmp %[n_samples], #0\n"
		"      beq 2f\n"
		"1:"
		"      ld1 { v0.4s }, [%[s]], #16\n"
		"      subs %[n_samples], %[n_samples], #4\n"
		"      fcvtzs v0.4s, v0.4s, #31\n"
		"      sqrshrn v0.4h, v0.4s, #16\n"
		"      st1 { v0.h }[0], [%[d]], %[stride]\n"
		"      st1 { v0.h }[1], [%[d]], %[stride]\n"
		"      st1 { v0.h }[2], [%[d]], %[stride]\n"
		"      st1 { v0.h }[3], [%[d]], %[stride]\n"
		"      bne 1b\n"
		"2:"
		"      cmp %[remainder], #0\n"
		"      beq 4f\n"
		"3:"
		"      ld1 { v0.s }[0], [%[s]], #4\n"
		"      subs %[remainder], %[remainder], #1\n"
		"      fcvtzs v0.4s, v0.4s, #31\n"
		"      sqrshrn v0.4h, v0.4s, #16\n"
		"      st1 { v0.h }[0], [%[d]], %[stride]\n"
		"      bne 3b\n"
		"4:"
		: [d] "+r" (d), [s] "+r" (s), [n_samples] "+r" (n_samples),
		  [remainder] "+r" (remainder)
		: [stride] "r" (stride)
		: "cc", "v0");
#else
	uint32_t n;
	for(n = 0; n < n_samples; n++) {
		*d = F32_TO_S16(s0[n]);
		d += n_channels;
	}
#endif
}

void
conv_f32d_to_s16_neon(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	int16_t *d = dst[0];
	uint32_t i = 0, n_channels = conv->n_channels;

	for(; i + 1 < n_channels; i += 2)
		conv_f32d_to_s16_2s_neon(conv, &d[i], &src[i], n_channels, n_samples);
	for(; i < n_channels; i++)
		conv_f32d_to_s16_1s_neon(conv, &d[i], &src[i], n_channels, n_samples);
}
