/* Spa */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

#include <spa/support/log-impl.h>
#include <spa/debug/mem.h>

SPA_LOG_IMPL(logger);

static uint32_t cpu_flags;

#include "test-helper.h"

#include "peaks-ops.c"

static void test_impl(void)
{
	struct peaks peaks;
	unsigned int i;
	float vals[1038];
	float min[2] = { 0.0f, 0.0f }, max[2] = { 0.0f, 0.0f }, absmax[2] = { 0.0f, 0.0f };

	for (i = 0; i < SPA_N_ELEMENTS(vals); i++)
		vals[i] = (drand48() - 0.5f) * 2.5f;

	peaks_min_max_c(&peaks, &vals[1], SPA_N_ELEMENTS(vals) - 1, &min[0], &max[0]);
	printf("c peaks min:%f max:%f\n", min[0], max[0]);

	absmax[0] = peaks_abs_max_c(&peaks, &vals[1], SPA_N_ELEMENTS(vals) - 1, 0.0f);
	printf("c peaks abs-max:%f\n", absmax[0]);

#if defined(HAVE_SSE)
	if (cpu_flags & SPA_CPU_FLAG_SSE) {
		peaks_min_max_sse(&peaks, &vals[1], SPA_N_ELEMENTS(vals) - 1, &min[1], &max[1]);
		printf("sse peaks min:%f max:%f\n", min[1], max[1]);

		absmax[1] = peaks_abs_max_sse(&peaks, &vals[1], SPA_N_ELEMENTS(vals) - 1, 0.0f);
		printf("sse peaks abs-max:%f\n", absmax[1]);

		spa_assert(min[0] == min[1]);
		spa_assert(max[0] == max[1]);
		spa_assert(absmax[0] == absmax[1]);
	}
#endif

}

static void test_min_max(void)
{
	struct peaks peaks;
	const float vals[] = { 0.0f, 0.5f, -0.5f, 0.0f, 0.6f, -0.8f, -0.5f, 0.0f };
	float min = 0.0f, max = 0.0f;

	spa_zero(peaks);
	peaks.log = &logger.log;
	peaks.cpu_flags = cpu_flags;
	peaks_init(&peaks);

	peaks_min_max(&peaks, vals, SPA_N_ELEMENTS(vals), &min, &max);

	spa_assert(min == -0.8f);
	spa_assert(max == 0.6f);
}

static void test_abs_max(void)
{
	struct peaks peaks;
	const float vals[] = { 0.0f, 0.5f, -0.5f, 0.0f, 0.6f, -0.8f, -0.5f, 0.0f };
	float max = 0.0f;

	spa_zero(peaks);
	peaks.log = &logger.log;
	peaks.cpu_flags = cpu_flags;
	peaks_init(&peaks);

	max = peaks_abs_max(&peaks, vals, SPA_N_ELEMENTS(vals), max);

	spa_assert(max == 0.8f);
}

int main(int argc, char *argv[])
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	srand48(SPA_TIMESPEC_TO_NSEC(&ts));

	logger.log.level = SPA_LOG_LEVEL_TRACE;

	cpu_flags = get_cpu_flags();
	printf("got CPU flags %d\n", cpu_flags);

	test_impl();

	test_min_max();
	test_abs_max();

	return 0;
}
