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
#endif
