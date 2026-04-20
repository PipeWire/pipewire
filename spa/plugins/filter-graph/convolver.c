/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2017 HiFi-LoFi */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */
/* Adapted from https://github.com/HiFi-LoFi/FFTConvolver */

#include "convolver.h"

#include <math.h>
#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <limits.h>

#include <spa/utils/defs.h>

struct partition {
	struct convolver *conv;

	int block_size;
	int time_size;
	int freq_size;

	int n_segments;
	float **segments;
	float **segments_ir;
	int current;

	float *time_buffer[2];

	void *fft;
	void *ifft;

	float *pre_mult;
	float *freq;
	float *precalc[2];

	int block_fill;

	float scale;
};

struct convolver
{
	struct spa_fga_dsp *dsp;
	struct convolver *conv;

	int min_size;
	int max_size;

	float *delay[2];
	int delay_fill;

	struct partition *partition[16];
	int n_partition;

	bool threaded;
	bool running;
	pthread_t thread;
	sem_t sem_start;
	sem_t sem_finish;
};

static int next_power_of_two(int val)
{
	int r = 1;
	while (r < val)
		r *= 2;
	return r;
}

static void partition_reset(struct spa_fga_dsp *dsp, struct partition *part)
{
	int i;
	for (i = 0; i < part->n_segments; i++)
		spa_fga_dsp_fft_memclear(dsp, part->segments[i], part->freq_size, false);
	spa_fga_dsp_fft_memclear(dsp, part->time_buffer[0], part->time_size, true);
	spa_fga_dsp_fft_memclear(dsp, part->time_buffer[1], part->time_size, true);
	spa_fga_dsp_fft_memclear(dsp, part->pre_mult, part->freq_size, false);
	spa_fga_dsp_fft_memclear(dsp, part->freq, part->freq_size, false);
	spa_fga_dsp_fft_memclear(dsp, part->precalc[0], part->block_size, true);
	spa_fga_dsp_fft_memclear(dsp, part->precalc[1], part->block_size, true);
	part->block_fill = 0;
	part->current = 0;
}

static void partition_free(struct spa_fga_dsp *dsp, struct partition *part)
{
	int i;
	for (i = 0; i < part->n_segments; i++) {
		if (part->segments)
			spa_fga_dsp_fft_memfree(dsp, part->segments[i]);
		if (part->segments_ir)
			spa_fga_dsp_fft_memfree(dsp, part->segments_ir[i]);
	}
	if (part->fft)
		spa_fga_dsp_fft_free(dsp, part->fft);
	if (part->ifft)
		spa_fga_dsp_fft_free(dsp, part->ifft);
	if (part->time_buffer[0])
		spa_fga_dsp_fft_memfree(dsp, part->time_buffer[0]);
	if (part->time_buffer[1])
		spa_fga_dsp_fft_memfree(dsp, part->time_buffer[1]);
	free(part->segments);
	free(part->segments_ir);
	spa_fga_dsp_fft_memfree(dsp, part->pre_mult);
	spa_fga_dsp_fft_memfree(dsp, part->freq);
	spa_fga_dsp_fft_memfree(dsp, part->precalc[0]);
	spa_fga_dsp_fft_memfree(dsp, part->precalc[1]);
	free(part);
}

static struct partition *partition_new(struct convolver *conv, int block, const float *ir, int irlen)
{
	struct partition *part;
	struct spa_fga_dsp *dsp = conv->dsp;
	int i;

	if (block == 0)
		return NULL;

	while (irlen > 0 && fabs(ir[irlen-1]) < 0.000001f)
		irlen--;

	part = calloc(1, sizeof(*part));
	if (part == NULL)
		return NULL;

	part->conv = conv;

	if (irlen == 0)
		return part;


	part->block_size = next_power_of_two(block);
	part->time_size = 2 * part->block_size;
	part->n_segments = (irlen + part->block_size-1) / part->block_size;
	part->freq_size = (part->time_size / 2) + 1;

	part->fft = spa_fga_dsp_fft_new(dsp, part->time_size, true);
	if (part->fft == NULL)
		goto error;
	part->ifft = spa_fga_dsp_fft_new(dsp, part->time_size, true);
	if (part->ifft == NULL)
		goto error;

