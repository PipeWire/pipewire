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

typedef void (*mix_func_t) (void *dst, const void *src, int n_bytes);
typedef void (*mix_scale_func_t) (void *dst, const void *src, const void *scale, int n_bytes);
typedef void (*mix_i_func_t) (void *dst, int dst_stride,
			      const void *src, int src_stride, int n_bytes);
typedef void (*mix_scale_i_func_t) (void *dst, int dst_stride,
				    const void *src, int src_stride, const void *scale, int n_bytes);

enum {
	CONV_S16_S16,
	CONV_F32_F32,
	CONV_MAX,
};

struct spa_audiomixer_ops {
	mix_func_t copy[CONV_MAX];
	mix_func_t add[CONV_MAX];
	mix_scale_func_t copy_scale[CONV_MAX];
	mix_scale_func_t add_scale[CONV_MAX];
	mix_i_func_t copy_i[CONV_MAX];
	mix_i_func_t add_i[CONV_MAX];
	mix_scale_i_func_t copy_scale_i[CONV_MAX];
	mix_scale_i_func_t add_scale_i[CONV_MAX];
};

void spa_audiomixer_get_ops(struct spa_audiomixer_ops *ops);
