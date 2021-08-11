// ==================================================================================
// Copyright (c) 2017 HiFi-LoFi
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is furnished
// to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
// WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ==================================================================================

#include "convolver.h"

#include <spa/utils/defs.h>

#include "kiss_fft_f32.h"
#include "kiss_fftr_f32.h"

struct convolver {
	int blockSize;
	int segSize;
	int segCount;
	int fftComplexSize;

	kiss_fft_f32_cpx **segments;
	kiss_fft_f32_cpx **segmentsIr;

	float *fft_buffer;

	void *fft;
	void *ifft;

	kiss_fft_f32_cpx *pre_mult;
	kiss_fft_f32_cpx *conv;
	float *overlap;

	float *inputBuffer;
	int inputBufferFill;

	int current;
};

static int next_power_of_two(int val)
{
	int r = 1;
	while (r < val)
		r *= 2;
	return r;
}

struct convolver *convolver_new(int block, const float *ir, int irlen)
{
	struct convolver *conv;
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

        conv->fft = kiss_fftr_f32_alloc(conv->segSize, 0, NULL, NULL);
        if (conv->fft == NULL)
                return NULL;
        conv->ifft = kiss_fftr_f32_alloc(conv->segSize, 1, NULL, NULL);
        if (conv->ifft == NULL)
                return NULL;

	conv->fft_buffer = calloc(sizeof(float), conv->segSize);
        if (conv->fft_buffer == NULL)
                return NULL;

	conv->segments = calloc(sizeof(kiss_fft_f32_cpx*), conv->segCount);
	conv->segmentsIr = calloc(sizeof(kiss_fft_f32_cpx*), conv->segCount);

	for (i = 0; i < conv->segCount; i++) {
		int left = irlen - (i * conv->blockSize);
		int copy = SPA_MIN(conv->blockSize, left);

		conv->segments[i] = calloc(sizeof(kiss_fft_f32_cpx), conv->fftComplexSize);
		conv->segmentsIr[i] = calloc(sizeof(kiss_fft_f32_cpx), conv->fftComplexSize);

		memcpy(conv->fft_buffer, &ir[i * conv->blockSize], copy * sizeof(float));
		if (copy < conv->segSize)
			memset(conv->fft_buffer + copy, 0, (conv->segSize - copy) * sizeof(float));

	        kiss_fftr_f32(conv->fft, conv->fft_buffer, conv->segmentsIr[i]);
	}
	conv->pre_mult = calloc(sizeof(kiss_fft_f32_cpx), conv->fftComplexSize);
	conv->conv = calloc(sizeof(kiss_fft_f32_cpx), conv->fftComplexSize);
	conv->overlap = calloc(sizeof(float), conv->blockSize);
	conv->inputBuffer = calloc(sizeof(float), conv->blockSize);
	conv->inputBufferFill = 0;
	conv->current = 0;

	return conv;
}

void convolver_free(struct convolver *conv)
{
	free(conv);
}

void Sum(float* result, const float* a, const float* b, int len)
{
	int i;
	for (i = 0; i < len; i++)
		result[i] = a[i] + b[i];
}

void ComplexMultiplyAccumulate(kiss_fft_f32_cpx *r, const kiss_fft_f32_cpx *a, const kiss_fft_f32_cpx *b, int len)
{
	int i;
	for (i = 0; i < len; i++) {
		r[i].r += a[i].r * b[i].r - a[i].i * b[i].i;
		r[i].i += a[i].r * b[i].i + a[i].i * b[i].r;
	}
}

int convolver_run(struct convolver *conv, const float *input, float *output, int len)
{
	int i, processed = 0;

	if (conv->segCount == 0) {
		memset(output, 0, len * sizeof(float));
		return len;
	}

	while (processed < len) {
		const int processing = SPA_MIN(len - processed, conv->blockSize - conv->inputBufferFill);
		const int inputBufferPos = conv->inputBufferFill;

		memcpy(conv->inputBuffer + inputBufferPos, input + processed, processing * sizeof(float));

		memcpy(conv->fft_buffer, conv->inputBuffer, conv->blockSize * sizeof(float));
		memset(conv->fft_buffer + conv->blockSize, 0, (conv->segSize - conv->blockSize) * sizeof(float));

		kiss_fftr_f32(conv->fft, conv->fft_buffer, conv->segments[conv->current]);

		if (conv->inputBufferFill == 0) {
			memset(conv->pre_mult, 0, sizeof(kiss_fft_f32_cpx) * conv->fftComplexSize);

			for (i = 1; i < conv->segCount; i++) {
				const int indexIr = i;
				const int indexAudio = (conv->current + i) % conv->segCount;

				ComplexMultiplyAccumulate(conv->pre_mult,
						conv->segmentsIr[indexIr],
						conv->segments[indexAudio],
						conv->fftComplexSize);
			}
		}
		memcpy(conv->conv, conv->pre_mult, sizeof(kiss_fft_f32_cpx) * conv->fftComplexSize);

		ComplexMultiplyAccumulate(conv->conv, conv->segments[conv->current], conv->segmentsIr[0],
				conv->fftComplexSize);

		kiss_fftri_f32(conv->ifft, conv->conv, conv->fft_buffer);

		for (i = 0; i < conv->segSize; i++)
			conv->fft_buffer[i] /= conv->segSize;

		Sum(output + processed, conv->fft_buffer + inputBufferPos, conv->overlap + inputBufferPos, processing);

		conv->inputBufferFill += processing;
		if (conv->inputBufferFill == conv->blockSize) {
			memset(conv->inputBuffer, 0, sizeof(float) * conv->blockSize);
			conv->inputBufferFill = 0;

			memcpy(conv->overlap, conv->fft_buffer + conv->blockSize, conv->blockSize * sizeof(float));

			conv->current = (conv->current > 0) ? (conv->current - 1) : (conv->segCount - 1);
		}

		processed += processing;
	}
	return len;
}
