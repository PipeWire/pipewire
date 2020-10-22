/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <time.h>

#include <spa/utils/defs.h>

#include <pipewire/log.h>

#include <pulse/timeval.h>
#include <pulse/rtclock.h>
#include "internal.h"

SPA_EXPORT
pa_usec_t pa_rtclock_now(void)
{
	struct timespec ts;
	pa_usec_t res;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	res = (ts.tv_sec * SPA_USEC_PER_SEC) + (ts.tv_nsec / SPA_NSEC_PER_USEC);
	return res;
}

static struct timeval *pa_rtclock_get(struct timeval *tv)
{
	struct timespec ts;

	pa_assert(tv);

	clock_gettime(CLOCK_MONOTONIC, &ts);
	tv->tv_sec = ts.tv_sec;
	tv->tv_usec = ts.tv_nsec / PA_NSEC_PER_USEC;
	return tv;
}

struct timeval* pa_rtclock_from_wallclock(struct timeval *tv)
{
	struct timeval wc_now, rt_now;

	pa_assert(tv);

	pa_gettimeofday(&wc_now);
	pa_rtclock_get(&rt_now);

	if (pa_timeval_cmp(&wc_now, tv) < 0)
		pa_timeval_add(&rt_now, pa_timeval_diff(tv, &wc_now));
	else
		pa_timeval_sub(&rt_now, pa_timeval_diff(&wc_now, tv));

	*tv = rt_now;

	return tv;
}

