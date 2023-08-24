/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <arm_neon.h>

#include "fmt-ops.h"

void
conv_s16_to_f32d_2_neon(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	const int16_t *s = src[0];
	float *d0 = dst[0], *d1 = dst[1];
	unsigned int remainder = n_samples & 7;
	n_samples -= remainder;

#ifdef __aarch64__
	asm volatile(
		"      cmp %[n_samples], #0\n"
		"      beq 2f\n"
		"1:"
		"      ld2 {v2.8h, v3.8h}, [%[s]], #32\n"
		"      subs %w[n_samples], %w[n_samples], #8\n"
		"      sxtl v0.4s, v2.4h\n"
		"      sxtl2 v1.4s, v2.8h\n"
		"      sxtl v2.4s, v3.4h\n"
		"      sxtl2 v3.4s, v3.8h\n"
		"      scvtf v0.4s, v0.4s, #15\n"
		"      scvtf v1.4s, v1.4s, #15\n"
		"      scvtf v2.4s, v2.4s, #15\n"
		"      scvtf v3.4s, v3.4s, #15\n"
		"      st1 {v0.4s, v1.4s}, [%[d0]], #32\n"
		"      st1 {v2.4s, v3.4s}, [%[d1]], #32\n"
		"      b.ne 1b\n"
		"2:"
		"      cmp %[remainder], #0\n"
		"      beq 4f\n"
		"3:"
		"      ld2 { v0.h, v1.h }[0], [%[s]], #4\n"
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
		: : "v0", "v1", "v2", "v3", "memory", "cc");
#else
	asm volatile(
		"      cmp %[n_samples], #0\n"
		"      beq 2f\n"
		"1:"
		"      vld2.16 {d0-d3}, [%[s]]!\n"
		"      subs %[n_samples], #8\n"
		"      vmovl.s16 q3, d3\n"
		"      vmovl.s16 q2, d2\n"
		"      vmovl.s16 q1, d1\n"
		"      vmovl.s16 q0, d0\n"
		"      vcvt.f32.s32 q3, q3, #15\n"
		"      vcvt.f32.s32 q2, q2, #15\n"
		"      vcvt.f32.s32 q1, q1, #15\n"
		"      vcvt.f32.s32 q0, q0, #15\n"
		"      vst1.32 {d4-d7}, [%[d1]]!\n"
		"      vst1.32 {d0-d3}, [%[d0]]!\n"
		"      bne 1b\n"
		"2:"
		"      cmp %[remainder], #0\n"
		"      beq 4f\n"
		"3:"
		"      vld2.16 { d0[0], d1[0] }, [%[s]]!\n"
		"      subs %[remainder], %[remainder], #1\n"
		"      vmovl.s16 q1, d1\n"
		"      vmovl.s16 q0, d0\n"
		"      vcvt.f32.s32 q1, q1, #15\n"
		"      vcvt.f32.s32 q0, q0, #15\n"
		"      vst1.32 { d2[0] }, [%[d1]]!\n"
		"      vst1.32 { d0[0] }, [%[d0]]!\n"
		"      bne 3b\n"
		"4:"
		: [d0] "+r" (d0), [d1] "+r" (d1), [s] "+r" (s), [n_samples] "+r" (n_samples),
		  [remainder] "+r" (remainder)
		: : "q0", "q1", "q2", "q3", "memory", "cc");
#endif
}

static void
conv_s16_to_f32d_2s_neon(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	const int16_t *s = src;
	float *d0 = dst[0], *d1 = dst[1];
	uint32_t stride = n_channels << 1;
	unsigned int remainder = n_samples & 3;
	n_samples -= remainder;

#ifdef __aarch64__
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
	asm volatile(
		"      cmp %[n_samples], #0\n"
		"      beq 2f\n"
		"1:"
		"      vld2.16 { d0[0], d1[0] }, [%[s]], %[stride]\n"
		"      vld2.16 { d0[1], d1[1] }, [%[s]], %[stride]\n"
		"      vld2.16 { d0[2], d1[2] }, [%[s]], %[stride]\n"
		"      vld2.16 { d0[3], d1[3] }, [%[s]], %[stride]\n"
		"      subs %[n_samples], %[n_samples], #4\n"
		"      vmovl.s16 q1, d1\n"
		"      vmovl.s16 q0, d0\n"
		"      vcvt.f32.s32 q0, q0, #15\n"
		"      vcvt.f32.s32 q1, q1, #15\n"
		"      vst1.32 { q0 }, [%[d0]]!\n"
		"      vst1.32 { q1 }, [%[d1]]!\n"
		"      bne 1b\n"
		"2:"
		"      cmp %[remainder], #0\n"
		"      beq 4f\n"
		"3:"
		"      vld2.16 { d0[0], d1[0] }, [%[s]], %[stride]\n"
		"      subs %[remainder], %[remainder], #1\n"
		"      vmovl.s16 q1, d1\n"
		"      vmovl.s16 q0, d0\n"
		"      vcvt.f32.s32 q0, q0, #15\n"
		"      vcvt.f32.s32 q1, q1, #15\n"
		"      vst1.32 { d0[0] }, [%[d0]]!\n"
		"      vst1.32 { d2[0] }, [%[d1]]!\n"
		"      bne 3b\n"
		"4:"
		: [d0] "+r" (d0), [d1] "+r" (d1), [s] "+r" (s), [n_samples] "+r" (n_samples),
		  [remainder] "+r" (remainder)
		: [stride] "r" (stride)
		: "cc", "q0", "q1");
#endif
}

