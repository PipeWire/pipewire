/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2017 HiFi-LoFi */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */
/* Adapted from https://github.com/HiFi-LoFi/FFTConvolver */

#include "convolver.h"

#include <math.h>
#include <pthread.h>
#include <semaphore.h>
#include <limits.h>

#include <spa/utils/defs.h>

struct ir {
	float **segments;
	float *time_buffer[2];
	float *precalc[2];
};

struct partition {
	struct convolver *conv;

	int block_size;
	int time_size;
	int freq_size;
	void *fft;
	float *freq;

	int n_segments;
	int current;
	float **segments;

	int block_fill;
	int n_ir;
	struct ir *ir;
	int time_idx;
	int precalc_idx;
};

struct convolver
{
	struct spa_fga_dsp *dsp;

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
	for (i = 0; i < part->n_ir; i++) {
		struct ir *r = &part->ir[i];
		spa_fga_dsp_fft_memclear(dsp, r->time_buffer[0], part->time_size, true);
		spa_fga_dsp_fft_memclear(dsp, r->time_buffer[1], part->time_size, true);
		spa_fga_dsp_fft_memclear(dsp, r->precalc[0], part->block_size, true);
		spa_fga_dsp_fft_memclear(dsp, r->precalc[1], part->block_size, true);
	}
	spa_fga_dsp_fft_memclear(dsp, part->freq, part->freq_size, false);
	part->block_fill = 0;
	part->time_idx = 0;
	part->precalc_idx = 0;
	part->current = 0;
}

static void partition_free(struct spa_fga_dsp *dsp, struct partition *part)
{
	int i, j;
	for (i = 0; i < part->n_segments; i++) {
		if (part->segments)
			spa_fga_dsp_fft_memfree(dsp, part->segments[i]);
	}
	for (i = 0; i < part->n_ir; i++) {
		struct ir *r = &part->ir[i];
		for (j = 0; j < part->n_segments; j++) {
			if (r->segments)
				spa_fga_dsp_fft_memfree(dsp, r->segments[j]);
		}
		if (r->time_buffer[0])
			spa_fga_dsp_fft_memfree(dsp, r->time_buffer[0]);
		if (r->time_buffer[1])
			spa_fga_dsp_fft_memfree(dsp, r->time_buffer[1]);
		if (r->precalc[0])
			spa_fga_dsp_fft_memfree(dsp, r->precalc[0]);
		if (r->precalc[1])
			spa_fga_dsp_fft_memfree(dsp, r->precalc[1]);
		free(r->segments);
	}
	if (part->fft)
		spa_fga_dsp_fft_free(dsp, part->fft);
	free(part->segments);
	free(part->ir);
	spa_fga_dsp_fft_memfree(dsp, part->freq);
	free(part);
}

