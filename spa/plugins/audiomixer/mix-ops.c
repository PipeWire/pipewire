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

#include "mix-ops.h"

static void
clear_s16(void *dst, int n_bytes)
{
	memset(dst, 0, n_bytes);
}

static void
clear_f32(void *dst, int n_bytes)
{
	memset(dst, 0, n_bytes);
}

static void
copy_s16(void *dst, const void *src, int n_bytes)
{
	memcpy(dst, src, n_bytes);
}

static void
copy_f32(void *dst, const void *src, int n_bytes)
{
	memcpy(dst, src, n_bytes);
}

static void
add_s16(void *dst, const void *src, int n_bytes)
{
	const int16_t *s = src;
	int16_t *d = dst;
	int32_t t;

	n_bytes /= sizeof(int16_t);
	while (n_bytes--) {
		t = *d + *s;
		*d = SPA_CLAMP(t, INT16_MIN, INT16_MAX);
		d++;
		s++;
	}
}

static void
add_f32(void *dst, const void *src, int n_bytes)
{
	const float *s = src;
	float *d = dst;

	n_bytes /= sizeof(float);
	while (n_bytes--) {
		*d += *s;
		d++;
		s++;
	}
}

static void
copy_scale_s16(void *dst, const void *src, const double scale, int n_bytes)
{
	const int16_t *s = src;
	int16_t *d = dst;;
	int32_t v = scale * (1 << 11), t;

	n_bytes /= sizeof(int16_t);
	while (n_bytes--) {
		t = (*s * v) >> 11;
		*d = SPA_CLAMP(t, INT16_MIN, INT16_MAX);
		d++;
		s++;
	}
}

static void
copy_scale_f32(void *dst, const void *src, const double scale, int n_bytes)
{
	const float *s = src;
	float *d = dst;
	float v = scale;

	n_bytes /= sizeof(float);
	while (n_bytes--) {
		*d = *s * v;
		d++;
		s++;
	}
}

static void
add_scale_s16(void *dst, const void *src, const double scale, int n_bytes)
{
	const int16_t *s = src;
	int16_t *d = dst;
	int32_t v = scale * (1 << 11), t;

	n_bytes /= sizeof(int16_t);
	while (n_bytes--) {
		t = *d + ((*s * v) >> 11);
		*d = SPA_CLAMP(t, INT16_MIN, INT16_MAX);
		d++;
		s++;
	}
}

static void
add_scale_f32(void *dst, const void *src, const double scale, int n_bytes)
{
	const float *s = src;
	float *d = dst;
	float v = scale;

	n_bytes /= sizeof(float);
	while (n_bytes--) {
		*d += *s * v;
		d++;
		s++;
	}
}

static void
copy_s16_i(void *dst, int dst_stride, const void *src, int src_stride, int n_bytes)
{
	const int16_t *s = src;
	int16_t *d = dst;

	n_bytes /= sizeof(int16_t);
	while (n_bytes--) {
		*d = *s;
		d += dst_stride;
		s += src_stride;
	}
}

static void
copy_f32_i(void *dst, int dst_stride, const void *src, int src_stride, int n_bytes)
{
	const float *s = src;
	float *d = dst;

	n_bytes /= sizeof(float);
	while (n_bytes--) {
		*d = *s;
		d += dst_stride;
		s += src_stride;
	}
}

static void
add_s16_i(void *dst, int dst_stride, const void *src, int src_stride, int n_bytes)
{
	const int16_t *s = src;
	int16_t *d = dst;
	int32_t t;

	n_bytes /= sizeof(int16_t);
	while (n_bytes--) {
		t = *d + *s;
		*d = SPA_CLAMP(t, INT16_MIN, INT16_MAX);
		d += dst_stride;
		s += src_stride;
	}
}

static void
add_f32_i(void *dst, int dst_stride, const void *src, int src_stride, int n_bytes)
{
	const float *s = src;
	float *d = dst;

	n_bytes /= sizeof(float);
	while (n_bytes--) {
		*d += *s;
		d += dst_stride;
		s += src_stride;
	}
}

static void
copy_scale_s16_i(void *dst, int dst_stride, const void *src, int src_stride, const double scale, int n_bytes)
{
	const int16_t *s = src;
	int16_t *d = dst;
	int32_t v = scale * (1 << 11), t;

	n_bytes /= sizeof(int16_t);
	while (n_bytes--) {
		t = (*s * v) >> 11;
		*d = SPA_CLAMP(t, INT16_MIN, INT16_MAX);
		d += dst_stride;
		s += src_stride;
	}
}

static void
copy_scale_f32_i(void *dst, int dst_stride, const void *src, int src_stride, const double scale, int n_bytes)
{
	const float *s = src;
	float *d = dst;
	float v = scale;

	n_bytes /= sizeof(float);
	while (n_bytes--) {
		*d = *s * v;
		d += dst_stride;
		s += src_stride;
	}
}

static void
add_scale_s16_i(void *dst, int dst_stride, const void *src, int src_stride, const double scale, int n_bytes)
{
	const int16_t *s = src;
	int16_t *d = dst;
	int32_t v = scale * (1 << 11), t;

	n_bytes /= sizeof(int16_t);
	while (n_bytes--) {
		t = *d + ((*s * v) >> 11);
		*d = SPA_CLAMP(t, INT16_MIN, INT16_MAX);
		d += dst_stride;
		s += src_stride;
	}
}

static void
add_scale_f32_i(void *dst, int dst_stride, const void *src, int src_stride, const double scale, int n_bytes)
{
	const float *s = src;
	float *d = dst;
	float v = scale;

	n_bytes /= sizeof(float);
	while (n_bytes--) {
		*d += *s * v;
		d += dst_stride;
		s += src_stride;
	}
}

void spa_audiomixer_get_ops(struct spa_audiomixer_ops *ops)
{
	ops->clear[FMT_S16] = clear_s16;
	ops->clear[FMT_F32] = clear_f32;
	ops->copy[FMT_S16] = copy_s16;
	ops->copy[FMT_F32] = copy_f32;
        ops->add[FMT_S16] = add_s16;
        ops->add[FMT_F32] = add_f32;
        ops->copy_scale[FMT_S16] = copy_scale_s16;
        ops->copy_scale[FMT_F32] = copy_scale_f32;
        ops->add_scale[FMT_S16] = add_scale_s16;
        ops->add_scale[FMT_F32] = add_scale_f32;
        ops->copy_i[FMT_S16] = copy_s16_i;
        ops->copy_i[FMT_F32] = copy_f32_i;
        ops->add_i[FMT_S16] = add_s16_i;
        ops->add_i[FMT_F32] = add_f32_i;
        ops->copy_scale_i[FMT_S16] = copy_scale_s16_i;
        ops->copy_scale_i[FMT_F32] = copy_scale_f32_i;
        ops->add_scale_i[FMT_S16] = add_scale_s16_i;
        ops->add_scale_i[FMT_F32] = add_scale_f32_i;
}
