/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2017 HiFi-LoFi */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */
/* Adapted from https://github.com/HiFi-LoFi/FFTConvolver */

#include "convolver.h"

#include <spa/utils/defs.h>

#include <math.h>

struct partition {
	int blockSize;
	int segSize;
	int segCount;
	int fftComplexSize;

	float **segments;
	float **segmentsIr;

	float *fft_buffer[2];

	void *fft;
	void *ifft;

	float *pre_mult;
	float *conv;

	int inputBufferFill;

	int current;
	float scale;
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
	for (i = 0; i < part->segCount; i++)
		spa_fga_dsp_fft_memclear(dsp, part->segments[i], part->fftComplexSize, false);
	spa_fga_dsp_fft_memclear(dsp, part->fft_buffer[0], part->segSize, true);
	spa_fga_dsp_fft_memclear(dsp, part->fft_buffer[1], part->segSize, true);
	spa_fga_dsp_fft_memclear(dsp, part->pre_mult, part->fftComplexSize, false);
	spa_fga_dsp_fft_memclear(dsp, part->conv, part->fftComplexSize, false);
	part->inputBufferFill = 0;
	part->current = 0;
}

static void partition_free(struct spa_fga_dsp *dsp, struct partition *part)
{
	int i;
	for (i = 0; i < part->segCount; i++) {
		if (part->segments)
			spa_fga_dsp_fft_memfree(dsp, part->segments[i]);
		if (part->segmentsIr)
			spa_fga_dsp_fft_memfree(dsp, part->segmentsIr[i]);
	}
	if (part->fft)
		spa_fga_dsp_fft_free(dsp, part->fft);
	if (part->ifft)
		spa_fga_dsp_fft_free(dsp, part->ifft);
	if (part->fft_buffer[0])
		spa_fga_dsp_fft_memfree(dsp, part->fft_buffer[0]);
	if (part->fft_buffer[1])
		spa_fga_dsp_fft_memfree(dsp, part->fft_buffer[1]);
	free(part->segments);
	free(part->segmentsIr);
	spa_fga_dsp_fft_memfree(dsp, part->pre_mult);
	spa_fga_dsp_fft_memfree(dsp, part->conv);
	free(part);
}

static struct partition *partition_new(struct spa_fga_dsp *dsp, int block, const float *ir, int irlen)
{
	struct partition *part;
	int i;

	if (block == 0)
		return NULL;

	while (irlen > 0 && fabs(ir[irlen-1]) < 0.000001f)
		irlen--;

	part = calloc(1, sizeof(*part));
	if (part == NULL)
		return NULL;

	if (irlen == 0)
		return part;

	part->blockSize = next_power_of_two(block);
	part->segSize = 2 * part->blockSize;
	part->segCount = (irlen + part->blockSize-1) / part->blockSize;
	part->fftComplexSize = (part->segSize / 2) + 1;

	part->fft = spa_fga_dsp_fft_new(dsp, part->segSize, true);
	if (part->fft == NULL)
		goto error;
	part->ifft = spa_fga_dsp_fft_new(dsp, part->segSize, true);
	if (part->ifft == NULL)
		goto error;

	part->fft_buffer[0] = spa_fga_dsp_fft_memalloc(dsp, part->segSize, true);
	part->fft_buffer[1] = spa_fga_dsp_fft_memalloc(dsp, part->segSize, true);
	if (part->fft_buffer[0] == NULL || part->fft_buffer[1] == NULL)
		goto error;

	part->segments = calloc(part->segCount, sizeof(float*));
	part->segmentsIr = calloc(part->segCount, sizeof(float*));
	if (part->segments == NULL || part->segmentsIr == NULL)
		goto error;

	for (i = 0; i < part->segCount; i++) {
		int left = irlen - (i * part->blockSize);
		int copy = SPA_MIN(part->blockSize, left);

		part->segments[i] = spa_fga_dsp_fft_memalloc(dsp, part->fftComplexSize, false);
		part->segmentsIr[i] = spa_fga_dsp_fft_memalloc(dsp, part->fftComplexSize, false);
		if (part->segments[i] == NULL || part->segmentsIr[i] == NULL)
			goto error;

		spa_fga_dsp_copy(dsp, part->fft_buffer[0], &ir[i * part->blockSize], copy);
		if (copy < part->segSize)
			spa_fga_dsp_fft_memclear(dsp, part->fft_buffer[0] + copy, part->segSize - copy, true);

	        spa_fga_dsp_fft_run(dsp, part->fft, 1, part->fft_buffer[0], part->segmentsIr[i]);
	}
	part->pre_mult = spa_fga_dsp_fft_memalloc(dsp, part->fftComplexSize, false);
	part->conv = spa_fga_dsp_fft_memalloc(dsp, part->fftComplexSize, false);
	if (part->pre_mult == NULL || part->conv == NULL)
			goto error;
	part->scale = 1.0f / part->segSize;
	partition_reset(dsp, part);

	return part;
error:
	partition_free(dsp, part);
	return NULL;
}

