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

#define VOLUME_MIN 0.0f
#define VOLUME_NORM 1.0f

#if defined (__SSE__)
#include "channelmix-ops-sse.c"
#endif

static void
channelmix_copy(void *data, int n_dst, void *dst[n_dst],
	   int n_src, const void *src[n_src], void *matrix, int n_bytes)
{
	int i, n, n_samples = n_bytes / sizeof(float);
	float **d = (float **)dst;
	float **s = (float **)src;
	float *m = matrix;
	float v = m[0];

	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		for (i = 0; i < n_dst; i++)
			memcpy(d[i], s[i], n_bytes);
	}
	else {
		for (i = 0; i < n_dst; i++)
			for (n = 0; n < n_samples; n++)
				d[i][n] = s[i][n] * v;
	}
}

static void
channelmix_f32_n_m(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, int n_bytes)
{
	int i, j, n, n_samples = n_bytes / sizeof(float);
	float **d = (float **) dst;
	float **s = (float **) src;
	float *m = matrix;

	for (n = 0; n < n_samples; n++) {
		for (i = 0; i < n_dst; i++) {
			float sum = 0.0f;

			for (j = 0; j < n_src; j++)
				sum += s[j][n] * m[i * n_src + j];

			d[i][n] = sum;
		}
	}
}

static void
channelmix_f32_1_2(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, int n_bytes)
{
	int n, n_samples = n_bytes / sizeof(float);
	float **d = (float **)dst;
	float **s = (float **)src;
	float *m = matrix;
	float v = m[0];

	if (v <= VOLUME_MIN) {
		memset(d[0], 0, n_bytes);
		memset(d[1], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		for (n = 0; n < n_samples; n++)
			d[0][n] = d[1][n] = s[0][n];
	}
	else {
		for (n = 0; n < n_samples; n++)
			d[0][n] = d[1][n] = s[0][n] * v;
	}
}

static void
channelmix_f32_2_1(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, int n_bytes)
{
	int n, n_samples = n_bytes / sizeof(float);
	float **d = (float **)dst;
	float **s = (float **)src;
	float *m = matrix;
	float v = m[0];

	if (v <= VOLUME_MIN) {
		memset(d[0], 0, n_bytes);
	}
	else {
		const float f = v * 0.5f;
		for (n = 0; n < n_samples; n++)
			d[0][n] = (s[0][n] + s[1][n]) * f;
	}
}

static void
channelmix_f32_2_4(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, int n_bytes)
{
	int i, n, n_samples = n_bytes / sizeof(float);
	float **d = (float **)dst;
	float **s = (float **)src;
	float *m = matrix;
	float v = m[0];

	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		for (n = 0; n < n_samples; n++) {
			d[0][n] = d[2][n] = s[0][n];
			d[1][n] = d[3][n] = s[1][n];
		}
	}
	else {
		for (n = 0; n < n_samples; n++) {
			d[0][n] = d[2][n] = s[0][n] * v;
			d[1][n] = d[3][n] = s[1][n] * v;
		}
	}
}

static void
channelmix_f32_2_3p1(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, int n_bytes)
{
	int i, n, n_samples = n_bytes / sizeof(float);
	float **d = (float **)dst;
	float **s = (float **)src;
	float *m = matrix;
	float v = m[0];

	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		for (n = 0; n < n_samples; n++) {
			d[0][n] = s[0][n];
			d[1][n] = s[1][n];
			d[2][n] = (s[0][n] + s[1][n]) * 0.5f;
			d[3][n] = 0.0f;
		}
	}
	else {
		const float f = 0.5f * v;
		for (n = 0; n < n_samples; n++) {
			d[0][n] = s[0][n] * v;
			d[1][n] = s[1][n] * v;
			d[2][n] = (s[0][n] + s[1][n]) * f;
			d[3][n] = 0.0f;
		}
	}
}

static void
channelmix_f32_2_5p1(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, int n_bytes)
{
	int i, n, n_samples = n_bytes / sizeof(float);
	float **d = (float **)dst;
	float **s = (float **)src;
	float *m = matrix;
	float v = m[0];

	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		for (n = 0; n < n_samples; n++) {
			d[0][n] = d[2][n] = s[0][n];
			d[1][n] = d[3][n] = s[1][n];
			d[4][n] = (s[0][n] + s[1][n]) * 0.5f;
			d[5][n] = 0.0f;
		}
	}
	else {
		const float f = 0.5f * v;
		for (n = 0; n < n_samples; n++) {
			d[0][n] = d[2][n] = s[0][n] * v;
			d[1][n] = d[3][n] = s[1][n] * v;
			d[4][n] = (s[0][n] + s[1][n]) * f;
			d[5][n] = 0.0f;
		}
	}
}

