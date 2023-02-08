/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "channelmix-ops.h"

static inline void clear_c(float *d, uint32_t n_samples)
{
	memset(d, 0, n_samples * sizeof(float));
}

static inline void copy_c(float *d, const float *s, uint32_t n_samples)
{
	spa_memcpy(d, s, n_samples * sizeof(float));
}

static inline void vol_c(float *d, const float *s, float vol, uint32_t n_samples)
{
	uint32_t n;
	if (vol == 0.0f) {
		clear_c(d, n_samples);
	} else if (vol == 1.0f) {
		copy_c(d, s, n_samples);
	} else {
		for (n = 0; n < n_samples; n++)
			d[n] = s[n] * vol;
	}
}
static inline void conv_c(float *d, const float **s, float *c, uint32_t n_c, uint32_t n_samples)
{
	uint32_t n, j;
	for (n = 0; n < n_samples; n++) {
		float sum = 0.0f;
		for (j = 0; j < n_c; j++)
			sum += s[j][n] * c[j];
		d[n] = sum;
	}
}

static inline void avg_c(float *d, const float *s0, const float *s1, uint32_t n_samples)
{
	uint32_t n;
	for (n = 0; n < n_samples; n++)
		d[n] = (s0[n] + s1[n]) * 0.5f;
}

static inline void sub_c(float *d, const float *s0, const float *s1, uint32_t n_samples)
{
	uint32_t n;
	for (n = 0; n < n_samples; n++)
		d[n] = s0[n] - s1[n];
}

void
channelmix_copy_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n_dst = mix->dst_chan;
	float **d = (float **)dst;
	const float **s = (const float **)src;
	for (i = 0; i < n_dst; i++)
		vol_c(d[i], s[i], mix->matrix[i][i], n_samples);
}

#define _M(ch)		(1UL << SPA_AUDIO_CHANNEL_ ## ch)

void
channelmix_f32_n_m_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, j, n_dst = mix->dst_chan, n_src = mix->src_chan;
	float **d = (float **) dst;
	const float **s = (const float **) src;

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_c(d[i], n_samples);
	}
	else if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_COPY)) {
		uint32_t copy = SPA_MIN(n_dst, n_src);
		for (i = 0; i < copy; i++)
			copy_c(d[i], s[i], n_samples);
		for (; i < n_dst; i++)
			clear_c(d[i], n_samples);
	}
	else {
		for (i = 0; i < n_dst; i++) {
			float *di = d[i];
			float mj[n_src];
			const float *sj[n_src];
			uint32_t n_j = 0;

			for (j = 0; j < n_src; j++) {
				if (mix->matrix[i][j] == 0.0f)
					continue;
				mj[n_j] = mix->matrix[i][j];
				sj[n_j++] = s[j];
			}
			if (n_j == 0) {
				clear_c(di, n_samples);
			} else if (n_j == 1) {
				lr4_process(&mix->lr4[i], di, sj[0], mj[0], n_samples);
			} else {
				conv_c(di, sj, mj, n_j, n_samples);
				lr4_process(&mix->lr4[i], di, di, 1.0f, n_samples);
			}
		}
	}
}

#define MASK_MONO	_M(FC)|_M(MONO)|_M(UNKNOWN)
#define MASK_STEREO	_M(FL)|_M(FR)|_M(UNKNOWN)

void
channelmix_f32_1_2_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	float **d = (float **)dst;
	const float **s = (const float **)src;
	const float v0 = mix->matrix[0][0];
	const float v1 = mix->matrix[1][0];

	vol_c(d[0], s[0], v0, n_samples);
	vol_c(d[1], s[0], v1, n_samples);
}

void
channelmix_f32_2_1_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t n;
	float **d = (float **)dst;
	const float **s = (const float **)src;
	const float v0 = mix->matrix[0][0];
	const float v1 = mix->matrix[0][1];

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		clear_c(d[0], n_samples);
	} else if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_EQUAL)) {
		for (n = 0; n < n_samples; n++)
			d[0][n] = (s[0][n] + s[1][n]) * v0;
	}
	else {
		for (n = 0; n < n_samples; n++)
			d[0][n] = s[0][n] * v0 + s[1][n] * v1;
	}
}