static int partition_run(struct spa_fga_dsp *dsp, struct partition *part, const float *input, float *output, int len)
{
	int i;

	if (part == NULL || part->segCount == 0) {
		spa_fga_dsp_fft_memclear(dsp, output, len, true);
		return len;
	}

	int inputBufferFill = part->inputBufferFill;

	spa_fga_dsp_fft_run(dsp, part->fft, 1, input, part->segments[part->current]);

	if (part->segCount > 1) {
		if (inputBufferFill == 0) {
			int indexAudio = part->current;

			if (++indexAudio == part->segCount)
				indexAudio = 0;

			spa_fga_dsp_fft_cmul(dsp, part->fft, part->pre_mult,
					part->segmentsIr[1],
					part->segments[indexAudio],
					part->fftComplexSize, part->scale);

			for (i = 2; i < part->segCount; i++) {
				if (++indexAudio == part->segCount)
					indexAudio = 0;

				spa_fga_dsp_fft_cmuladd(dsp, part->fft,
						part->pre_mult,
						part->pre_mult,
						part->segmentsIr[i],
						part->segments[indexAudio],
						part->fftComplexSize, part->scale);
			}
		}
		spa_fga_dsp_fft_cmuladd(dsp, part->fft,
				part->conv,
				part->pre_mult,
				part->segments[part->current],
				part->segmentsIr[0],
				part->fftComplexSize, part->scale);
	} else {
		spa_fga_dsp_fft_cmul(dsp, part->fft,
				part->conv,
				part->segments[part->current],
				part->segmentsIr[0],
				part->fftComplexSize, part->scale);
	}

	spa_fga_dsp_fft_run(dsp, part->ifft, -1, part->conv, part->fft_buffer[0]);

	spa_fga_dsp_sum(dsp, output, part->fft_buffer[0] + inputBufferFill,
			part->fft_buffer[1] + part->blockSize + inputBufferFill, len);

	inputBufferFill += len;
	if (inputBufferFill == part->blockSize) {
		inputBufferFill = 0;

		SPA_SWAP(part->fft_buffer[0], part->fft_buffer[1]);

		if (part->current == 0)
			part->current = part->segCount;
		part->current--;
	}
	part->inputBufferFill = inputBufferFill;
	return len;
}

struct convolver
{
	struct spa_fga_dsp *dsp;
	int headBlockSize;
	int tailBlockSize;
	struct partition *headPartition;
	struct partition *tailPartition;
	float *tailOutput;
	float *tailPrecalculated;
	float *tailInput;
	int tailInputFill;
};

void convolver_reset(struct convolver *conv)
{
	struct spa_fga_dsp *dsp = conv->dsp;

	if (conv->headPartition)
		partition_reset(dsp, conv->headPartition);
	if (conv->tailPartition) {
		partition_reset(dsp, conv->tailPartition);
		spa_fga_dsp_fft_memclear(dsp, conv->tailOutput, conv->tailBlockSize, true);
		spa_fga_dsp_fft_memclear(dsp, conv->tailPrecalculated, conv->tailBlockSize, true);
	}
	conv->tailInputFill = 0;
}

struct convolver *convolver_new(struct spa_fga_dsp *dsp, int head_block, int tail_block, const float *ir, int irlen)
{
	struct convolver *conv;
	int head_ir_len;

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

	conv->headBlockSize = next_power_of_two(head_block);
	conv->tailBlockSize = next_power_of_two(tail_block);

	head_ir_len = SPA_MIN(irlen, 2 * conv->tailBlockSize);
	conv->headPartition = partition_new(dsp, conv->headBlockSize, ir, head_ir_len);
	if (conv->headPartition == NULL)
		goto error;

	if (irlen > 2 * conv->tailBlockSize) {
		int tailIrLen = irlen - (2 * conv->tailBlockSize);
		conv->tailPartition = partition_new(dsp, conv->tailBlockSize, ir + (2 * conv->tailBlockSize), tailIrLen);
		conv->tailOutput = spa_fga_dsp_fft_memalloc(dsp, conv->tailBlockSize, true);
		conv->tailPrecalculated = spa_fga_dsp_fft_memalloc(dsp, conv->tailBlockSize, true);
		conv->tailInput = spa_fga_dsp_fft_memalloc(dsp, 2 * conv->tailBlockSize, true);
		if (conv->tailPartition == NULL || conv->tailOutput == NULL ||
				conv->tailPrecalculated == NULL || conv->tailInput == NULL)
			goto error;
	}

	convolver_reset(conv);

	return conv;
error:
	convolver_free(conv);
	return NULL;
}

void convolver_free(struct convolver *conv)
{
	struct spa_fga_dsp *dsp = conv->dsp;

	if (conv->headPartition)
		partition_free(dsp, conv->headPartition);
	if (conv->tailPartition)
		partition_free(dsp, conv->tailPartition);
	spa_fga_dsp_fft_memfree(dsp, conv->tailOutput);
	spa_fga_dsp_fft_memfree(dsp, conv->tailPrecalculated);
	spa_fga_dsp_fft_memfree(dsp, conv->tailInput);
	free(conv);
}

int convolver_run(struct convolver *conv, const float *input, float *output, int length)
{
	int processed = 0;
	struct spa_fga_dsp *dsp = conv->dsp;

	while (processed < length) {
		int remaining = length - processed;
		int blockRemain = conv->tailInputFill % conv->headBlockSize;
		int processing = SPA_MIN(remaining, conv->headBlockSize - blockRemain);

		spa_memcpy(conv->tailInput + conv->tailInputFill, input + processed, processing * sizeof(float));
		memset(conv->tailInput + conv->tailInputFill + processing, 0,
						(2 * conv->headBlockSize - processing) * sizeof(float));

		partition_run(dsp, conv->headPartition, conv->tailInput + conv->tailInputFill,
				&output[processed], processing);

		if (conv->tailPrecalculated)
			spa_fga_dsp_sum(dsp, &output[processed], &output[processed],
					&conv->tailPrecalculated[conv->tailInputFill],
					processing);

		conv->tailInputFill += processing;

		if (conv->tailInputFill == conv->tailBlockSize) {
			if (conv->tailPrecalculated) {
				SPA_SWAP(conv->tailPrecalculated, conv->tailOutput);
				partition_run(dsp, conv->tailPartition, conv->tailInput,
						conv->tailOutput, conv->tailBlockSize);
			}
			conv->tailInputFill = 0;
		}
		processed += processing;
	}
	return 0;
}
