/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2017 HiFi-LoFi */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */
/* Adapted from https://github.com/HiFi-LoFi/FFTConvolver */

#include "convolver.h"

#include <spa/utils/defs.h>

#include <math.h>

static struct dsp_ops *dsp;

struct convolver1 {
	int blockSize;
	int segSize;
	int segCount;
	int fftComplexSize;

	float **segments;
	float **segmentsIr;

	float *fft_buffer;

	void *fft;
	void *ifft;

	float *pre_mult;
	float *conv;
	float *overlap;

	float *inputBuffer;
	int inputBufferFill;

	int current;
	float scale;
};

static void *fft_alloc(int size)
{
	size_t nb_bytes = size * sizeof(float);
#define ALIGNMENT 64
	void *p, *p0 = malloc(nb_bytes + ALIGNMENT);
	if (!p0)
		return (void *)0;
	p = (void *)(((size_t)p0 + ALIGNMENT) & (~((size_t)(ALIGNMENT - 1))));
	*((void **)p - 1) = p0;
	return p;
}
static void fft_free(void *p)
{
	if (p)
		free(*((void **)p - 1));
}

static inline void fft_cpx_clear(float *v, int size)
{
	dsp_ops_clear(dsp, v, size * 2);
}
static float *fft_cpx_alloc(int size)
{
	return fft_alloc(size * 2);
}

static void fft_cpx_free(float *cpx)
{
	fft_free(cpx);
}

static int next_power_of_two(int val)
{
	int r = 1;
	while (r < val)
		r *= 2;
	return r;
}

static void convolver1_reset(struct convolver1 *conv)
{
	int i;
	for (i = 0; i < conv->segCount; i++)
		fft_cpx_clear(conv->segments[i], conv->fftComplexSize);
	dsp_ops_clear(dsp, conv->overlap, conv->blockSize);
	dsp_ops_clear(dsp, conv->inputBuffer, conv->segSize);
	fft_cpx_clear(conv->pre_mult, conv->fftComplexSize);
	fft_cpx_clear(conv->conv, conv->fftComplexSize);
	conv->inputBufferFill = 0;
	conv->current = 0;
}

static struct convolver1 *convolver1_new(int block, const float *ir, int irlen)
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

	conv->fft = dsp_ops_fft_new(dsp, conv->segSize, true);
	if (conv->fft == NULL)
		goto error;
	conv->ifft = dsp_ops_fft_new(dsp, conv->segSize, true);
	if (conv->ifft == NULL)
		goto error;

	conv->fft_buffer = fft_alloc(conv->segSize);
	if (conv->fft_buffer == NULL)
		goto error;

	conv->segments = calloc(sizeof(float*), conv->segCount);
	conv->segmentsIr = calloc(sizeof(float*), conv->segCount);

	for (i = 0; i < conv->segCount; i++) {
		int left = irlen - (i * conv->blockSize);
		int copy = SPA_MIN(conv->blockSize, left);

		conv->segments[i] = fft_cpx_alloc(conv->fftComplexSize);
		conv->segmentsIr[i] = fft_cpx_alloc(conv->fftComplexSize);

		dsp_ops_copy(dsp, conv->fft_buffer, &ir[i * conv->blockSize], copy);
		if (copy < conv->segSize)
			dsp_ops_clear(dsp, conv->fft_buffer + copy, conv->segSize - copy);

	        dsp_ops_fft_run(dsp, conv->fft, 1, conv->fft_buffer, conv->segmentsIr[i]);
	}
	conv->pre_mult = fft_cpx_alloc(conv->fftComplexSize);
	conv->conv = fft_cpx_alloc(conv->fftComplexSize);
	conv->overlap = fft_alloc(conv->blockSize);
	conv->inputBuffer = fft_alloc(conv->segSize);
	conv->scale = 1.0f / conv->segSize;
	convolver1_reset(conv);

	return conv;
error:
	if (conv->fft)
		dsp_ops_fft_free(dsp, conv->fft);
	if (conv->ifft)
		dsp_ops_fft_free(dsp, conv->ifft);
	if (conv->fft_buffer)
		fft_free(conv->fft_buffer);
	free(conv);
	return NULL;
}