static struct partition *partition_new(struct convolver *conv, int block,
		const struct convolver_ir ir[], int n_ir, int iroffset, int irlen)
{
	struct partition *part;
	struct spa_fga_dsp *dsp = conv->dsp;
	int i, j;

	if (block <= 0)
		return NULL;

	part = calloc(1, sizeof(*part));
	if (part == NULL)
		return NULL;

	part->conv = conv;

	part->block_size = next_power_of_two(block);
	part->time_size = 2 * part->block_size;
	part->n_segments = (irlen + part->block_size-1) / part->block_size;
	part->freq_size = (part->time_size / 2) + 1;
	part->n_ir = n_ir;

	part->fft = spa_fga_dsp_fft_new(dsp, part->time_size, true);
	if (part->fft == NULL)
		goto error;

	part->segments = calloc(part->n_segments, sizeof(float*));
	part->freq = spa_fga_dsp_fft_memalloc(dsp, part->freq_size, false);
	part->ir = calloc(part->n_ir, sizeof(struct ir));
	if (part->segments == NULL || part->freq == NULL || part->ir == NULL)
		goto error;

	for (i = 0; i < part->n_segments; i++) {
		part->segments[i] = spa_fga_dsp_fft_memalloc(dsp, part->freq_size, false);
		if (part->segments[i] == NULL)
			goto error;
	}

	for (i = 0; i < part->n_ir; i++) {
		struct ir *r = &part->ir[i];

		r->segments = calloc(part->n_segments, sizeof(float*));
		r->time_buffer[0] = spa_fga_dsp_fft_memalloc(dsp, part->time_size, true);
		r->time_buffer[1] = spa_fga_dsp_fft_memalloc(dsp, part->time_size, true);
		r->precalc[0] = spa_fga_dsp_fft_memalloc(dsp, part->block_size, true);
		r->precalc[1] = spa_fga_dsp_fft_memalloc(dsp, part->block_size, true);
		if (r->segments == NULL || r->time_buffer[0] == NULL || r->time_buffer[1] == NULL ||
		    r->precalc[0] == NULL || r->precalc[1] == NULL)
			goto error;

		for (j = 0; j < part->n_segments; j++) {
			int left = ir[i].len - iroffset - (j * part->block_size);
			int copy = SPA_CLAMP(left, 0, part->block_size);

			r->segments[j] = spa_fga_dsp_fft_memalloc(dsp, part->freq_size, false);
			if (r->segments[j] == NULL)
				goto error;

			spa_fga_dsp_copy(dsp, r->time_buffer[0], &ir[i].ir[iroffset + (j * part->block_size)], copy);
			spa_fga_dsp_fft_memclear(dsp, r->time_buffer[0] + copy, part->time_size - copy, true);

		        spa_fga_dsp_fft_run(dsp, part->fft, 1, r->time_buffer[0], r->segments[j]);
		}
	}
	partition_reset(dsp, part);

	return part;
error:
	partition_free(dsp, part);
	return NULL;
}