void
channelmix_f32_4_1_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t n;
	float **d = (float **)dst;
	const float **s = (const float **)src;
	const float v0 = mix->matrix[0][0];
	const float v1 = mix->matrix[0][1];
	const float v2 = mix->matrix[0][2];
	const float v3 = mix->matrix[0][3];

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		clear_c(d[0], n_samples);
	}
	else if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_EQUAL)) {
		for (n = 0; n < n_samples; n++)
			d[0][n] = (s[0][n] + s[1][n] + s[2][n] + s[3][n]) * v0;
	}
	else {
		for (n = 0; n < n_samples; n++)
			d[0][n] = s[0][n] * v0 + s[1][n] * v1 +
				s[2][n] * v2 + s[3][n] * v3;
	}
}

#define MASK_QUAD	_M(FL)|_M(FR)|_M(RL)|_M(RR)|_M(UNKNOWN)

void
channelmix_f32_2_4_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n_dst = mix->dst_chan;
	float **d = (float **)dst;
	const float **s = (const float **)src;
	const float v0 = mix->matrix[0][0];
	const float v1 = mix->matrix[1][1];
	const float v2 = mix->matrix[2][0];
	const float v3 = mix->matrix[3][1];

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_c(d[i], n_samples);
	}
	else {
		vol_c(d[0], s[0], v0, n_samples);
		vol_c(d[1], s[1], v1, n_samples);
		if (mix->upmix != CHANNELMIX_UPMIX_PSD) {
			vol_c(d[2], s[0], v2, n_samples);
			vol_c(d[3], s[1], v3, n_samples);
		} else {
			sub_c(d[2], s[0], s[1], n_samples);

			delay_convolve_run(mix->buffer[1], &mix->pos[1], BUFFER_SIZE, mix->delay,
					   mix->taps, mix->n_taps, d[3], d[2], -v3, n_samples);
			delay_convolve_run(mix->buffer[0], &mix->pos[0], BUFFER_SIZE, mix->delay,
					   mix->taps, mix->n_taps, d[2], d[2], v2, n_samples);
		}
	}
}

#define MASK_3_1	_M(FL)|_M(FR)|_M(FC)|_M(LFE)
void
channelmix_f32_2_3p1_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n, n_dst = mix->dst_chan;
	float **d = (float **)dst;
	const float **s = (const float **)src;
	const float v0 = mix->matrix[0][0];
	const float v1 = mix->matrix[1][1];
	const float v2 = (mix->matrix[2][0] + mix->matrix[2][1]) * 0.5f;
	const float v3 = (mix->matrix[3][0] + mix->matrix[3][1]) * 0.5f;

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_c(d[i], n_samples);
	}
	else {
		if (mix->widen == 0.0f) {
			vol_c(d[0], s[0], v0, n_samples);
			vol_c(d[1], s[1], v1, n_samples);
			avg_c(d[2], s[0], s[1], n_samples);
		} else {
			for (n = 0; n < n_samples; n++) {
				float c = s[0][n] + s[1][n];
				float w = c * mix->widen;
				d[0][n] = (s[0][n] - w) * v0;
				d[1][n] = (s[1][n] - w) * v1;
				d[2][n] = c * 0.5f;
			}
		}
		lr4_process(&mix->lr4[3], d[3], d[2], v3, n_samples);
		lr4_process(&mix->lr4[2], d[2], d[2], v2, n_samples);
	}
}

#define MASK_5_1	_M(FL)|_M(FR)|_M(FC)|_M(LFE)|_M(SL)|_M(SR)|_M(RL)|_M(RR)
void
channelmix_f32_2_5p1_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n_dst = mix->dst_chan;
	float **d = (float **)dst;
	const float **s = (const float **)src;
	const float v4 = mix->matrix[4][0];
	const float v5 = mix->matrix[5][1];

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_c(d[i], n_samples);
	}
	else {
		channelmix_f32_2_3p1_c(mix, dst, src, n_samples);

		if (mix->upmix != CHANNELMIX_UPMIX_PSD) {
			vol_c(d[4], s[0], v4, n_samples);
			vol_c(d[5], s[1], v5, n_samples);
		} else {
			sub_c(d[4], s[0], s[1], n_samples);

			delay_convolve_run(mix->buffer[1], &mix->pos[1], BUFFER_SIZE, mix->delay,
					mix->taps, mix->n_taps, d[5], d[4], -v5, n_samples);
			delay_convolve_run(mix->buffer[0], &mix->pos[0], BUFFER_SIZE, mix->delay,
					mix->taps, mix->n_taps, d[4], d[4], v4, n_samples);
		}
	}
}

