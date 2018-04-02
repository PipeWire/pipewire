/* Spa
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#define U8_TO_F32(v)	(((v) / 128.0) - 1.0)

#define F32_TO_U8(v)		\
({				\
	typeof(v) _v = (v);	\
	_v < -1.0f ? 0 :	\
	_v >= 1.0f ? 255 :	\
	(_v * 127.0f) + 128.0f;	\
})

#define S16_TO_F32(v)	((v) / 32767.0)

#define F32_TO_S16(v)		\
({				\
	typeof(v) _v = (v);	\
	_v < -1.0f ? -32767 :	\
	_v >= 1.0f ? 32767 :	\
	_v * 32767.0f;		\
})

static void
conv_s16_to_f32(void *dst, const void *src, int n_bytes)
{
	const int16_t *s = src;
	float *d = dst;

	n_bytes /= sizeof(int16_t);
	while (n_bytes--)
		*d++ = S16_TO_F32(*s++);
}

static void
conv_s16_to_f32d(int n_dst, void *dst[n_dst], const void *src, int n_bytes)
{
	const int16_t *s = src;
	float **d = (float **) dst;
	int i;

	n_bytes /= (sizeof(int16_t) * n_dst);
	while (n_bytes--)
		for (i = 0; i < n_dst; i++)
			*d[i]++ = S16_TO_F32(*s++);
}

static void
conv_f32d_to_s16(void *dst, int n_src, const void *src[n_src], int n_bytes)
{
	const int16_t **s = (const int16_t **) src;
	float *d = dst;
	int i;

	n_bytes /= (sizeof(float) * n_src);
	while (n_bytes--)
		for (i = 0; i < n_src; i++)
			*d++ = F32_TO_S16(*s[i]++);
}

static void
conv_f32_to_s16(void *dst, const void *src, int n_bytes)
{
	const float *s = src;
	int16_t *d = dst;

	n_bytes /= sizeof(float);
	while (n_bytes--)
		*d++ = F32_TO_S16(*s++);
}


static void
conv_u8_to_f32(void *dst, const void *src, int n_bytes)
{
	const int8_t *s = src;
	float *d = dst;

	while (n_bytes--)
		*d++ = U8_TO_F32(*s++);
}

static void
conv_u8_to_f32d(int n_dst, void *dst[n_dst], const void *src, int n_bytes)
{
	const int8_t *s = src;
	float **d = (float **) dst;
	int i;

	n_bytes /= n_dst;
	while (n_bytes--)
		for (i = 0; i < n_dst; i++)
			*d[i]++ = U8_TO_F32(*s++);
}

static void
conv_f32d_to_u8(void *dst, int n_src, const void *src[n_src], int n_bytes)
{
	const int8_t **s = (const int8_t **) src;
	float *d = dst;
	int i;

	n_bytes /= (sizeof(float) * n_src);
	while (n_bytes--)
		for (i = 0; i < n_src; i++)
			*d++ = F32_TO_U8(*s[i]++);
}

static void
conv_f32_to_u8(void *dst, const void *src, int n_bytes)
{
	const float *s = src;
	int8_t *d = dst;

	n_bytes /= sizeof(float);
	while (n_bytes--)
		*d++ = F32_TO_U8(*s++);
}

static void
deinterleave_8(int n_dst, void *dst[n_dst], const void *src, int n_bytes)
{
	const uint8_t *s = src;
	uint8_t **d = (uint8_t **) dst;
	int i;

	n_bytes /= n_dst;
	while (n_bytes--) {
		for (i = 0; i < n_dst; i++)
			*d[i]++ = *s++;
	}
}

static void
deinterleave_16(int n_dst, void *dst[n_dst], const void *src, int n_bytes)
{
	const uint16_t *s = src;
	uint16_t **d = (uint16_t **) dst;
	int i;

	n_bytes /= (sizeof(uint16_t) * n_dst);
	while (n_bytes--) {
		for (i = 0; i < n_dst; i++)
			*d[i]++ = *s++;
	}
}

static void
deinterleave_32(int n_dst, void *dst[n_dst], const void *src, int n_bytes)
{
	const uint32_t *s = src;
	uint32_t **d = (uint32_t **) dst;
	int i;

	n_bytes /= (sizeof(uint32_t) * n_dst);
	while (n_bytes--) {
		for (i = 0; i < n_dst; i++)
			*d[i]++ = *s++;
	}
}

static void
interleave_32(void *dst, int n_src, const void *src[n_src], int n_bytes)
{
	const int32_t **s = (const int32_t **) src;
	uint32_t *d = dst;
	int i;

	n_bytes /= (sizeof(uint32_t) * n_src);
	while (n_bytes--) {
		for (i = 0; i < n_src; i++)
			*d++ = *s[i]++;
	}
}
