/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2017 HiFi-LoFi */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */
/* Adapted from https://github.com/HiFi-LoFi/FFTConvolver */

#include "convolver.h"

#include <spa/utils/defs.h>

#include <math.h>

struct convolver1 {
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

	float *inputBuffer;
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

static void convolver1_reset(struct spa_fga_dsp *dsp, struct convolver1 *conv)
{
	int i;
	for (i = 0; i < conv->segCount; i++)
		spa_fga_dsp_fft_memclear(dsp, conv->segments[i], conv->fftComplexSize, false);
	spa_fga_dsp_fft_memclear(dsp, conv->fft_buffer[0], conv->segSize, true);
	spa_fga_dsp_fft_memclear(dsp, conv->fft_buffer[1], conv->segSize, true);
	spa_fga_dsp_fft_memclear(dsp, conv->inputBuffer, conv->segSize, true);
	spa_fga_dsp_fft_memclear(dsp, conv->pre_mult, conv->fftComplexSize, false);
	spa_fga_dsp_fft_memclear(dsp, conv->conv, conv->fftComplexSize, false);
	conv->inputBufferFill = 0;
	conv->current = 0;
}

static void convolver1_free(struct spa_fga_dsp *dsp, struct convolver1 *conv)
{
	int i;
	for (i = 0; i < conv->segCount; i++) {
		if (conv->segments)
			spa_fga_dsp_fft_memfree(dsp, conv->segments[i]);
		if (conv->segmentsIr)
			spa_fga_dsp_fft_memfree(dsp, conv->segmentsIr[i]);
	}
	if (conv->fft)
		spa_fga_dsp_fft_free(dsp, conv->fft);
	if (conv->ifft)
		spa_fga_dsp_fft_free(dsp, conv->ifft);
	if (conv->fft_buffer[0])
		spa_fga_dsp_fft_memfree(dsp, conv->fft_buffer[0]);
	if (conv->fft_buffer[1])
		spa_fga_dsp_fft_memfree(dsp, conv->fft_buffer[1]);
	free(conv->segments);
	free(conv->segmentsIr);
	spa_fga_dsp_fft_memfree(dsp, conv->pre_mult);
	spa_fga_dsp_fft_memfree(dsp, conv->conv);
	spa_fga_dsp_fft_memfree(dsp, conv->inputBuffer);
	free(conv);
}

static struct convolver1 *convolver1_new(struct spa_fga_dsp *dsp, int block, const float *ir, int irlen)
{
	struct convolver1 *conv;
	int i;

	if (block == 0)
		return NULL;

	while (irlen > 0 && fabs(ir[irlen-1]) < 0.000001f)
		irlen--;

	conv = calloc(1, sizeof(*conv));
	if (conv == NULL)
		return NULL;

	if (irlen == 0)
		return conv;

	conv->blockSize = next_power_of_two(block);
	conv->segSize = 2 * conv->blockSize;
	conv->segCount = (irlen + conv->blockSize-1) / conv->blockSize;
	conv->fftComplexSize = (conv->segSize / 2) + 1;

	conv->fft = spa_fga_dsp_fft_new(dsp, conv->segSize, true);
	if (conv->fft == NULL)
		goto error;
	conv->ifft = spa_fga_dsp_fft_new(dsp, conv->segSize, true);
	if (conv->ifft == NULL)
		goto error;

	conv->fft_buffer[0] = spa_fga_dsp_fft_memalloc(dsp, conv->segSize, true);
	conv->fft_buffer[1] = spa_fga_dsp_fft_memalloc(dsp, conv->segSize, true);
	if (conv->fft_buffer[0] == NULL || conv->fft_buffer[1] == NULL)
		goto error;

	conv->segments = calloc(conv->segCount, sizeof(float*));
	conv->segmentsIr = calloc(conv->segCount, sizeof(float*));
	if (conv->segments == NULL || conv->segmentsIr == NULL)
		goto error;

	for (i = 0; i < conv->segCount; i++) {
		int left = irlen - (i * conv->blockSize);
		int copy = SPA_MIN(conv->blockSize, left);

		conv->segments[i] = spa_fga_dsp_fft_memalloc(dsp, conv->fftComplexSize, false);
		conv->segmentsIr[i] = spa_fga_dsp_fft_memalloc(dsp, conv->fftComplexSize, false);
		if (conv->segments[i] == NULL || conv->segmentsIr[i] == NULL)
			goto error;

		spa_fga_dsp_copy(dsp, conv->fft_buffer[0], &ir[i * conv->blockSize], copy);
		if (copy < conv->segSize)
			spa_fga_dsp_fft_memclear(dsp, conv->fft_buffer[0] + copy, conv->segSize - copy, true);

	        spa_fga_dsp_fft_run(dsp, conv->fft, 1, conv->fft_buffer[0], conv->segmentsIr[i]);
	}
	conv->pre_mult = spa_fga_dsp_fft_memalloc(dsp, conv->fftComplexSize, false);
	conv->conv = spa_fga_dsp_fft_memalloc(dsp, conv->fftComplexSize, false);
	conv->inputBuffer = spa_fga_dsp_fft_memalloc(dsp, conv->segSize, true);
	if (conv->pre_mult == NULL || conv->conv == NULL || conv->inputBuffer == NULL)
			goto error;
	conv->scale = 1.0f / conv->segSize;
	convolver1_reset(dsp, conv);

