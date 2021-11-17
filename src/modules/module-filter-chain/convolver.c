/* PipeWire
 *
 * Copyright (c) 2017 HiFi-LoFi
 * Copyright © 2021 Wim Taymans
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
 *
 * Adapted from https://github.com/HiFi-LoFi/FFTConvolver
 */
#include "convolver.h"

#include <spa/utils/defs.h>

#include <math.h>

#include "pffft.h"

struct fft_cpx {
	float *v;
};

struct convolver1 {
	int blockSize;
	int segSize;
	int segCount;
	int fftComplexSize;

	struct fft_cpx *segments;
	struct fft_cpx *segmentsIr;

	float *fft_buffer;

	void *fft;
	void *ifft;

	struct fft_cpx pre_mult;
	struct fft_cpx conv;
	float *overlap;

	float *inputBuffer;
	int inputBufferFill;

	int current;
	float scale;
};

static inline void fft_copy(void *dst, const void *src, int size)
{
	memcpy(dst, src, size * sizeof(float));
}

static void fft_clear(void *data, int size)
{
	memset(data, 0, size * sizeof(float));
}

static void *fft_alloc(int size)
{
	return pffft_aligned_malloc(size * sizeof(float));
}
static void fft_free(void *data)
{
	pffft_aligned_free(data);
}

static void fft_cpx_clear(struct fft_cpx *cpx, int size)
{
	fft_clear(cpx->v, size * 2);
}
static void fft_cpx_init(struct fft_cpx *cpx, int size)
{
	cpx->v = fft_alloc(size * 2);
}

static void fft_cpx_free(struct fft_cpx *cpx)
{
	fft_free(cpx->v);
}

static int next_power_of_two(int val)
{
	int r = 1;
	while (r < val)
		r *= 2;
	return r;
}

static inline void *fft_new(int size)
{
	return pffft_new_setup(size, PFFFT_REAL);
}

static inline void *ifft_new(int size)
{
	return pffft_new_setup(size, PFFFT_REAL);
}

static inline void fft_destroy(void *fft)
{
	pffft_destroy_setup(fft);
}

static inline void fft_run(void *fft, float *in, struct fft_cpx *out)
{
	pffft_transform(fft, in, out->v, NULL, PFFFT_FORWARD);
}

static inline void ifft_run(void *ifft, struct fft_cpx *in, float *out)
{
	pffft_transform(ifft, in->v, out, NULL, PFFFT_BACKWARD);
}

static inline void fft_convolve(void *fft, struct fft_cpx *r,
		const struct fft_cpx *a, const struct fft_cpx *b, int len, float scale)
{
	pffft_zconvolve(fft, a->v, b->v, r->v, scale);
}
static inline void fft_convolve_accum(void *fft, struct fft_cpx *r,
		const struct fft_cpx *c, const struct fft_cpx *a, const struct fft_cpx *b, int len, float scale)
{
	pffft_zconvolve_accumulate(fft, a->v, b->v, c->v, r->v, scale);
}

static inline void fft_sum(float *r, const float *a, const float *b,int len)
{
	pffft_sum(a, b, r, len);
}

static void convolver1_reset(struct convolver1 *conv)
{
	int i;
	for (i = 0; i < conv->segCount; i++)
		fft_cpx_clear(&conv->segments[i], conv->fftComplexSize);
	fft_clear(conv->overlap, conv->blockSize);
	fft_clear(conv->inputBuffer, conv->segSize);
	fft_cpx_clear(&conv->pre_mult, conv->fftComplexSize);
	fft_cpx_clear(&conv->conv, conv->fftComplexSize);
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

	conv->fft = fft_new(conv->segSize);
	if (conv->fft == NULL)
		goto error;
	conv->ifft = ifft_new(conv->segSize);
	if (conv->ifft == NULL)
		goto error;

	conv->fft_buffer = fft_alloc(conv->segSize);
	if (conv->fft_buffer == NULL)
		goto error;

	conv->segments = calloc(sizeof(struct fft_cpx), conv->segCount);
	conv->segmentsIr = calloc(sizeof(struct fft_cpx), conv->segCount);

	for (i = 0; i < conv->segCount; i++) {
		int left = irlen - (i * conv->blockSize);
		int copy = SPA_MIN(conv->blockSize, left);

		fft_cpx_init(&conv->segments[i], conv->fftComplexSize);
		fft_cpx_init(&conv->segmentsIr[i], conv->fftComplexSize);

		fft_copy(conv->fft_buffer, &ir[i * conv->blockSize], copy);
		if (copy < conv->segSize)
			fft_clear(conv->fft_buffer + copy, conv->segSize - copy);

	        fft_run(conv->fft, conv->fft_buffer, &conv->segmentsIr[i]);
	}
	fft_cpx_init(&conv->pre_mult, conv->fftComplexSize);
	fft_cpx_init(&conv->conv, conv->fftComplexSize);
	conv->overlap = fft_alloc(conv->blockSize);
	conv->inputBuffer = fft_alloc(conv->segSize);
	conv->scale = 1.0f / conv->segSize;
	convolver1_reset(conv);

	return conv;
error:
	if (conv->fft)
		fft_destroy(conv->fft);
	if (conv->ifft)
		fft_destroy(conv->ifft);
	if (conv->fft_buffer)
		fft_free(conv->fft_buffer);
	free(conv);
	return NULL;
}