void
channelmix_f32_2_7p1_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n_dst = mix->dst_chan;
	float **d = (float **)dst;
	const float **s = (const float **)src;
	const float v4 = mix->matrix[4][0];
	const float v5 = mix->matrix[5][1];
	const float v6 = mix->matrix[6][0];
	const float v7 = mix->matrix[7][1];

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_c(d[i], n_samples);
	}
	else {
		channelmix_f32_2_3p1_c(mix, dst, src, n_samples);

		vol_c(d[4], s[0], v4, n_samples);
		vol_c(d[5], s[1], v5, n_samples);

		if (mix->upmix != CHANNELMIX_UPMIX_PSD) {
			vol_c(d[6], s[0], v6, n_samples);
			vol_c(d[7], s[1], v7, n_samples);
		} else {
			sub_c(d[6], s[0], s[1], n_samples);

			delay_convolve_run(mix->buffer[1], &mix->pos[1], BUFFER_SIZE, mix->delay,
					mix->taps, mix->n_taps, d[7], d[6], -v7, n_samples);
			delay_convolve_run(mix->buffer[0], &mix->pos[0], BUFFER_SIZE, mix->delay,
					mix->taps, mix->n_taps, d[6], d[6], v6, n_samples);
		}
	}
}

/* FL+FR+FC+LFE -> FL+FR */
void
channelmix_f32_3p1_2_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t n;
	float **d = (float **) dst;
	const float **s = (const float **) src;
	const float v0 = mix->matrix[0][0];
	const float v1 = mix->matrix[1][1];
	const float clev = (mix->matrix[0][2] + mix->matrix[1][2]) * 0.5f;
	const float llev = (mix->matrix[0][3] + mix->matrix[1][3]) * 0.5f;

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		clear_c(d[0], n_samples);
		clear_c(d[1], n_samples);
	}
	else {
		for (n = 0; n < n_samples; n++) {
			const float ctr = clev * s[2][n] + llev * s[3][n];
			d[0][n] = s[0][n] * v0 + ctr;
			d[1][n] = s[1][n] * v1 + ctr;
		}
	}
}

/* FL+FR+FC+LFE+SL+SR -> FL+FR */
void
channelmix_f32_5p1_2_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t n;
	float **d = (float **) dst;
	const float **s = (const float **) src;
	const float v0 = mix->matrix[0][0];
	const float v1 = mix->matrix[1][1];
	const float clev = (mix->matrix[0][2] + mix->matrix[1][2]) * 0.5f;
	const float llev = (mix->matrix[0][3] + mix->matrix[1][3]) * 0.5f;
	const float slev0 = mix->matrix[0][4];
	const float slev1 = mix->matrix[1][5];

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		clear_c(d[0], n_samples);
		clear_c(d[1], n_samples);
	}
	else {
		for (n = 0; n < n_samples; n++) {
			const float ctr = clev * s[2][n] + llev * s[3][n];
			d[0][n] = s[0][n] * v0 + ctr + (slev0 * s[4][n]);
			d[1][n] = s[1][n] * v1 + ctr + (slev1 * s[5][n]);
		}
	}
}

/* FL+FR+FC+LFE+SL+SR -> FL+FR+FC+LFE*/
void
channelmix_f32_5p1_3p1_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n, n_dst = mix->dst_chan;
	float **d = (float **) dst;
	const float **s = (const float **) src;
	const float v0 = mix->matrix[0][0];
	const float v1 = mix->matrix[1][1];
	const float v2 = mix->matrix[2][2];
	const float v3 = mix->matrix[3][3];
	const float v4 = mix->matrix[0][4];
	const float v5 = mix->matrix[1][5];

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_c(d[i], n_samples);
	}
	else {
		for (n = 0; n < n_samples; n++) {
			d[0][n] = s[0][n] * v0 + s[4][n] * v4;
			d[1][n] = s[1][n] * v1 + s[5][n] * v5;
		}
		vol_c(d[2], s[2], v2, n_samples);
		vol_c(d[3], s[3], v3, n_samples);
	}
}

/* FL+FR+FC+LFE+SL+SR -> FL+FR+RL+RR*/
void
channelmix_f32_5p1_4_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n_dst = mix->dst_chan;
	float **d = (float **) dst;
	const float **s = (const float **) src;
	const float v4 = mix->matrix[2][4];
	const float v5 = mix->matrix[3][5];

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_c(d[i], n_samples);
	}
	else {
		channelmix_f32_3p1_2_c(mix, dst, src, n_samples);

		vol_c(d[2], s[4], v4, n_samples);
		vol_c(d[3], s[5], v5, n_samples);
	}
}