static void convolver1_free(struct convolver1 *conv)
{
	int i;
	for (i = 0; i < conv->segCount; i++) {
		fft_cpx_free(conv->segments[i]);
		fft_cpx_free(conv->segmentsIr[i]);
	}
	if (conv->fft)
		dsp_ops_fft_free(dsp, conv->fft);
	if (conv->ifft)
		dsp_ops_fft_free(dsp, conv->ifft);
	if (conv->fft_buffer)
		fft_free(conv->fft_buffer);
	free(conv->segments);
	free(conv->segmentsIr);
	fft_cpx_free(conv->pre_mult);
	fft_cpx_free(conv->conv);
	fft_free(conv->overlap);
	fft_free(conv->inputBuffer);
	free(conv);
}

static int convolver1_run(struct convolver1 *conv, const float *input, float *output, int len)
{
	int i, processed = 0;

	if (conv == NULL || conv->segCount == 0) {
		dsp_ops_clear(dsp, output, len);
		return len;
	}

	while (processed < len) {
		const int processing = SPA_MIN(len - processed, conv->blockSize - conv->inputBufferFill);
		const int inputBufferPos = conv->inputBufferFill;

		dsp_ops_copy(dsp, conv->inputBuffer + inputBufferPos, input + processed, processing);
		if (inputBufferPos == 0 && processing < conv->blockSize)
			dsp_ops_clear(dsp, conv->inputBuffer + processing, conv->blockSize - processing);

		dsp_ops_fft_run(dsp, conv->fft, 1, conv->inputBuffer, conv->segments[conv->current]);

		if (conv->segCount > 1) {
			if (conv->inputBufferFill == 0) {
				int indexAudio = (conv->current + 1) % conv->segCount;

				dsp_ops_fft_cmul(dsp, conv->fft, conv->pre_mult,
						conv->segmentsIr[1],
						conv->segments[indexAudio],
						conv->fftComplexSize, conv->scale);

				for (i = 2; i < conv->segCount; i++) {
					indexAudio = (conv->current + i) % conv->segCount;

					dsp_ops_fft_cmuladd(dsp, conv->fft,
							conv->pre_mult,
							conv->pre_mult,
							conv->segmentsIr[i],
							conv->segments[indexAudio],
							conv->fftComplexSize, conv->scale);
				}
			}
			dsp_ops_fft_cmuladd(dsp, conv->fft,
					conv->conv,
					conv->pre_mult,
					conv->segments[conv->current],
					conv->segmentsIr[0],
					conv->fftComplexSize, conv->scale);
		} else {
			dsp_ops_fft_cmul(dsp, conv->fft,
					conv->conv,
					conv->segments[conv->current],
					conv->segmentsIr[0],
					conv->fftComplexSize, conv->scale);
		}

		dsp_ops_fft_run(dsp, conv->ifft, -1, conv->conv, conv->fft_buffer);

		dsp_ops_sum(dsp, output + processed, conv->fft_buffer + inputBufferPos,
				conv->overlap + inputBufferPos, processing);

		conv->inputBufferFill += processing;
		if (conv->inputBufferFill == conv->blockSize) {
			conv->inputBufferFill = 0;

			dsp_ops_copy(dsp, conv->overlap, conv->fft_buffer + conv->blockSize, conv->blockSize);

			conv->current = (conv->current > 0) ? (conv->current - 1) : (conv->segCount - 1);
		}

		processed += processing;
	}
	return len;
}

struct convolver
{
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
	int precalculatedPos;
};

void convolver_reset(struct convolver *conv)
{
	if (conv->headConvolver)
		convolver1_reset(conv->headConvolver);
	if (conv->tailConvolver0) {
		convolver1_reset(conv->tailConvolver0);
		dsp_ops_clear(dsp, conv->tailOutput0, conv->tailBlockSize);
		dsp_ops_clear(dsp, conv->tailPrecalculated0, conv->tailBlockSize);
	}
	if (conv->tailConvolver) {
		convolver1_reset(conv->tailConvolver);
		dsp_ops_clear(dsp, conv->tailOutput, conv->tailBlockSize);
		dsp_ops_clear(dsp, conv->tailPrecalculated, conv->tailBlockSize);
	}
	conv->tailInputFill = 0;
	conv->precalculatedPos = 0;
}