	part->time_buffer[0] = spa_fga_dsp_fft_memalloc(dsp, part->time_size, true);
	part->time_buffer[1] = spa_fga_dsp_fft_memalloc(dsp, part->time_size, true);
	if (part->time_buffer[0] == NULL || part->time_buffer[1] == NULL)
		goto error;

	part->segments = calloc(part->n_segments, sizeof(float*));
	part->segments_ir = calloc(part->n_segments, sizeof(float*));
	if (part->segments == NULL || part->segments_ir == NULL)
		goto error;

	for (i = 0; i < part->n_segments; i++) {
		int left = irlen - (i * part->block_size);
		int copy = SPA_MIN(part->block_size, left);

		part->segments[i] = spa_fga_dsp_fft_memalloc(dsp, part->freq_size, false);
		part->segments_ir[i] = spa_fga_dsp_fft_memalloc(dsp, part->freq_size, false);
		if (part->segments[i] == NULL || part->segments_ir[i] == NULL)
			goto error;

		spa_fga_dsp_copy(dsp, part->time_buffer[0], &ir[i * part->block_size], copy);
		if (copy < part->time_size)
			spa_fga_dsp_fft_memclear(dsp, part->time_buffer[0] + copy, part->time_size - copy, true);

	        spa_fga_dsp_fft_run(dsp, part->fft, 1, part->time_buffer[0], part->segments_ir[i]);
	}
	part->pre_mult = spa_fga_dsp_fft_memalloc(dsp, part->freq_size, false);
	part->freq = spa_fga_dsp_fft_memalloc(dsp, part->freq_size, false);
	part->precalc[0] = spa_fga_dsp_fft_memalloc(dsp, part->block_size, true);
	part->precalc[1] = spa_fga_dsp_fft_memalloc(dsp, part->block_size, true);
	if (part->pre_mult == NULL || part->freq == NULL ||
	    part->precalc[0] == NULL ||  part->precalc[1] == NULL)
		goto error;
	part->scale = 1.0f / part->time_size;
	partition_reset(dsp, part);

	return part;
error:
	partition_free(dsp, part);
	return NULL;
}

static int partition_run(struct spa_fga_dsp *dsp, struct partition *part, const float *input, float *output, int len)
{
	int i;
	int block_fill = part->block_fill;
	int current = part->current;

	spa_fga_dsp_fft_run(dsp, part->fft, 1, input, part->segments[current]);

	spa_fga_dsp_fft_cmul(dsp, part->fft,
			part->freq,
			part->segments[current],
			part->segments_ir[0],
			part->freq_size, part->scale);

	for (i = 1; i < part->n_segments; i++) {
		if (++current == part->n_segments)
			current = 0;

		spa_fga_dsp_fft_cmuladd(dsp, part->fft,
				part->freq,
				part->freq,
				part->segments[current],
				part->segments_ir[i],
				part->freq_size, part->scale);
	}
	spa_fga_dsp_fft_run(dsp, part->ifft, -1, part->freq, part->time_buffer[0]);

	spa_fga_dsp_sum(dsp, output, part->time_buffer[0] + block_fill,
			part->time_buffer[1] + part->block_size + block_fill, len);

	block_fill += len;
	if (block_fill == part->block_size) {
		block_fill = 0;

		SPA_SWAP(part->time_buffer[0], part->time_buffer[1]);

		if (part->current == 0)
			part->current = part->n_segments;
		part->current--;
	}
	part->block_fill = block_fill;
	return len;
}

static void *do_background_process(void *data)
{
	struct partition *part = data;
	struct convolver *conv = part->conv;

	while (conv->running) {
		sem_wait(&conv->sem_start);

		if (!conv->running)
			break;

		partition_run(conv->dsp, part, conv->delay[1], part->time_buffer[1], part->block_size);

		sem_post(&conv->sem_finish);
	}
	return NULL;
}

void convolver_reset(struct convolver *conv)
{
	struct spa_fga_dsp *dsp = conv->dsp;
	int i;

	for (i = 0; i < conv->n_partition; i++)
		partition_reset(dsp, conv->partition[i]);

	spa_fga_dsp_fft_memclear(dsp, conv->delay[0], 2 * conv->max_size, true);
	spa_fga_dsp_fft_memclear(dsp, conv->delay[1], 2 * conv->max_size, true);
	conv->delay_fill = 0;
}