static void
conv_s16_to_f32d_1s_neon(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	const int16_t *s = src;
	float *d = dst[0];
	uint32_t stride = n_channels << 1;
	uint32_t remainder = n_samples & 3;
	n_samples -= remainder;

#ifdef __aarch64__
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
	asm volatile(
		"      cmp %[n_samples], #0\n"
		"      beq 2f\n"
		"1:"
		"      vld1.16 { d0[0] }, [%[s]], %[stride]\n"
		"      vld1.16 { d0[1] }, [%[s]], %[stride]\n"
		"      vld1.16 { d0[2] }, [%[s]], %[stride]\n"
		"      vld1.16 { d0[3] }, [%[s]], %[stride]\n"
		"      subs %[n_samples], %[n_samples], #4\n"
		"      vmovl.s16 q0, d0\n"
		"      vcvt.f32.s32 q0, q0, #15\n"
		"      vst1.32 { q0 }, [%[d]]!\n"
		"      bne 1b\n"
		"2:"
		"      cmp %[remainder], #0\n"
		"      beq 4f\n"
		"3:"
		"      vld1.16 { d0[0] }, [%[s]], %[stride]\n"
		"      subs %[remainder], %[remainder], #1\n"
		"      vmovl.s16 q0, d0\n"
		"      vcvt.f32.s32 q0, q0, #15\n"
		"      vst1.32 { d0[0] }, [%[d]]!\n"
		"      bne 3b\n"
		"4:"
		: [d] "+r" (d), [s] "+r" (s), [n_samples] "+r" (n_samples),
		  [remainder] "+r" (remainder)
		: [stride] "r" (stride)
		: "cc", "q0");
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
	uint32_t stride = n_channels << 1;
	uint32_t remainder = n_samples & 3;
	n_samples -= remainder;

#ifdef __aarch64__
	asm volatile(
		"      dup v2.4s, %w[scale]\n"
		"      cmp %[n_samples], #0\n"
		"      beq 2f\n"
		"1:"
		"      ld1 { v0.4s }, [%[s0]], #16\n"
		"      ld1 { v1.4s }, [%[s1]], #16\n"
		"      subs %[n_samples], %[n_samples], #4\n"
		"      sqadd  v0.4s, v0.4s, v2.4s\n"
		"      sqadd  v1.4s, v1.4s, v2.4s\n"
		"      fcvtns v0.4s, v0.4s\n"
		"      fcvtns v1.4s, v1.4s\n"
		"      sqxtn  v0.4h, v0.4s\n"
		"      sqxtn  v1.4h, v1.4s\n"
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
		"      ld1 { v1.s }[0], [%[s1]], #4\n"
		"      subs %[remainder], %[remainder], #1\n"
		"      sqadd  v0.4s, v0.4s, v2.4s\n"
		"      sqadd  v1.4s, v1.4s, v2.4s\n"
		"      fcvtns v0.4s, v0.4s\n"
		"      fcvtns v1.4s, v1.4s\n"
		"      sqxtn  v0.4h, v0.4s\n"
		"      sqxtn  v1.4h, v1.4s\n"
		"      st2 { v0.h, v1.h }[0], [%[d]], %[stride]\n"
		"      bne 3b\n"
		"4:"
		: [d] "+r" (d), [s0] "+r" (s0), [s1] "+r" (s1), [n_samples] "+r" (n_samples),
		  [remainder] "+r" (remainder)
		: [stride] "r" (stride),
		  [scale] "r" (15 << 23)
		: "cc", "v0", "v1");
#else
	float32x4_t pos = vdupq_n_f32(0.4999999f / S16_SCALE);
	float32x4_t neg = vdupq_n_f32(-0.4999999f / S16_SCALE);

	asm volatile(
		"      veor q2, q2, q2\n"
		"      cmp %[n_samples], #0\n"
		"      beq 2f\n"
		"1:"
		"      vld1.32 { q0 }, [%[s0]]!\n"
		"      vld1.32 { q1 }, [%[s1]]!\n"
		"      subs %[n_samples], %[n_samples], #4\n"
		"      vcgt.f32 q3, q0, q2\n"
		"      vcgt.f32 q4, q0, q2\n"
		"      vbsl q3, %q[pos], %q[neg]\n"
		"      vbsl q4, %q[pos], %q[neg]\n"
		"      vadd.f32 q0, q0, q3\n"
		"      vadd.f32 q1, q1, q4\n"
		"      vcvt.s32.f32 q0, q0, #15\n"
		"      vcvt.s32.f32 q1, q1, #15\n"
		"      vqmovn.s32 d0, q0\n"
		"      vqmovn.s32 d1, q1\n"
		"      vst2.16 { d0[0], d1[0] }, [%[d]], %[stride]\n"
		"      vst2.16 { d0[1], d1[1] }, [%[d]], %[stride]\n"
		"      vst2.16 { d0[2], d1[2] }, [%[d]], %[stride]\n"
		"      vst2.16 { d0[3], d1[3] }, [%[d]], %[stride]\n"
		"      bne 1b\n"
		"2:"
		"      cmp %[remainder], #0\n"
		"      beq 4f\n"
		"3:"
		"      vld1.32 { d0[0] }, [%[s0]]!\n"
		"      vld1.32 { d2[0] }, [%[s1]]!\n"
		"      subs %[remainder], %[remainder], #1\n"
		"      vcgt.f32 q3, q0, q2\n"
		"      vcgt.f32 q4, q0, q2\n"
		"      vbsl q3, %q[pos], %q[neg]\n"
		"      vbsl q4, %q[pos], %q[neg]\n"
		"      vadd.f32 q0, q0, q3\n"
		"      vadd.f32 q1, q1, q4\n"
		"      vcvt.s32.f32 q0, q0, #15\n"
		"      vcvt.s32.f32 q1, q1, #15\n"
		"      vqmovn.s32 d0, q0\n"
		"      vqmovn.s32 d1, q1\n"
		"      vst2.16 { d0[0], d1[0] }, [%[d]], %[stride]\n"
		"      bne 3b\n"
		"4:"
		: [d] "+r" (d), [s0] "+r" (s0), [s1] "+r" (s1), [n_samples] "+r" (n_samples),
		  [remainder] "+r" (remainder)
		: [stride] "r" (stride),
		  [pos]"w"(pos),
		  [neg]"w"(neg)
		: "cc", "q0", "q1", "q2", "q3", "q4");
#endif
}