static int partition_run(struct spa_fga_dsp *dsp, struct partition *part, const float *input,
		float *output[], int offset, int len)
{
	int i, j;
	int block_fill = part->block_fill;
	int idx = part->time_idx, pc_idx = part->precalc_idx;
	float *dst;

	spa_fga_dsp_fft_run(dsp, part->fft, 1, input, part->segments[part->current]);

	for (i = 0; i < part->n_ir; i++) {
		struct ir *r = &part->ir[i];
		int current = part->current;

		spa_fga_dsp_fft_cmul(dsp, part->fft,
				part->freq,
				part->segments[current],
				r->segments[0],
				part->freq_size);

		for (j = 1; j < part->n_segments; j++) {
			if (++current == part->n_segments)
				current = 0;

			spa_fga_dsp_fft_cmuladd(dsp, part->fft,
					part->freq,
					part->freq,
					part->segments[current],
					r->segments[j],
					part->freq_size);
		}
		spa_fga_dsp_fft_run(dsp, part->fft, -1, part->freq, r->time_buffer[idx]);

		dst = output ? output[i]: r->precalc[pc_idx];
		if (dst)
			spa_fga_dsp_sum(dsp, dst + offset, r->time_buffer[idx] + block_fill,
					r->time_buffer[idx^1] + part->block_size + block_fill, len);
	}

	block_fill += len;
	if (block_fill == part->block_size) {
		block_fill = 0;

		part->time_idx = idx ^ 1;

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

		partition_run(conv->dsp, part, conv->delay[1], NULL, 0, part->block_size);

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

struct convolver *convolver_new_many(struct spa_fga_dsp *dsp, int head_block, int tail_block,
		const struct convolver_ir ir[], int n_ir)
{
	struct convolver *conv;
	int i, irlen, head_ir_len, min_size, max_size, ir_consumed = 0;
	struct convolver_ir tmp[n_ir];

	if (head_block == 0 || tail_block == 0)
		return NULL;

	head_block = SPA_CLAMP(head_block, 1, (1<<16));
	tail_block = SPA_CLAMP(tail_block, 1, (1<<16));
	if (head_block > tail_block)
		SPA_SWAP(head_block, tail_block);

	conv = calloc(1, sizeof(*conv));
	if (conv == NULL)
		return NULL;

	irlen = 0;
	for (i = 0; i < n_ir; i++) {
		int l = ir[i].len;
		while (l > 0 && fabs(ir[i].ir[l-1]) < 0.000001f)
			l--;
		tmp[i].ir = ir[i].ir;
		tmp[i].len = l;
		irlen = SPA_MAX(irlen, l);
	}

	conv->dsp = dsp;
	conv->min_size = next_power_of_two(head_block);
	conv->max_size = next_power_of_two(tail_block);

	conv->delay[0] = spa_fga_dsp_fft_memalloc(dsp, 2 * conv->max_size, true);
	conv->delay[1] = spa_fga_dsp_fft_memalloc(dsp, 2 * conv->max_size, true);
	if (conv->delay[0] == NULL || conv->delay[1] == NULL)
		goto error;

	if (irlen == 0)
		return conv;

	min_size = conv->min_size;
	max_size = conv->max_size;

	while (ir_consumed < irlen) {
		head_ir_len = SPA_MIN(irlen - ir_consumed, 2 * max_size);

		conv->partition[conv->n_partition] = partition_new(conv, min_size,
				tmp, n_ir, ir_consumed, head_ir_len);
		if (conv->partition[conv->n_partition] == NULL)
			goto error;
		conv->n_partition++;

		ir_consumed += head_ir_len;
		min_size = max_size;
		max_size = min_size * 2;
		if (max_size > conv->max_size)
			max_size = irlen;
	}

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

struct convolver *convolver_new(struct spa_fga_dsp *dsp, int head_block, int tail_block, const float *ir, int irlen)
{
	const struct convolver_ir tmp = { ir, irlen };
	return convolver_new_many(dsp, head_block, tail_block, &tmp, 1);
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

int convolver_run_many(struct convolver *conv, const float *input, float *output[], int length)
{
	int processed = 0, i, j;
	struct spa_fga_dsp *dsp = conv->dsp;

	if (conv->n_partition == 0)
		return length;

	while (processed < length) {
		int remaining = length - processed, pc_idx;
		float *delay = conv->delay[0];
		int delay_fill = conv->delay_fill;
		struct partition *part = conv->partition[0];
		int block_size = part->block_size;
		int block_fill = delay_fill % block_size;
		int processing = SPA_MIN(remaining, block_size - block_fill);

		spa_memcpy(delay + delay_fill, input + processed, processing * sizeof(float));
		conv->delay_fill += processing;

		partition_run(dsp, part, delay + delay_fill - block_fill, output, processed, processing);

		for (i = 1; i < conv->n_partition; i++) {
			part = conv->partition[i];
			pc_idx = part->precalc_idx;
			block_size = part->block_size;
			block_fill = delay_fill % block_size;

			for (j = 0; j < part->n_ir; j++) {
				struct ir *r = &part->ir[j];
				spa_fga_dsp_sum(dsp, &output[j][processed], &output[j][processed],
						&r->precalc[pc_idx][block_fill], processing);
			}
			if (block_fill + processing == block_size) {
				part->precalc_idx = pc_idx ^ 1;
				partition_run(dsp, part, delay + conv->delay_fill - block_size,
						NULL, 0, block_size);
			}
		}
		if (conv->delay_fill == conv->max_size) {
			SPA_SWAP(conv->delay[0], conv->delay[1]);
			memset(conv->delay[0], 0, conv->max_size * sizeof(float));
			conv->delay_fill = 0;
		}
		processed += processing;
	}
	return processed;
}

int convolver_run(struct convolver *conv, const float *input, float *output, int length)
{
	float *tmp[1] = { output };
	return convolver_run_many(conv, input, tmp, length);
}
