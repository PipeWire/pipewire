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

#include <spa/utils/defs.h>

typedef void (*mix_clear_func_t) (void *dst, int n_bytes);
typedef void (*mix_func_t) (void *dst, const void *src, int n_bytes);
typedef void (*mix_scale_func_t) (void *dst, const void *src, const double scale, int n_bytes);
typedef void (*mix_i_func_t) (void *dst, int dst_stride,
			      const void *src, int src_stride, int n_bytes);
typedef void (*mix_scale_i_func_t) (void *dst, int dst_stride,
				    const void *src, int src_stride, const double scale, int n_bytes);

enum {
	FMT_S16,
	FMT_F32,
	FMT_MAX,
};

struct spa_audiomixer_ops {
	mix_clear_func_t clear[FMT_MAX];
	mix_func_t copy[FMT_MAX];
	mix_func_t add[FMT_MAX];
	mix_scale_func_t copy_scale[FMT_MAX];
	mix_scale_func_t add_scale[FMT_MAX];
	mix_i_func_t copy_i[FMT_MAX];
	mix_i_func_t add_i[FMT_MAX];
	mix_scale_i_func_t copy_scale_i[FMT_MAX];
	mix_scale_i_func_t add_scale_i[FMT_MAX];
};

void spa_audiomixer_get_ops(struct spa_audiomixer_ops *ops);
