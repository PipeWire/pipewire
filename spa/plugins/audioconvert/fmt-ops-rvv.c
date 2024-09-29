/* Spa */
/* SPDX-FileCopyrightText: Copyright (c) 2023 Institue of Software Chinese Academy of Sciences (ISCAS). */
/* SPDX-License-Identifier: MIT */

#include "fmt-ops.h"

#if HAVE_RVV
void
f32_to_s16(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_samples)
{
	asm __volatile__ (
		".option       arch, +v                                 \n\t"
		"li            t0, 1191182336                           \n\t"
		"fmv.w.x       fa5, t0                                  \n\t"
		"1:                                                     \n\t"
		"vsetvli       t0, %[n_samples], e32, m8, ta, ma        \n\t"
		"vle32.v       v8, (%[src])                             \n\t"
		"sub           %[n_samples], %[n_samples], t0           \n\t"
		"vfmul.vf      v8, v8, fa5                              \n\t"
		"vsetvli       zero, zero, e16, m4, ta, ma              \n\t"
		"vfncvt.x.f.w  v8, v8                                   \n\t"
		"slli          t0, t0, 1                                \n\t"
		"vse16.v       v8, (%[dst])                             \n\t"
		"add           %[src], %[src], t0                       \n\t"
		"add           %[dst], %[dst], t0                       \n\t"
		"add           %[src], %[src], t0                       \n\t"
		"bnez          %[n_samples], 1b                         \n\t"
		: [n_samples] "+r" (n_samples),
		  [src] "+r" (src),
		  [dst] "+r" (dst)
		:
		: "cc", "memory"
	);
}

void
conv_f32_to_s16_rvv(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	if (n_samples * conv->n_channels <= 4) {
		conv_f32_to_s16_c(conv, dst, src, n_samples);
		return;
	}

	f32_to_s16(conv, *dst, *src, n_samples * conv -> n_channels);
}

void
conv_f32d_to_s16d_rvv(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	if (n_samples <= 4) {
		conv_f32d_to_s16d_c(conv, dst, src, n_samples);
		return;
	}

	uint32_t i = 0, n_channels = conv->n_channels;
	for(i = 0; i < n_channels; i++) {
		f32_to_s16(conv, dst[i], src[i], n_samples);
	}
}

static void
f32d_to_s16(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s = src[0];
	uint32_t stride = n_channels << 1;

	asm __volatile__ (
		".option       arch, +v                                 \n\t"
		"li            t0, 1191182336                           \n\t"
		"fmv.w.x       fa5, t0                                  \n\t"
		"1:                                                     \n\t"
		"vsetvli       t0, %[n_samples], e32, m8, ta, ma        \n\t"
		"vle32.v       v8, (%[s])                               \n\t"
		"sub           %[n_samples], %[n_samples], t0           \n\t"
		"vfmul.vf      v8, v8, fa5                              \n\t"
		"vsetvli       zero, zero, e16, m4, ta, ma              \n\t"
		"vfncvt.x.f.w  v8, v8                                   \n\t"
		"slli          t2, t0, 2                                \n\t"
		"mul           t3, t0, %[stride]                        \n\t"
		"vsse16.v      v8, (%[dst]), %[stride]                  \n\t"
		"add           %[s], %[s], t2                           \n\t"
		"add           %[dst], %[dst], t3                       \n\t"
		"bnez          %[n_samples], 1b                         \n\t"
		: [n_samples] "+r" (n_samples),
		  [s] "+r" (s),
		  [dst] "+r" (dst)
		: [stride] "r" (stride)
		: "cc", "memory"
	);
}

void
conv_f32d_to_s16_rvv(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	if (n_samples <= 4) {
		conv_f32d_to_s16_c(conv, dst, src, n_samples);
		return;
	}

	int16_t *d = dst[0];
	uint32_t i = 0, n_channels = conv->n_channels;

	for(i = 0; i < n_channels; i++)
		f32d_to_s16(conv, &d[i], &src[i], n_channels, n_samples);
}

static void
s16_to_f32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	float *d = dst[0];
	uint32_t stride = n_channels << 1;

	asm __volatile__ (
		".option       arch, +v                                 \n\t"
		"li            t0, 939524096                            \n\t"
		"fmv.w.x       fa5, t0                                  \n\t"
		"1:                                                     \n\t"
		"vsetvli       t0, %[n_samples], e16, m4, ta, ma        \n\t"
		"vlse16.v      v8, (%[src]), %[stride]                  \n\t"
		"sub           %[n_samples], %[n_samples], t0           \n\t"
		"vfwcvt.f.x.v  v16, v8                                  \n\t"
		"vsetvli       zero, zero, e32, m8, ta, ma              \n\t"
		"mul           t4, t0, %[stride]                        \n\t"
		"vfmul.vf      v8, v16, fa5                             \n\t"
		"slli          t3, t0, 2                                \n\t"
		"vse32.v       v8, (%[d])                               \n\t"
		"add           %[src], %[src], t4                       \n\t"
		"add           %[d], %[d], t3                           \n\t"
		"bnez          %[n_samples], 1b                         \n\t"
		: [n_samples] "+r" (n_samples),
		  [src] "+r" (src),
		  [d] "+r" (d)
		: [stride] "r" (stride)
		: "cc", "memory"
	);

}

