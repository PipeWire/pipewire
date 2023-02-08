/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <spa/utils/defs.h>

#include "mix-ops.h"

#define MAKE_FUNC(name,type,atype,accum,clamp,zero)				\
void mix_ ##name## _c(struct mix_ops *ops,					\
		void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],	\
                uint32_t n_src, uint32_t n_samples)				\
{										\
	uint32_t i, n;								\
	type *d = dst;								\
	const type **s = (const type **)src;					\
	n_samples *= ops->n_channels;						\
	if (n_src == 0 && zero)							\
		memset(dst, 0, n_samples * sizeof(type));			\
	else if (n_src == 1) {							\
		if (dst != src[0])						\
			spa_memcpy(dst, src[0], n_samples * sizeof(type));	\
	} else {								\
		for (n = 0; n < n_samples; n++) {				\
			atype ac = 0;						\
			for (i = 0; i < n_src; i++)				\
				ac = accum (ac, s[i][n]);			\
			d[n] = clamp (ac);					\
		}								\
	}									\
}

MAKE_FUNC(s8, int8_t, int16_t, S8_ACCUM, S8_CLAMP, true);
MAKE_FUNC(u8, uint8_t, int16_t, U8_ACCUM, U8_CLAMP, false);
MAKE_FUNC(s16, int16_t, int32_t, S16_ACCUM, S16_CLAMP, true);
MAKE_FUNC(u16, uint16_t, int16_t, U16_ACCUM, U16_CLAMP, false);
MAKE_FUNC(s24, int24_t, int32_t, S24_ACCUM, S24_CLAMP, false);
MAKE_FUNC(u24, uint24_t, int32_t, U24_ACCUM, U24_CLAMP, false);
MAKE_FUNC(s32, int32_t, int64_t, S32_ACCUM, S32_CLAMP, true);
MAKE_FUNC(u32, uint32_t, int64_t, U32_ACCUM, U32_CLAMP, false);
MAKE_FUNC(s24_32, int32_t, int32_t, S24_32_ACCUM, S24_32_CLAMP, true);
MAKE_FUNC(u24_32, uint32_t, int32_t, U24_32_ACCUM, U24_32_CLAMP, false);
MAKE_FUNC(f32, float, float, F32_ACCUM, F32_CLAMP, true);
MAKE_FUNC(f64, double, double, F64_ACCUM, F64_CLAMP, true);
