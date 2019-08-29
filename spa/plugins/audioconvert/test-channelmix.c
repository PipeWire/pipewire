/* Spa
 *
 * Copyright © 2019 Wim Taymans
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
 */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <spa/support/log-impl.h>
#include <spa/debug/mem.h>

SPA_LOG_IMPL(logger);

#include "channelmix-ops.c"
static void dump_matrix(struct channelmix *mix)
{
	uint32_t i, j;

	for (i = 0; i < mix->dst_chan; i++) {
		for (j = 0; j < mix->src_chan; j++) {
			float v = mix->matrix_orig[i][j];
			spa_log_debug(mix->log, "%d %d: %f", i, j, v);
		}
	}
}

static void test_mix(uint32_t src_chan, uint32_t src_mask, uint32_t dst_chan, uint32_t dst_mask, float *coeff)
{
	struct channelmix mix;

	spa_log_debug(&logger.log, "start %d->%d (%08x -> %08x)", src_chan, dst_chan, src_mask, dst_mask);

	spa_zero(mix);
	mix.src_chan = src_chan;
	mix.dst_chan = dst_chan;
	mix.src_mask = src_mask;
	mix.dst_mask = dst_mask;
	mix.log = &logger.log;

	channelmix_init(&mix);
	dump_matrix(&mix);
}

static void test_1_N(void)
{
	test_mix(1, _M(MONO), 2, _M(FL)|_M(FR), (float[]) { 1.0, 1.0 });
	test_mix(1, _M(MONO), 3, _M(FL)|_M(FR)|_M(LFE), (float[]) { 1.0, 1.0, 0.0 });
	test_mix(1, _M(MONO), 4, _M(FL)|_M(FR)|_M(RL)|_M(RR), (float[]) { 1.0, 1.0, 1.0, 1.0 });
}

static void test_N_1(void)
{
	test_mix(1, _M(MONO), 1, _M(MONO), (float[]) { 1.0 });
	test_mix(1, _M(MONO), 1, _M(FC), (float[]) { 1.0 });
	test_mix(1, _M(FC), 1, _M(MONO), (float[]) { 1.0 });
	test_mix(1, _M(FC), 1, _M(FC), (float[]) { 1.0 });
	test_mix(2, _M(FL)|_M(FR), 1, _M(MONO), (float[]) { 0.5, 0.5 });
}

static void test_3p1_N(void)
{
	test_mix(4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 1, _M(MONO), (float[]) { 0.5, 0.5 });
	test_mix(4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 2, _M(FL)|_M(FR), (float[]) { 0.5, 0.5 });
	test_mix(4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 3, _M(FL)|_M(FR)|_M(LFE), (float[]) { 0.5, 0.5 });
	test_mix(4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), (float[]) { 0.5, 0.5 });
	test_mix(4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), 4, _M(FL)|_M(FR)|_M(RL)|_M(RR), (float[]) { 0.5, 0.5 });
}

static void test_4_N(void)
{
	test_mix(4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 1, _M(MONO), (float[]) { 0.5, 0.5 });
	test_mix(4, _M(FL)|_M(FR)|_M(SL)|_M(SR), 1, _M(MONO), (float[]) { 0.5, 0.5 });
	test_mix(4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 2, _M(FL)|_M(FR), (float[]) { 0.5, 0.5 });
	test_mix(4, _M(FL)|_M(FR)|_M(SL)|_M(SR), 2, _M(FL)|_M(FR), (float[]) { 0.5, 0.5 });
	test_mix(4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 3, _M(FL)|_M(FR)|_M(LFE), (float[]) { 0.5, 0.5 });
	test_mix(4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 4, _M(FL)|_M(FR)|_M(RL)|_M(RR), (float[]) { 0.5, 0.5 });
	test_mix(4, _M(FL)|_M(FR)|_M(RL)|_M(RR), 4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), (float[]) { 0.5, 0.5 });
}

static void test_5p1_N(void)
{
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 1, _M(MONO), (float[]) { 0.5, 0.5 });
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 2, _M(FL)|_M(FR), (float[]) { 0.5, 0.5 });
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(RL)|_M(RR), 2, _M(FL)|_M(FR), (float[]) { 0.5, 0.5 });
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 3, _M(FL)|_M(FR)|_M(LFE), (float[]) { 0.5, 0.5 });
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 4, _M(FL)|_M(FR)|_M(LFE)|_M(FC), (float[]) { 0.5, 0.5 });
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 4, _M(FL)|_M(FR)|_M(RL)|_M(RR), (float[]) { 0.5, 0.5 });
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 5, _M(FL)|_M(FR)|_M(FC)|_M(SL)|_M(SR), (float[]) { 0.5, 0.5 });
	test_mix(6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), 6, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR), (float[]) { 0.5, 0.5 });
}

static void test_7p1_N(void)
{
	test_mix(8, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR)|_M(RL)|_M(RR), 1, _M(MONO), (float[]) { 0.5, 0.5 });
	test_mix(8, _M(FL)|_M(FR)|_M(LFE)|_M(FC)|_M(SL)|_M(SR)|_M(RL)|_M(RR), 2, _M(FL)|_M(FR), (float[]) { 0.5, 0.5 });
}

int main(int argc, char *argv[])
{
	logger.log.level = SPA_LOG_LEVEL_TRACE;

	test_1_N();
	test_N_1();
	test_3p1_N();
	test_4_N();
	test_5p1_N();
	test_7p1_N();

	return 0;
}