static void convolver1_free(struct convolver1 *conv)
{
	int i;
	for (i = 0; i < conv->segCount; i++) {
		fft_cpx_free(&conv->segments[i]);
		fft_cpx_free(&conv->segmentsIr[i]);
	}
	fft_destroy(conv->fft);
	fft_destroy(conv->ifft);
	fft_free(conv->fft_buffer);
	free(conv->segments);
	free(conv->segmentsIr);
	fft_cpx_free(&conv->pre_mult);
	fft_cpx_free(&conv->conv);
	fft_free(conv->overlap);
	fft_free(conv->inputBuffer);
	free(conv);
}

static int convolver1_run(struct convolver1 *conv, const float *input, float *output, int len)
{
	int i, processed = 0;

	if (conv->segCount == 0) {
		fft_clear(output, len);
		return len;
	}

	while (processed < len) {
		const int processing = SPA_MIN(len - processed, conv->blockSize - conv->inputBufferFill);
		const int inputBufferPos = conv->inputBufferFill;

		fft_copy(conv->inputBuffer + inputBufferPos, input + processed, processing);
		if (inputBufferPos == 0 && processing < conv->blockSize)
			fft_clear(conv->inputBuffer + processing, conv->blockSize - processing);

		fft_run(conv->fft, conv->inputBuffer, &conv->segments[conv->current]);

		if (conv->segCount > 1) {
			if (conv->inputBufferFill == 0) {
				int indexAudio = (conv->current + 1) % conv->segCount;

				fft_convolve(conv->fft, &conv->pre_mult,
						&conv->segmentsIr[1],
						&conv->segments[indexAudio],
						conv->fftComplexSize, conv->scale);

				for (i = 2; i < conv->segCount; i++) {
					indexAudio = (conv->current + i) % conv->segCount;

					fft_convolve_accum(conv->fft,
							&conv->pre_mult,
							&conv->pre_mult,
							&conv->segmentsIr[i],
							&conv->segments[indexAudio],
							conv->fftComplexSize, conv->scale);
				}
			}
			fft_convolve_accum(conv->fft,
					&conv->conv,
					&conv->pre_mult,
					&conv->segments[conv->current],
					&conv->segmentsIr[0],
					conv->fftComplexSize, conv->scale);
		} else {
			fft_convolve(conv->fft, &conv->conv,
					&conv->segments[conv->current],
					&conv->segmentsIr[0],
					conv->fftComplexSize, conv->scale);
		}

		ifft_run(conv->ifft, &conv->conv, conv->fft_buffer);

		fft_sum(output + processed, conv->fft_buffer + inputBufferPos,
				conv->overlap + inputBufferPos, processing);

		conv->inputBufferFill += processing;
		if (conv->inputBufferFill == conv->blockSize) {
			conv->inputBufferFill = 0;

			fft_copy(conv->overlap, conv->fft_buffer + conv->blockSize, conv->blockSize);

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
		fft_clear(conv->tailOutput0, conv->tailBlockSize);
		fft_clear(conv->tailPrecalculated0, conv->tailBlockSize);
	}
	if (conv->tailConvolver) {
		convolver1_reset(conv->tailConvolver);
		fft_clear(conv->tailOutput, conv->tailBlockSize);
		fft_clear(conv->tailPrecalculated, conv->tailBlockSize);
	}
	conv->tailInputFill = 0;
	conv->precalculatedPos = 0;
}

struct convolver *convolver_new(int head_block, int tail_block, const float *ir, int irlen)
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
	int i;

	convolver1_run(conv->headConvolver, input, output, length);

	if (conv->tailInput) {
		int processed = 0;

		while (processed < length) {
			int remaining = length - processed;
			int processing = SPA_MIN(remaining, conv->headBlockSize - (conv->tailInputFill % conv->headBlockSize));

			const int sumBegin = processed;
			const int sumEnd = processed + processing;

			if (conv->tailPrecalculated0) {
				int precalculatedPos = conv->precalculatedPos;
				for (i = sumBegin; i < sumEnd; i++) {
					output[i] += conv->tailPrecalculated0[precalculatedPos];
					precalculatedPos++;
				}
			}

			if (conv->tailPrecalculated) {
				int precalculatedPos = conv->precalculatedPos;
				for (i = sumBegin; i < sumEnd; i++) {
					output[i] += conv->tailPrecalculated[precalculatedPos];
					precalculatedPos++;
				}
			}
			conv->precalculatedPos += processing;

			fft_copy(conv->tailInput + conv->tailInputFill, input + processed, processing);
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
				convolver1_run(conv->tailConvolver, conv->tailInput, conv->tailOutput, conv->tailBlockSize);
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