	return conv;
error:
	convolver1_free(dsp, conv);
	return NULL;
}

static int convolver1_run(struct spa_fga_dsp *dsp, struct convolver1 *conv, const float *input, float *output, int len)
{
	int i, processed = 0;

	if (conv == NULL || conv->segCount == 0) {
		spa_fga_dsp_fft_memclear(dsp, output, len, true);
		return len;
	}

	int inputBufferFill = conv->inputBufferFill;
	while (processed < len) {
		const int processing = SPA_MIN(len - processed, conv->blockSize - inputBufferFill);

		spa_fga_dsp_copy(dsp, conv->inputBuffer + inputBufferFill, input + processed, processing);
		if (inputBufferFill == 0 && processing < conv->blockSize)
			spa_fga_dsp_fft_memclear(dsp, conv->inputBuffer + processing,
						conv->blockSize - processing, true);
		spa_fga_dsp_fft_run(dsp, conv->fft, 1, conv->inputBuffer, conv->segments[conv->current]);

		if (conv->segCount > 1) {
			if (inputBufferFill == 0) {
				int indexAudio = (conv->current + 1) % conv->segCount;

				spa_fga_dsp_fft_cmul(dsp, conv->fft, conv->pre_mult,
						conv->segmentsIr[1],
						conv->segments[indexAudio],
						conv->fftComplexSize, conv->scale);

				for (i = 2; i < conv->segCount; i++) {
					indexAudio = (conv->current + i) % conv->segCount;

					spa_fga_dsp_fft_cmuladd(dsp, conv->fft,
							conv->pre_mult,
							conv->pre_mult,
							conv->segmentsIr[i],
							conv->segments[indexAudio],
							conv->fftComplexSize, conv->scale);
				}
			}
			spa_fga_dsp_fft_cmuladd(dsp, conv->fft,
					conv->conv,
					conv->pre_mult,
					conv->segments[conv->current],
					conv->segmentsIr[0],
					conv->fftComplexSize, conv->scale);
		} else {
			spa_fga_dsp_fft_cmul(dsp, conv->fft,
					conv->conv,
					conv->segments[conv->current],
					conv->segmentsIr[0],
					conv->fftComplexSize, conv->scale);
		}

		spa_fga_dsp_fft_run(dsp, conv->ifft, -1, conv->conv, conv->fft_buffer[0]);

		spa_fga_dsp_sum(dsp, output + processed, conv->fft_buffer[0] + inputBufferFill,
				conv->fft_buffer[1] + conv->blockSize + inputBufferFill, processing);

		inputBufferFill += processing;
		if (inputBufferFill == conv->blockSize) {
			inputBufferFill = 0;

			SPA_SWAP(conv->fft_buffer[0], conv->fft_buffer[1]);

			conv->current = (conv->current > 0) ? (conv->current - 1) : (conv->segCount - 1);
		}

		processed += processing;
	}
	conv->inputBufferFill = inputBufferFill;
	return len;
}

struct convolver
{
	struct spa_fga_dsp *dsp;
	int headBlockSize;
	int tailBlockSize;
	struct convolver1 *headConvolver;
	struct convolver1 *tailConvolver0;
	float *tailOutput0;
	float *tailPrecalculated0;
	struct convolver1 *tailConvolver;
	float *tailOutput;
	float *tailPrecalculated;
	float *tailInput;
	int tailInputFill;
};

void convolver_reset(struct convolver *conv)
{
	struct spa_fga_dsp *dsp = conv->dsp;

	if (conv->headConvolver)
		convolver1_reset(dsp, conv->headConvolver);
	if (conv->tailConvolver0) {
		convolver1_reset(dsp, conv->tailConvolver0);
		spa_fga_dsp_fft_memclear(dsp, conv->tailOutput0, conv->tailBlockSize, true);
		spa_fga_dsp_fft_memclear(dsp, conv->tailPrecalculated0, conv->tailBlockSize, true);
	}
	if (conv->tailConvolver) {
		convolver1_reset(dsp, conv->tailConvolver);
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

	head_ir_len = SPA_MIN(irlen, conv->tailBlockSize);
	conv->headConvolver = convolver1_new(dsp, conv->headBlockSize, ir, head_ir_len);
	if (conv->headConvolver == NULL)
		goto error;

	if (irlen > conv->tailBlockSize) {
		int conv1IrLen = SPA_MIN(irlen - conv->tailBlockSize, conv->tailBlockSize);
		conv->tailConvolver0 = convolver1_new(dsp, conv->headBlockSize, ir + conv->tailBlockSize, conv1IrLen);
		conv->tailOutput0 = spa_fga_dsp_fft_memalloc(dsp, conv->tailBlockSize, true);
		conv->tailPrecalculated0 = spa_fga_dsp_fft_memalloc(dsp, conv->tailBlockSize, true);
		if (conv->tailConvolver0 == NULL || conv->tailOutput0 == NULL ||
				conv->tailPrecalculated0 == NULL)
			goto error;
	}

	if (irlen > 2 * conv->tailBlockSize) {
		int tailIrLen = irlen - (2 * conv->tailBlockSize);
		conv->tailConvolver = convolver1_new(dsp, conv->tailBlockSize, ir + (2 * conv->tailBlockSize), tailIrLen);
		conv->tailOutput = spa_fga_dsp_fft_memalloc(dsp, conv->tailBlockSize, true);
		conv->tailPrecalculated = spa_fga_dsp_fft_memalloc(dsp, conv->tailBlockSize, true);
		if (conv->tailConvolver == NULL || conv->tailOutput == NULL ||
				conv->tailPrecalculated == NULL)
			goto error;
	}

	if (conv->tailConvolver0 || conv->tailConvolver) {
		conv->tailInput = spa_fga_dsp_fft_memalloc(dsp, conv->tailBlockSize, true);
		if (conv->tailInput == NULL)
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

	if (conv->headConvolver)
		convolver1_free(dsp, conv->headConvolver);
	if (conv->tailConvolver0)
		convolver1_free(dsp, conv->tailConvolver0);
	if (conv->tailConvolver)
		convolver1_free(dsp, conv->tailConvolver);
	spa_fga_dsp_fft_memfree(dsp, conv->tailOutput0);
	spa_fga_dsp_fft_memfree(dsp, conv->tailPrecalculated0);
	spa_fga_dsp_fft_memfree(dsp, conv->tailOutput);
	spa_fga_dsp_fft_memfree(dsp, conv->tailPrecalculated);
	spa_fga_dsp_fft_memfree(dsp, conv->tailInput);
	free(conv);
}

int convolver_run(struct convolver *conv, const float *input, float *output, int length)
{
	struct spa_fga_dsp *dsp = conv->dsp;

	convolver1_run(dsp, conv->headConvolver, input, output, length);

	if (conv->tailInput) {
		int processed = 0;

		while (processed < length) {
			int remaining = length - processed;
			int processing = SPA_MIN(remaining, conv->headBlockSize - (conv->tailInputFill % conv->headBlockSize));

			if (conv->tailPrecalculated0)
				spa_fga_dsp_sum(dsp, &output[processed], &output[processed],
						&conv->tailPrecalculated0[conv->tailInputFill],
						processing);
			if (conv->tailPrecalculated)
				spa_fga_dsp_sum(dsp, &output[processed], &output[processed],
						&conv->tailPrecalculated[conv->tailInputFill],
						processing);

			spa_fga_dsp_copy(dsp, conv->tailInput + conv->tailInputFill, input + processed, processing);
			conv->tailInputFill += processing;

			if (conv->tailPrecalculated0 && (conv->tailInputFill % conv->headBlockSize == 0)) {
				int blockOffset = conv->tailInputFill - conv->headBlockSize;
				convolver1_run(dsp, conv->tailConvolver0,
						conv->tailInput + blockOffset,
						conv->tailOutput0 + blockOffset,
						conv->headBlockSize);
				if (conv->tailInputFill == conv->tailBlockSize)
					SPA_SWAP(conv->tailPrecalculated0, conv->tailOutput0);
			}

			if (conv->tailPrecalculated &&
			    conv->tailInputFill == conv->tailBlockSize) {
				SPA_SWAP(conv->tailPrecalculated, conv->tailOutput);
				convolver1_run(dsp, conv->tailConvolver, conv->tailInput,
						conv->tailOutput, conv->tailBlockSize);
			}
			if (conv->tailInputFill == conv->tailBlockSize)
				conv->tailInputFill = 0;

			processed += processing;
		}
	}
	return 0;
}
