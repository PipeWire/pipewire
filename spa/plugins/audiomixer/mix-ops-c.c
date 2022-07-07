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

#include <string.h>
#include <stdio.h>
#include <math.h>

#include <spa/utils/defs.h>

#include "mix-ops.h"

#define MAKE_FUNC(name,type,func) 						\
void mix_ ##name## _c(struct mix_ops *ops,					\
		void * SPA_RESTRICT dst, const void * SPA_RESTRICT src[],	\
                uint32_t n_src, uint32_t n_samples)				\
{										\
	uint32_t i, n;								\
	type *d = dst;								\
	n_samples *= ops->n_channels;						\
	if (n_src == 0)								\
		memset(dst, 0, n_samples * sizeof(type));			\
	else if (dst != src[0])							\
		spa_memcpy(dst, src[0], n_samples * sizeof(type));		\
	for (i = 1; i < n_src; i++) {						\
		const type *s = src[i];						\
		for (n = 0; n < n_samples; n++)					\
			d[n] = func (d[n], s[n]);				\
	}									\
}

MAKE_FUNC(s8, int8_t, S8_MIX);
MAKE_FUNC(u8, uint8_t, U8_MIX);
MAKE_FUNC(s16, int16_t, S16_MIX);
MAKE_FUNC(u16, uint16_t, U16_MIX);
MAKE_FUNC(s24, int24_t, S24_MIX);
MAKE_FUNC(u24, uint24_t, U24_MIX);
MAKE_FUNC(s32, int32_t, S32_MIX);
MAKE_FUNC(u32, uint32_t, U32_MIX);
MAKE_FUNC(s24_32, int32_t, S24_32_MIX);
MAKE_FUNC(u24_32, uint32_t, U24_32_MIX);
MAKE_FUNC(f32, float, F32_MIX);
MAKE_FUNC(f64, double, F64_MIX);
