/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stddef.h>
#include <sys/time.h>

#ifdef HAVE_WINDOWS_H
#include <windows.h>
#endif

#include <pulse/timeval.h>

#include "internal.h"

#define HAVE_GETTIMEOFDAY

SPA_EXPORT
struct timeval *pa_gettimeofday(struct timeval *tv) {
    pa_assert(tv);

#if defined(OS_IS_WIN32)
    /*
     * Copied from implementation by Steven Edwards (LGPL).
     * Found on wine mailing list.
     */
#if defined(_MSC_VER) || defined(__BORLANDC__)
#define EPOCHFILETIME (116444736000000000i64)
#else
#define EPOCHFILETIME (116444736000000000LL)
#endif
{
    FILETIME ft;
    LARGE_INTEGER li;
    int64_t t;

    GetSystemTimeAsFileTime(&ft);
    li.LowPart  = ft.dwLowDateTime;
    li.HighPart = ft.dwHighDateTime;
    t  = li.QuadPart;       /* In 100-nanosecond intervals */
    t -= EPOCHFILETIME;     /* Offset to the Epoch time */
    t /= 10;                /* In microseconds */
    tv->tv_sec  = (time_t) (t / PA_USEC_PER_SEC);
    tv->tv_usec = (suseconds_t) (t % PA_USEC_PER_SEC);
}
#elif defined(HAVE_GETTIMEOFDAY)
    pa_assert_se(gettimeofday(tv, NULL) == 0);
#else
#error "Platform lacks gettimeofday() or equivalent function."
#endif

    return tv;
}

SPA_EXPORT
pa_usec_t pa_timeval_diff(const struct timeval *a, const struct timeval *b) {
    pa_usec_t r;

    pa_assert(a);
    pa_assert(b);

    /* Check which is the earlier time and swap the two arguments if required. */
    if (PA_UNLIKELY(pa_timeval_cmp(a, b) < 0)) {
        const struct timeval *c;
        c = a;
        a = b;
        b = c;
    }

    /* Calculate the second difference*/
    r = ((pa_usec_t) a->tv_sec - (pa_usec_t) b->tv_sec) * PA_USEC_PER_SEC;

    /* Calculate the microsecond difference */
    if (a->tv_usec > b->tv_usec)
        r += (pa_usec_t) a->tv_usec - (pa_usec_t) b->tv_usec;
    else if (a->tv_usec < b->tv_usec)
        r -= (pa_usec_t) b->tv_usec - (pa_usec_t) a->tv_usec;

    return r;
}

SPA_EXPORT
int pa_timeval_cmp(const struct timeval *a, const struct timeval *b) {
    pa_assert(a);
    pa_assert(b);

    if (a->tv_sec < b->tv_sec)
        return -1;

    if (a->tv_sec > b->tv_sec)
        return 1;

    if (a->tv_usec < b->tv_usec)
        return -1;

    if (a->tv_usec > b->tv_usec)
        return 1;

    return 0;
}

SPA_EXPORT
pa_usec_t pa_timeval_age(const struct timeval *tv) {
    struct timeval now;
    pa_assert(tv);

    return pa_timeval_diff(pa_gettimeofday(&now), tv);
}

SPA_EXPORT
struct timeval* pa_timeval_add(struct timeval *tv, pa_usec_t v) {
    time_t secs;
    pa_assert(tv);

    secs = (time_t) (v/PA_USEC_PER_SEC);

    if (PA_UNLIKELY(tv->tv_sec > PA_INT_TYPE_MAX(time_t) - secs))
        goto overflow;

    tv->tv_sec += secs;
    v -= (pa_usec_t) secs * PA_USEC_PER_SEC;
    tv->tv_usec += (suseconds_t) v;

    /* Normalize */
    while ((pa_usec_t) tv->tv_usec >= PA_USEC_PER_SEC) {

        if (PA_UNLIKELY(tv->tv_sec >= PA_INT_TYPE_MAX(time_t)))
            goto overflow;

        tv->tv_sec++;
        tv->tv_usec -= (suseconds_t) PA_USEC_PER_SEC;
    }

    return tv;

overflow:
    tv->tv_sec = PA_INT_TYPE_MAX(time_t);
    tv->tv_usec = (suseconds_t) (PA_USEC_PER_SEC-1);
    return tv;
}

SPA_EXPORT
struct timeval* pa_timeval_sub(struct timeval *tv, pa_usec_t v) {
    time_t secs;
    pa_assert(tv);

    secs = (time_t) (v/PA_USEC_PER_SEC);

    if (PA_UNLIKELY(tv->tv_sec < secs))
        goto underflow;

    tv->tv_sec -= secs;
    v -= (pa_usec_t) secs * PA_USEC_PER_SEC;

    if (tv->tv_usec >= (suseconds_t) v)
        tv->tv_usec -= (suseconds_t) v;
    else {

        if (PA_UNLIKELY(tv->tv_sec <= 0))
            goto underflow;

        tv->tv_sec --;
        tv->tv_usec += (suseconds_t) (PA_USEC_PER_SEC - v);
    }

    return tv;

underflow:
    tv->tv_sec = 0;
    tv->tv_usec = 0;
    return tv;
}

SPA_EXPORT
struct timeval* pa_timeval_store(struct timeval *tv, pa_usec_t v) {
    pa_assert(tv);

    if (PA_UNLIKELY(v == PA_USEC_INVALID)) {
        tv->tv_sec = PA_INT_TYPE_MAX(time_t);
        tv->tv_usec = (suseconds_t) (PA_USEC_PER_SEC-1);

        return tv;
    }

    tv->tv_sec = (time_t) (v / PA_USEC_PER_SEC);
    tv->tv_usec = (suseconds_t) (v % PA_USEC_PER_SEC);

    return tv;
}

SPA_EXPORT
pa_usec_t pa_timeval_load(const struct timeval *tv) {

    if (PA_UNLIKELY(!tv))
        return PA_USEC_INVALID;

    return
        (pa_usec_t) tv->tv_sec * PA_USEC_PER_SEC +
        (pa_usec_t) tv->tv_usec;
}