/* FL+FR+RL+RR+FC+LFE -> FL+FR */
static void
channelmix_f32_5p1_2(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, int n_bytes)
{
	int n, n_samples = n_bytes / sizeof(float);
	float **d = (float **) dst;
	float **s = (float **) src;
	float *m = matrix;
	float v = m[0];
	const float clev = 0.7071f;
	const float slev = 0.7071f;

	if (v <= VOLUME_MIN) {
		memset(d[0], 0, n_bytes);
		memset(d[1], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		for (n = 0; n < n_samples; n++) {
			const float ctr = clev * s[4][n];
			d[0][n] = s[0][n] + ctr + (slev * s[2][n]);
			d[1][n] = s[1][n] + ctr + (slev * s[3][n]);
		}
	}
	else {
		for (n = 0; n < n_samples; n++) {
			const float ctr = clev * s[4][n];
			d[0][n] = (s[0][n] + ctr + (slev * s[2][n])) * v;
			d[1][n] = (s[1][n] + ctr + (slev * s[3][n])) * v;
		}
	}
}

/* FL+FR+RL+RR+FC+LFE -> FL+FR+FC+LFE*/
static void
channelmix_f32_5p1_3p1(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, int n_bytes)
{
	int i, n, n_samples;
	float **d = (float **) dst;
	float **s = (float **) src;
	float *m = matrix;
	float v = m[0];

	n_samples = n_bytes / sizeof(float);
	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else {
		const float f1 = 0.5f * v;
		for (n = 0; n < n_samples; n++) {
			d[0][n] = (s[0][n] + s[2][n]) * f1;
			d[1][n] = (s[1][n] + s[3][n]) * f1;
			d[2][n] = s[4][n] * v;
			d[3][n] = s[5][n] * v;
		}
	}
}

/* FL+FR+RL+RR+FC+LFE -> FL+FR+RL+RR*/
static void
channelmix_f32_5p1_4(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, int n_bytes)
{
	int i, n, n_samples;
	float **d = (float **) dst;
	float **s = (float **) src;
	float *m = matrix;
	float v = m[0];

	n_samples = n_bytes / sizeof(float);
	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		for (n = 0; n < n_samples; n++) {
			float ctr = s[4][n] * 0.7071f;
			d[0][n] = s[0][n] + ctr;
			d[1][n] = s[1][n] + ctr;
			d[2][n] = s[2][n];
			d[3][n] = s[3][n];
		}
	}
	else {
		for (n = 0; n < n_samples; n++) {
			float ctr = s[4][n] * 0.7071f;
			d[0][n] = (s[0][n] + ctr) * v;
			d[1][n] = (s[1][n] + ctr) * v;
			d[2][n] = s[2][n] * v;
			d[3][n] = s[3][n] * v;
		}
	}
}

typedef void (*channelmix_func_t) (void *data, int n_dst, void *dst[n_dst],
				   int n_src, const void *src[n_src],
				   void *matrix, int n_bytes);


static const struct channelmix_info {
	uint32_t src_chan;
	uint64_t src_mask;
	uint32_t dst_chan;
	uint64_t dst_mask;

	channelmix_func_t func;
#define FEATURE_SSE	(1<<0)
	uint32_t features;
} channelmix_table[] =
{
#if defined (__SSE2__)
	{ -2, 0, -2, 0, channelmix_copy_sse, 0 },
#endif
	{ -2, 0, -2, 0, channelmix_copy, 0 },
	{ 1, 0, 2, 0, channelmix_f32_1_2, 0 },
	{ 2, 0, 1, 0, channelmix_f32_2_1, 0 },
#if defined (__SSE2__)
	{ 2, 0, 4, 0, channelmix_f32_2_4_sse, FEATURE_SSE },
#endif
	{ 2, 0, 4, 0, channelmix_f32_2_4, 0 },
	{ 2, 0, 4, 0, channelmix_f32_2_3p1, 0 },
	{ 2, 0, 6, 0, channelmix_f32_2_5p1, 0 },
#if defined (__SSE2__)
	{ 6, 0, 2, 0, channelmix_f32_5p1_2_sse, FEATURE_SSE },
#endif
	{ 6, 0, 2, 0, channelmix_f32_5p1_2, 0 },
#if defined (__SSE2__)
	{ 6, 0, 4, 0, channelmix_f32_5p1_4_sse, FEATURE_SSE },
#endif
	{ 6, 0, 4, 0, channelmix_f32_5p1_4, 0 },
	{ 6, 0, 4, 0, channelmix_f32_5p1_3p1, 0 },
	{ -1, 0, -1, 0, channelmix_f32_n_m, 0 },
};

#define MATCH_CHAN(a,b)	((a) == -1 || (a) == (b))

static const struct channelmix_info *find_channelmix_info(uint32_t src_chan, uint64_t src_mask,
		uint32_t dst_chan, uint64_t dst_mask, uint32_t features)
{
	int i;

	if (src_chan == dst_chan && src_mask == dst_mask)
		return &channelmix_table[0];

	for (i = 1; i < SPA_N_ELEMENTS(channelmix_table); i++) {
		if (MATCH_CHAN(channelmix_table[i].src_chan, src_chan) &&
		    MATCH_CHAN(channelmix_table[i].dst_chan, dst_chan) &&
		    (channelmix_table[i].features == 0 || (channelmix_table[i].features & features) != 0))
			return &channelmix_table[i];
	}
	return NULL;
}
