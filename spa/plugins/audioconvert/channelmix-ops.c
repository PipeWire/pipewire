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

#include <spa/support/cpu.h>
#include <spa/utils/defs.h>

#define VOLUME_MIN 0.0f
#define VOLUME_NORM 1.0f

#if defined (__SSE__)
#include "channelmix-ops-sse.c"
#endif

static void
channelmix_copy(void *data, int n_dst, void *dst[n_dst],
	   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int i, n, n_samples = n_bytes / sizeof(float);
	float **d = (float **)dst;
	float **s = (float **)src;

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

#define _M(ch)		(1UL << SPA_AUDIO_CHANNEL_ ## ch)

static void
channelmix_f32_n_m(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int i, j, n, n_samples = n_bytes / sizeof(float);
	float **d = (float **) dst;
	float **s = (float **) src;
	float *m = matrix;

	for (n = 0; n < n_samples; n++) {
		for (i = 0; i < n_dst; i++) {
			float sum = 0.0f;
			for (j = 0; j < n_src; j++)
				sum += s[j][n] * m[i * n_src + j] * v;
			d[i][n] = sum;
		}
	}
}

#define MASK_MONO	_M(FC)|_M(MONO)|_M(UNKNOWN)
#define MASK_STEREO	_M(FL)|_M(FR)|_M(UNKNOWN)

static void
channelmix_f32_1_2(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int n, n_samples = n_bytes / sizeof(float);
	float **d = (float **)dst;
	float **s = (float **)src;

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
		   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int n, n_samples = n_bytes / sizeof(float);
	float **d = (float **)dst;
	float **s = (float **)src;

	if (v <= VOLUME_MIN) {
		memset(d[0], 0, n_bytes);
	}
	else {
		const float f = v * 0.5f;
		for (n = 0; n < n_samples; n++)
			d[0][n] = (s[0][n] + s[1][n]) * f;
	}
}


#define MASK_QUAD	_M(FL)|_M(FR)|_M(RL)|_M(RR)|_M(UNKNOWN)

static void
channelmix_f32_2_4(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int i, n, n_samples = n_bytes / sizeof(float);
	float **d = (float **)dst;
	float **s = (float **)src;

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

#define MASK_3_1	_M(FL)|_M(FR)|_M(FC)|_M(LFE)
static void
channelmix_f32_2_3p1(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int i, n, n_samples = n_bytes / sizeof(float);
	float **d = (float **)dst;
	float **s = (float **)src;

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

#define MASK_5_1	_M(FL)|_M(FR)|_M(FC)|_M(LFE)|_M(SL)|_M(SR)|_M(RL)|_M(RR)
static void
channelmix_f32_2_5p1(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int i, n, n_samples = n_bytes / sizeof(float);
	float **d = (float **)dst;
	float **s = (float **)src;

	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		for (n = 0; n < n_samples; n++) {
			d[0][n] = d[4][n] = s[0][n];
			d[1][n] = d[5][n] = s[1][n];
			d[2][n] = (s[0][n] + s[1][n]) * 0.5f;
			d[3][n] = 0.0f;
		}
	}
	else {
		const float f = 0.5f * v;
		for (n = 0; n < n_samples; n++) {
			d[0][n] = d[4][n] = s[0][n] * v;
			d[1][n] = d[5][n] = s[1][n] * v;
			d[2][n] = (s[0][n] + s[1][n]) * f;
			d[3][n] = 0.0f;
		}
	}
}

/* FL+FR+FC+LFE+SL+SR -> FL+FR */
static void
channelmix_f32_5p1_2(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int n, n_samples = n_bytes / sizeof(float);
	float **d = (float **) dst;
	float **s = (float **) src;
	float *m = matrix;
	const float clev = m[2];
	const float llev = m[3];
	const float slev = m[4];

	if (v <= VOLUME_MIN) {
		memset(d[0], 0, n_bytes);
		memset(d[1], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		for (n = 0; n < n_samples; n++) {
			const float ctr = clev * s[2][n] + llev * s[3][n];
			d[0][n] = s[0][n] + ctr + (slev * s[4][n]);
			d[1][n] = s[1][n] + ctr + (slev * s[5][n]);
		}
	}
	else {
		for (n = 0; n < n_samples; n++) {
			const float ctr = clev * s[2][n] + llev * s[3][n];
			d[0][n] = (s[0][n] + ctr + (slev * s[4][n])) * v;
			d[1][n] = (s[1][n] + ctr + (slev * s[5][n])) * v;
		}
	}
}

/* FL+FR+FC+LFE+SL+SR -> FL+FR+FC+LFE*/
static void
channelmix_f32_5p1_3p1(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int i, n, n_samples;
	float **d = (float **) dst;
	float **s = (float **) src;

	n_samples = n_bytes / sizeof(float);
	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else {
		const float f1 = 0.5f * v;
		for (n = 0; n < n_samples; n++) {
			d[0][n] = (s[0][n] + s[4][n]) * f1;
			d[1][n] = (s[1][n] + s[5][n]) * f1;
			d[2][n] = s[2][n] * v;
			d[3][n] = s[3][n] * v;
		}
	}
}

/* FL+FR+FC+LFE+SL+SR -> FL+FR+RL+RR*/
static void
channelmix_f32_5p1_4(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int i, n, n_samples;
	float **d = (float **) dst;
	float **s = (float **) src;
	float *m = matrix;
	const float clev = m[2];
	const float llev = m[3];

	n_samples = n_bytes / sizeof(float);
	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		for (n = 0; n < n_samples; n++) {
			float ctr = s[2][n] * clev + s[3][n] * llev;
			d[0][n] = s[0][n] + ctr;
			d[1][n] = s[1][n] + ctr;
			d[2][n] = s[4][n];
			d[3][n] = s[5][n];
		}
	}
	else {
		for (n = 0; n < n_samples; n++) {
			float ctr = s[2][n] * clev + s[3][n] * llev;
			d[0][n] = (s[0][n] + ctr) * v;
			d[1][n] = (s[1][n] + ctr) * v;
			d[2][n] = s[4][n] * v;
			d[3][n] = s[5][n] * v;
		}
	}
}

#define MASK_7_1	_M(FL)|_M(FR)|_M(FC)|_M(LFE)|_M(SL)|_M(SR)|_M(RL)|_M(RR)

/* FL+FR+FC+LFE+SL+SR+RL+RR -> FL+FR */
static void
channelmix_f32_7p1_2(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int n, n_samples = n_bytes / sizeof(float);
	float **d = (float **) dst;
	float **s = (float **) src;
	float *m = matrix;
	const float clev = m[2];
	const float llev = m[3];
	const float slev = m[4];

	if (v <= VOLUME_MIN) {
		memset(d[0], 0, n_bytes);
		memset(d[1], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		for (n = 0; n < n_samples; n++) {
			const float ctr = clev * s[2][n] + llev * s[3][n];
			d[0][n] = s[0][n] + ctr + (slev * (s[4][n] + s[6][n]));
			d[1][n] = s[1][n] + ctr + (slev * (s[5][n] + s[7][n]));
		}
	}
	else {
		for (n = 0; n < n_samples; n++) {
			const float ctr = clev * s[2][n] + llev * s[3][n];
			d[0][n] = (s[0][n] + ctr + (slev * (s[4][n] + s[6][n]))) * v;
			d[1][n] = (s[1][n] + ctr + (slev * (s[5][n] + s[6][n]))) * v;
		}
	}
}

/* FL+FR+FC+LFE+SL+SR+RL+RR -> FL+FR+FC+LFE*/
static void
channelmix_f32_7p1_3p1(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int i, n, n_samples;
	float **d = (float **) dst;
	float **s = (float **) src;

	n_samples = n_bytes / sizeof(float);
	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else {
		const float f1 = 0.5 * v;
		for (n = 0; n < n_samples; n++) {
			d[0][n] = s[0][n] + (s[4][n] + s[6][n]) * f1;
			d[1][n] = s[1][n] + (s[5][n] + s[7][n]) * f1;
			d[2][n] = s[2][n] * v;
			d[3][n] = s[3][n] * v;
		}
	}
}

/* FL+FR+FC+LFE+SL+SR+RL+RR -> FL+FR+RL+RR*/
static void
channelmix_f32_7p1_4(void *data, int n_dst, void *dst[n_dst],
		   int n_src, const void *src[n_src], void *matrix, float v, int n_bytes)
{
	int i, n, n_samples;
	float **d = (float **) dst;
	float **s = (float **) src;
	float *m = matrix;
	const float clev = m[2];
	const float llev = m[3];
	const float slev = m[4];

	n_samples = n_bytes / sizeof(float);
	if (v <= VOLUME_MIN) {
		for (i = 0; i < n_dst; i++)
			memset(d[i], 0, n_bytes);
	}
	else if (v == VOLUME_NORM) {
		for (n = 0; n < n_samples; n++) {
			float ctr = s[2][n] * clev + s[3][n] * llev;
			float sl = s[4][n] * slev;
			float sr = s[5][n] * slev;
			d[0][n] = s[0][n] + ctr + sl;
			d[1][n] = s[1][n] + ctr + sr;
			d[2][n] = s[6][n] + sl;
			d[3][n] = s[7][n] + sr;
		}
	}
	else {
		for (n = 0; n < n_samples; n++) {
			float ctr = s[2][n] * clev + s[3][n] * llev;
			float sl = s[4][n] * slev;
			float sr = s[5][n] * slev;
			d[0][n] = (s[0][n] + ctr + sl) * v;
			d[1][n] = (s[1][n] + ctr + sr) * v;
			d[2][n] = (s[6][n] + sl) * v;
			d[3][n] = (s[7][n] + sr) * v;
		}
	}
}

typedef void (*channelmix_func_t) (void *data, int n_dst, void *dst[n_dst],
				   int n_src, const void *src[n_src],
				   void *matrix, float v, int n_bytes);


static const struct channelmix_info {
	uint32_t src_chan;
	uint64_t src_mask;
	uint32_t dst_chan;
	uint64_t dst_mask;

	channelmix_func_t func;
#define FEATURE_SSE	SPA_CPU_FLAG_SSE
	uint32_t features;
} channelmix_table[] =
{
#if defined (__SSE__)
	{ 2, MASK_MONO, 2, MASK_MONO, channelmix_copy_sse, FEATURE_SSE },
	{ 2, MASK_STEREO, 2, MASK_STEREO, channelmix_copy_sse, FEATURE_SSE },
	{ -2, 0, -2, 0, channelmix_copy_sse, FEATURE_SSE },
#endif
	{ 2, MASK_MONO, 2, MASK_MONO, channelmix_copy, 0 },
	{ 2, MASK_STEREO, 2, MASK_STEREO, channelmix_copy, 0 },
	{ -2, 0, -2, 0, channelmix_copy, 0 },

	{ 1, MASK_MONO, 2, MASK_STEREO, channelmix_f32_1_2, 0 },
	{ 2, MASK_STEREO, 1, MASK_MONO, channelmix_f32_2_1, 0 },
#if defined (__SSE__)
	{ 2, MASK_STEREO, 4, MASK_QUAD, channelmix_f32_2_4_sse, FEATURE_SSE },
#endif
	{ 2, MASK_STEREO, 4, MASK_QUAD, channelmix_f32_2_4, 0 },
	{ 2, MASK_STEREO, 4, MASK_3_1, channelmix_f32_2_3p1, 0 },
	{ 2, MASK_STEREO, 6, MASK_5_1, channelmix_f32_2_5p1, 0 },
#if defined (__SSE__)
	{ 6, MASK_5_1, 2, MASK_STEREO, channelmix_f32_5p1_2_sse, FEATURE_SSE },
#endif
	{ 6, MASK_5_1, 2, MASK_STEREO, channelmix_f32_5p1_2, 0 },
#if defined (__SSE__)
	{ 6, MASK_5_1, 4, MASK_QUAD, channelmix_f32_5p1_4_sse, FEATURE_SSE },
#endif
	{ 6, MASK_5_1, 4, MASK_QUAD, channelmix_f32_5p1_4, 0 },

#if defined (__SSE__)
	{ 6, MASK_5_1, 4, MASK_3_1, channelmix_f32_5p1_3p1_sse, FEATURE_SSE },
#endif
	{ 6, MASK_5_1, 4, MASK_3_1, channelmix_f32_5p1_3p1, 0 },

	{ 8, MASK_7_1, 2, MASK_STEREO, channelmix_f32_7p1_2, 0 },
	{ 8, MASK_7_1, 4, MASK_QUAD, channelmix_f32_7p1_4, 0 },
	{ 8, MASK_7_1, 4, MASK_3_1, channelmix_f32_7p1_3p1, 0 },

	{ -1, 0, -1, 0, channelmix_f32_n_m, 0 },
};

#define MATCH_CHAN(a,b)		((a) == -1 || (a) == (b))
#define MATCH_FEATURES(a,b)	((a) == 0 || ((a) & (b)) != 0)
#define MATCH_MASK(a,b)		((a) == 0 || ((a) & (b)) == (b))

static const struct channelmix_info *find_channelmix_info(uint32_t src_chan, uint64_t src_mask,
		uint32_t dst_chan, uint64_t dst_mask, uint32_t features)
{
	int i;
	for (i = 0; i < SPA_N_ELEMENTS(channelmix_table); i++) {
		if (!MATCH_FEATURES(channelmix_table[i].features, features))
			continue;

		if (src_chan == dst_chan && src_mask == dst_mask)
			return &channelmix_table[i];

		if (MATCH_CHAN(channelmix_table[i].src_chan, src_chan) &&
		    MATCH_CHAN(channelmix_table[i].dst_chan, dst_chan) &&
		    MATCH_MASK(channelmix_table[i].src_mask, src_mask) &&
		    MATCH_MASK(channelmix_table[i].dst_mask, dst_mask))
			return &channelmix_table[i];
	}
	return NULL;
}