struct convolver *convolver_new(struct spa_fga_dsp *dsp, int head_block, int tail_block, const float *ir, int irlen)
{
	struct convolver *conv;
	int head_ir_len, min_size, max_size, ir_consumed = 0;

	if (head_block == 0 || tail_block == 0)
		return NULL;

	head_block = SPA_MAX(1, head_block);
	if (head_block > tail_block)
		SPA_SWAP(head_block, tail_block);

	while (irlen > 0 && fabs(ir[irlen-1]) < 0.000001f)
		irlen--;

	conv = calloc(1, sizeof(*conv));
	if (conv == NULL)
		return NULL;

	conv->dsp = dsp;

	if (irlen == 0)
		return conv;

	conv->min_size = next_power_of_two(head_block);
	conv->max_size = next_power_of_two(tail_block);

	min_size = conv->min_size;
	max_size = conv->max_size;

	while (ir_consumed < irlen) {
		head_ir_len = SPA_MIN(irlen - ir_consumed, 2 * max_size);
		conv->partition[conv->n_partition] = partition_new(conv, min_size, ir + ir_consumed, head_ir_len);
		if (conv->partition[conv->n_partition] == NULL)
			goto error;
		conv->n_partition++;

		ir_consumed += head_ir_len;
		min_size = max_size;
		max_size = min_size * 2;
		if (max_size > conv->max_size)
			max_size = irlen;
	}

	conv->delay[0] = spa_fga_dsp_fft_memalloc(dsp, 2 * conv->max_size, true);
	conv->delay[1] = spa_fga_dsp_fft_memalloc(dsp, 2 * conv->max_size, true);
	if (conv->delay[0] == NULL || conv->delay[1] == NULL)
		goto error;

	convolver_reset(conv);

	conv->threaded = false;

	if (conv->threaded) {
		int res;

		sem_init(&conv->sem_start, 0, 1);
		sem_init(&conv->sem_finish, 0, 0);
		conv->running = true;

		res = pthread_create(&conv->thread, NULL,
				do_background_process, conv);
		if (res != 0) {
			conv->threaded = false;
			sem_destroy(&conv->sem_start);
			sem_destroy(&conv->sem_finish);
		}
	}
	return conv;
error:
	convolver_free(conv);
	return NULL;
}

void convolver_free(struct convolver *conv)
{
	struct spa_fga_dsp *dsp = conv->dsp;
	int i;

	if (conv->threaded) {
		conv->running = false;
	        sem_post(&conv->sem_start);
		pthread_join(conv->thread, NULL);
		sem_destroy(&conv->sem_start);
		sem_destroy(&conv->sem_finish);
	}
	for (i = 0; i < conv->n_partition; i++)
		partition_free(dsp, conv->partition[i]);

	spa_fga_dsp_fft_memfree(dsp, conv->delay[0]);
	spa_fga_dsp_fft_memfree(dsp, conv->delay[1]);
	free(conv);
}

int convolver_run(struct convolver *conv, const float *input, float *output, int length)
{
	int processed = 0, i;
	struct spa_fga_dsp *dsp = conv->dsp;

	while (processed < length) {
		int remaining = length - processed;
		float *delay = conv->delay[0];
		int delay_fill = conv->delay_fill;
		struct partition *part = conv->partition[0];
		int block_size = part->block_size;
		int block_fill = delay_fill % block_size;
		int processing = SPA_MIN(remaining, block_size - block_fill);

		spa_memcpy(delay + delay_fill, input + processed, processing * sizeof(float));
		conv->delay_fill += processing;

		partition_run(dsp, part, delay + delay_fill - block_fill, &output[processed], processing);

		for (i = 1; i < conv->n_partition; i++) {
			part = conv->partition[i];
			block_size = part->block_size;
			block_fill = delay_fill % block_size;

			spa_fga_dsp_sum(dsp, &output[processed], &output[processed],
					&part->precalc[0][block_fill], processing);

			if (block_fill + processing == block_size) {
				SPA_SWAP(part->precalc[0], part->precalc[1]);
				partition_run(dsp, part, delay + conv->delay_fill - block_size,
						part->precalc[0], block_size);
			}
		}
		if (conv->delay_fill == conv->max_size) {
			SPA_SWAP(conv->delay[0], conv->delay[1]);
			memset(conv->delay[0], 0, conv->max_size * sizeof(float));
			conv->delay_fill = 0;
		}
		processed += processing;
	}
	return 0;
}