void
conv_s16_to_f32d_rvv(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	if (n_samples <= 4) {
		conv_s16_to_f32d_c(conv, dst, src, n_samples);
		return;
	}

	const int16_t *s = src[0];
	uint32_t i = 0, n_channels = conv->n_channels;

	for(i = 0; i < n_channels; i++)
		s16_to_f32d(conv, &dst[i], &s[i], n_channels, n_samples);
}

static void
s32_to_f32d(void *data, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src,
		uint32_t n_channels, uint32_t n_samples)
{
	float *d = dst[0];
	uint32_t stride = n_channels << 2;

	asm __volatile__ (
		".option       arch, +v                                 \n\t"
		"li            t0, 805306368                            \n\t"
		"fmv.w.x       fa5, t0                                  \n\t"
		"1:                                                     \n\t"
		"vsetvli       t0, %[n_samples], e32, m8, ta, ma        \n\t"
		"vlse32.v      v8, (%[src]), %[stride]                  \n\t"
		"sub           %[n_samples], %[n_samples], t0           \n\t"
		"vfcvt.f.x.v   v8, v8                                   \n\t"
		"mul           t4, t0, %[stride]                        \n\t"
		"vfmul.vf      v8, v8, fa5                              \n\t"
		"slli          t3, t0, 2                                \n\t"
		"vse32.v       v8, (%[d])                               \n\t"
		"add           %[src], %[src], t4                       \n\t"
		"add           %[d], %[d], t3                           \n\t"
		"bnez          %[n_samples], 1b                         \n\t"
		: [n_samples] "+r" (n_samples),
		  [src] "+r" (src),
		  [d] "+r" (d)
		: [stride] "r" (stride)
		: "cc", "memory"
	);

}

void
conv_s32_to_f32d_rvv(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	if (n_samples <= 4) {
		conv_s32_to_f32d_c(conv, dst, src, n_samples);
		return;
	}

	const int32_t *s = src[0];
	uint32_t i = 0, n_channels = conv->n_channels;

	for(i = 0; i < n_channels; i++)
		s32_to_f32d(conv, &dst[i], &s[i], n_channels, n_samples);
	return;
}

static void
f32d_to_s32(void *data, void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],
		uint32_t n_channels, uint32_t n_samples)
{
	const float *s = src[0];
	uint32_t stride = n_channels << 2;

	asm __volatile__ (
		".option       arch, +v                                 \n\t"
		"li            t0, 1325400064                           \n\t"
		"li            t2, 1325400063                           \n\t"
		"fmv.w.x       fa5, t0                                  \n\t"
		"fmv.w.x       fa4, t2                                  \n\t"
		"1:                                                     \n\t"
		"vsetvli       t0, %[n_samples], e32, m8, ta, ma        \n\t"
		"vle32.v       v8, (%[s])                               \n\t"
		"sub           %[n_samples], %[n_samples], t0           \n\t"
		"vfmul.vf      v8, v8, fa5                              \n\t"
		"vfmin.vf      v8, v8, fa4                              \n\t"
		"vfcvt.x.f.v   v8, v8                                   \n\t"
		"slli          t2, t0, 2                                \n\t"
		"mul           t3, t0, %[stride]                        \n\t"
		"vsse32.v      v8, (%[dst]), %[stride]                  \n\t"
		"add           %[s], %[s], t2                           \n\t"
		"add           %[dst], %[dst], t3                       \n\t"
		"bnez          %[n_samples], 1b                         \n\t"
		: [n_samples] "+r" (n_samples),
		  [s] "+r" (s),
		  [dst] "+r" (dst)
		: [stride] "r" (stride)
		: "cc", "memory"
	);
}

void
conv_f32d_to_s32_rvv(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	if (n_samples <= 4) {
		conv_f32d_to_s32_c(conv, dst, src, n_samples);
		return;
	}

	int32_t *d = dst[0];
	uint32_t i = 0, n_channels = conv->n_channels;

	for(i = 0; i < n_channels; i++)
		f32d_to_s32(conv, &d[i], &src[i], n_channels, n_samples);
}
#endif