#define MASK_7_1	_M(FL)|_M(FR)|_M(FC)|_M(LFE)|_M(SL)|_M(SR)|_M(RL)|_M(RR)

/* FL+FR+FC+LFE+SL+SR+RL+RR -> FL+FR */
void
channelmix_f32_7p1_2_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t n;
	float **d = (float **) dst;
	const float **s = (const float **) src;
	const float v0 = mix->matrix[0][0];
	const float v1 = mix->matrix[1][1];
	const float clev = (mix->matrix[0][2] + mix->matrix[1][2]) * 0.5f;
	const float llev = (mix->matrix[0][3] + mix->matrix[1][3]) * 0.5f;
	const float slev0 = mix->matrix[0][4];
	const float slev1 = mix->matrix[1][5];
	const float rlev0 = mix->matrix[0][6];
	const float rlev1 = mix->matrix[1][7];

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		clear_c(d[0], n_samples);
		clear_c(d[1], n_samples);
	}
	else {
		for (n = 0; n < n_samples; n++) {
			const float ctr = clev * s[2][n] + llev * s[3][n];
			d[0][n] = s[0][n] * v0 + ctr + s[4][n] * slev0 + s[6][n] * rlev0;
			d[1][n] = s[1][n] * v1 + ctr + s[5][n] * slev1 + s[7][n] * rlev1;
		}
	}
}

/* FL+FR+FC+LFE+SL+SR+RL+RR -> FL+FR+FC+LFE*/
void
channelmix_f32_7p1_3p1_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n, n_dst = mix->dst_chan;
	float **d = (float **) dst;
	const float **s = (const float **) src;
	const float v0 = mix->matrix[0][0];
	const float v1 = mix->matrix[1][1];
	const float v2 = mix->matrix[2][2];
	const float v3 = mix->matrix[3][3];
	const float v4 = (mix->matrix[0][4] + mix->matrix[0][6]) * 0.5f;
	const float v5 = (mix->matrix[1][5] + mix->matrix[1][7]) * 0.5f;

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_c(d[i], n_samples);
	}
	else {
		for (n = 0; n < n_samples; n++) {
			d[0][n] = s[0][n] * v0 + (s[4][n] + s[6][n]) * v4;
			d[1][n] = s[1][n] * v1 + (s[5][n] + s[7][n]) * v5;
		}
		vol_c(d[2], s[2], v2, n_samples);
		vol_c(d[3], s[3], v3, n_samples);
	}
}

/* FL+FR+FC+LFE+SL+SR+RL+RR -> FL+FR+RL+RR*/
void
channelmix_f32_7p1_4_c(struct channelmix *mix, void * SPA_RESTRICT dst[],
		   const void * SPA_RESTRICT src[], uint32_t n_samples)
{
	uint32_t i, n, n_dst = mix->dst_chan;
	float **d = (float **) dst;
	const float **s = (const float **) src;
	const float v0 = mix->matrix[0][0];
	const float v1 = mix->matrix[1][1];
	const float clev = (mix->matrix[0][2] + mix->matrix[1][2]) * 0.5f;
	const float llev = (mix->matrix[0][3] + mix->matrix[1][3]) * 0.5f;
	const float slev0 = mix->matrix[2][4];
	const float slev1 = mix->matrix[3][5];
	const float rlev0 = mix->matrix[2][6];
	const float rlev1 = mix->matrix[3][7];

	if (SPA_FLAG_IS_SET(mix->flags, CHANNELMIX_FLAG_ZERO)) {
		for (i = 0; i < n_dst; i++)
			clear_c(d[i], n_samples);
	}
	else {
		for (n = 0; n < n_samples; n++) {
			const float ctr = s[2][n] * clev + s[3][n] * llev;
			const float sl = s[4][n] * slev0;
			const float sr = s[5][n] * slev1;
			d[0][n] = s[0][n] * v0 + ctr + sl;
			d[1][n] = s[1][n] * v1 + ctr + sr;
			d[2][n] = s[6][n] * rlev0 + sl;
			d[3][n] = s[7][n] * rlev1 + sr;
		}
	}
}
