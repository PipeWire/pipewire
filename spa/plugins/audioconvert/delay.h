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
	uint32_t w = *pos;
	uint32_t o = n_buffer - delay;

	for (i = 0; i < n_samples; i++) {
		buffer[w] = buffer[w + n_buffer] = src[i];
		dst[i] = buffer[w + o] * vol;
		w = w + 1 >= n_buffer ? 0 : w + 1;
	}
	*pos = w;
}

static inline void delay_convolve_run(float *buffer, uint32_t *pos,
		uint32_t n_buffer, uint32_t delay,
		const float *taps, uint32_t n_taps,
		float *dst, const float *src, const float vol, uint32_t n_samples)
{
	uint32_t i, j;
	uint32_t w = *pos;
	uint32_t o = n_buffer - delay - n_taps-1;

	if (n_taps == 1) {
		delay_run(buffer, pos, n_buffer, delay, dst, src, vol, n_samples);
		return;
	}
	for (i = 0; i < n_samples; i++) {
		float sum = 0.0f;

		buffer[w] = buffer[w + n_buffer] = src[i];
		for (j = 0; j < n_taps; j++)
			sum += taps[j] * buffer[w+o+j];
		dst[i] = sum * vol;

		w = w + 1 >= n_buffer ? 0 : w + 1;
	}
	*pos = w;
}

#ifdef __cplusplus
}
#endif

#endif /* DELAY_H */
