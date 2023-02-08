/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef DELAY_H
#define DELAY_H

#ifdef __cplusplus
extern "C" {
#endif

static inline void delay_run(float *buffer, uint32_t *pos,
		uint32_t n_buffer, uint32_t delay,
		float *dst, const float *src, const float vol, uint32_t n_samples)
{
	uint32_t i;
	uint32_t p = *pos;

	for (i = 0; i < n_samples; i++) {
		buffer[p] = src[i];
		dst[i] = buffer[(p - delay) & (n_buffer-1)] * vol;
		p = (p + 1) & (n_buffer-1);
	}
	*pos = p;
}

static inline void delay_convolve_run(float *buffer, uint32_t *pos,
		uint32_t n_buffer, uint32_t delay,
		const float *taps, uint32_t n_taps,
		float *dst, const float *src, const float vol, uint32_t n_samples)
{
	uint32_t i, j;
	uint32_t p = *pos;

	for (i = 0; i < n_samples; i++) {
		float sum = 0.0f;

		buffer[p] = src[i];
		for (j = 0; j < n_taps; j++)
			sum += (taps[j] * buffer[((p - delay) - j) & (n_buffer-1)]);
		dst[i] = sum * vol;

		p = (p + 1) & (n_buffer-1);
	}
	*pos = p;
}

#ifdef __cplusplus
}
#endif

#endif /* DELAY_H */