static void
conv_f32d_to_s16_1s_neon(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s = src[0];
	int16_t *d = dst;
	uint32_t stride = n_channels << 1;
	uint32_t remainder = n_samples & 3;
	n_samples -= remainder;

#ifdef __aarch64__
	asm volatile(
		"      dup v2.4s, %w[scale]\n"
		"      cmp %[n_samples], #0\n"
		"      beq 2f\n"
		"1:"
		"      ld1 { v0.4s }, [%[s]], #16\n"
		"      subs %[n_samples], %[n_samples], #4\n"
		"      sqadd  v0.4s, v0.4s, v2.4s\n"
		"      fcvtns v0.4s, v0.4s\n"
		"      sqxtn  v0.4h, v0.4s\n"
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
		"      sqadd  v0.4s, v0.4s, v2.4s\n"
		"      fcvtns v0.4s, v0.4s\n"
		"      sqxtn  v0.4h, v0.4s\n"
		"      st1 { v0.h }[0], [%[d]], %[stride]\n"
		"      bne 3b\n"
		"4:"
		: [d] "+r" (d), [s] "+r" (s), [n_samples] "+r" (n_samples),
		  [remainder] "+r" (remainder)
		: [stride] "r" (stride),
		  [scale] "r" (15 << 23)
		: "cc", "v0");
#else
	float32x4_t pos = vdupq_n_f32(0.4999999f / S16_SCALE);
	float32x4_t neg = vdupq_n_f32(-0.4999999f / S16_SCALE);

	asm volatile(
		"      veor q1, q1, q1\n"
		"      cmp %[n_samples], #0\n"
		"      beq 2f\n"
		"1:"
		"      vld1.32 { q0 }, [%[s]]!\n"
		"      subs %[n_samples], %[n_samples], #4\n"
		"      vcgt.f32 q2, q0, q1\n"
		"      vbsl q2, %q[pos], %q[neg]\n"
		"      vadd.f32 q0, q0, q2\n"
		"      vcvt.s32.f32 q0, q0, #15\n"
		"      vqmovn.s32 d0, q0\n"
		"      vst1.16 { d0[0] }, [%[d]], %[stride]\n"
		"      vst1.16 { d0[1] }, [%[d]], %[stride]\n"
		"      vst1.16 { d0[2] }, [%[d]], %[stride]\n"
		"      vst1.16 { d0[3] }, [%[d]], %[stride]\n"
		"      bne 1b\n"
		"2:"
		"      cmp %[remainder], #0\n"
		"      beq 4f\n"
		"3:"
		"      vld1.32 { d0[0] }, [%[s]]!\n"
		"      subs %[remainder], %[remainder], #1\n"
		"      vcgt.f32 q2, q0, q1\n"
		"      vbsl q2, %q[pos], %q[neg]\n"
		"      vadd.f32 q0, q0, q2\n"
		"      vcvt.s32.f32 q0, q0, #15\n"
		"      vqmovn.s32 d0, q0\n"
		"      vst1.16 { d0[0] }, [%[d]], %[stride]\n"
		"      bne 3b\n"
		"4:"
		: [d] "+r" (d), [s] "+r" (s), [n_samples] "+r" (n_samples),
		  [remainder] "+r" (remainder)
		: [stride] "r" (stride),
		  [pos]"w"(pos),
		  [neg]"w"(neg)
		: "cc", "q0", "q1", "q2");
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
