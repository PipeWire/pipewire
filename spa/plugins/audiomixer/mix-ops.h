/* Spa
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
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
