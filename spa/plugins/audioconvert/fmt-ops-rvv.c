/* Spa */
/* SPDX-FileCopyrightText: Copyright (c) 2023 Institue of Software Chinese Academy of Sciences (ISCAS). */
/* SPDX-License-Identifier: MIT */

#include "fmt-ops.h"

#if HAVE_RVV
void
conv_f32_to_s16_rvv(struct convert *conv, void * SPA_RESTRICT dst[], const void * SPA_RESTRICT src[],
		uint32_t n_samples)
{
	if (n_samples * conv->n_channels <= 4) {
		conv_f32_to_s16_c(conv, dst, src, n_samples);
		return;
	}

	__asm__ __volatile__ (
		".option       arch, +v                       \n\t"
		"ld            a1, (a1)                       \n\t"
		"ld            a2, (a2)                       \n\t"
		"lwu           a0, 16(a0)                     \n\t"
		"li            t0, 1191182336                 \n\t"
		"mul           a0, a0, a3                     \n\t"
		"fmv.w.x       fa5, t0                        \n\t"
		"1:                                           \n\t"
		"vsetvli       t0, a0, e32, m8, ta, ma        \n\t"
		"vle32.v       v8, (a2)                       \n\t"
		"sub           a0, a0, t0                     \n\t"
		"vfmul.vf      v8, v8, fa5                    \n\t"
		"vsetvli       zero, zero, e16, m4, ta, ma    \n\t"
		"vfncvt.x.f.w  v8, v8                         \n\t"
		"slli          t0, t0, 1                      \n\t"
		"vse16.v       v8, (a1)                       \n\t"
		"add           a2, a2, t0                     \n\t"
		"add           a1, a1, t0                     \n\t"
		"add           a2, a2, t0                     \n\t"
		"bnez          a0, 1b                         \n\t"
		:
		:
		: "cc", "memory"
	);
}
#endif