struct convolver *convolver_new(struct dsp_ops *dsp_ops, int head_block, int tail_block, const float *ir, int irlen)
{
	struct convolver *conv;
	int head_ir_len;

	dsp = dsp_ops;

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

	if (irlen == 0)
		return conv;

	conv->headBlockSize = next_power_of_two(head_block);
	conv->tailBlockSize = next_power_of_two(tail_block);

	head_ir_len = SPA_MIN(irlen, conv->tailBlockSize);
	conv->headConvolver = convolver1_new(conv->headBlockSize, ir, head_ir_len);

	if (irlen > conv->tailBlockSize) {
		int conv1IrLen = SPA_MIN(irlen - conv->tailBlockSize, conv->tailBlockSize);
		conv->tailConvolver0 = convolver1_new(conv->headBlockSize, ir + conv->tailBlockSize, conv1IrLen);
		conv->tailOutput0 = fft_alloc(conv->tailBlockSize);
		conv->tailPrecalculated0 = fft_alloc(conv->tailBlockSize);
	}

	if (irlen > 2 * conv->tailBlockSize) {
		int tailIrLen = irlen - (2 * conv->tailBlockSize);
		conv->tailConvolver = convolver1_new(conv->tailBlockSize, ir + (2 * conv->tailBlockSize), tailIrLen);
		conv->tailOutput = fft_alloc(conv->tailBlockSize);
		conv->tailPrecalculated = fft_alloc(conv->tailBlockSize);
	}

	if (conv->tailConvolver0 || conv->tailConvolver)
		conv->tailInput = fft_alloc(conv->tailBlockSize);

	convolver_reset(conv);

	return conv;
}

void convolver_free(struct convolver *conv)
{
	if (conv->headConvolver)
		convolver1_free(conv->headConvolver);
	if (conv->tailConvolver0)
		convolver1_free(conv->tailConvolver0);
	if (conv->tailConvolver)
		convolver1_free(conv->tailConvolver);
	fft_free(conv->tailOutput0);
	fft_free(conv->tailPrecalculated0);
	fft_free(conv->tailOutput);
	fft_free(conv->tailPrecalculated);
	fft_free(conv->tailInput);
	free(conv);
}

int convolver_run(struct convolver *conv, const float *input, float *output, int length)
{
	convolver1_run(conv->headConvolver, input, output, length);

	if (conv->tailInput) {
		int processed = 0;

		while (processed < length) {
			int remaining = length - processed;
			int processing = SPA_MIN(remaining, conv->headBlockSize - (conv->tailInputFill % conv->headBlockSize));

			if (conv->tailPrecalculated0)
				dsp_ops_sum(dsp, &output[processed], &output[processed],
						&conv->tailPrecalculated0[conv->precalculatedPos],
						processing);
			if (conv->tailPrecalculated)
				dsp_ops_sum(dsp, &output[processed], &output[processed],
						&conv->tailPrecalculated[conv->precalculatedPos],
						processing);
			conv->precalculatedPos += processing;

			dsp_ops_copy(dsp, conv->tailInput + conv->tailInputFill, input + processed, processing);
			conv->tailInputFill += processing;

			if (conv->tailPrecalculated0 && (conv->tailInputFill % conv->headBlockSize == 0)) {
				int blockOffset = conv->tailInputFill - conv->headBlockSize;
				convolver1_run(conv->tailConvolver0,
						conv->tailInput + blockOffset,
						conv->tailOutput0 + blockOffset,
						conv->headBlockSize);
				if (conv->tailInputFill == conv->tailBlockSize)
					SPA_SWAP(conv->tailPrecalculated0, conv->tailOutput0);
			}

			if (conv->tailPrecalculated &&
			    conv->tailInputFill == conv->tailBlockSize) {
				SPA_SWAP(conv->tailPrecalculated, conv->tailOutput);
				convolver1_run(conv->tailConvolver, conv->tailInput,
						conv->tailOutput, conv->tailBlockSize);
			}
			if (conv->tailInputFill == conv->tailBlockSize) {
				conv->tailInputFill = 0;
				conv->precalculatedPos = 0;
			}
			processed += processing;
		}
	}
	return 0;
}
