/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PIPEWIRE_RTKIT_H__
#define __PIPEWIRE_RTKIT_H__

#include <sys/types.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTKIT_SERVICE_NAME "org.freedesktop.RealtimeKit1"
#define RTKIT_OBJECT_PATH "/org/freedesktop/RealtimeKit1"

/** \class pw_rtkit
 *
 * RTKit helpers
 */
/** Get an RTKit bus \memberof pw_rtkit */
struct pw_rtkit_bus *
pw_rtkit_bus_get_system(void);

/** Free an RTKit bus \memberof pw_rtkit */
void
pw_rtkit_bus_free(struct pw_rtkit_bus *system_bus);


/** This is mostly equivalent to sched_setparam(thread, SCHED_RR, {
 * .sched_priority = priority }). 'thread' needs to be a kernel thread
 * id as returned by gettid(), not a pthread_t! If 'thread' is 0 the
 * current thread is used. The returned value is a negative errno
 * style error code, or 0 on success.
 * \memberof pw_rtkit
 */
int
pw_rtkit_make_realtime(struct pw_rtkit_bus *system_bus, pid_t thread, int priority);


/** This is mostly equivalent to setpriority(PRIO_PROCESS, thread,
 * nice_level). 'thread' needs to be a kernel thread id as returned by
 * gettid(), not a pthread_t! If 'thread' is 0 the current thread is
 * used. The returned value is a negative errno style error code, or
 * 0 on success.
 * \memberof pw_rtkit
 */
int
pw_rtkit_make_high_priority(struct pw_rtkit_bus *system_bus, pid_t thread, int nice_level);

/** Return the maximum value of realtime priority available. Realtime requests
 * above this value will fail. A negative value is an errno style error code.
 * \memberof pw_rtkit
 */
int
pw_rtkit_get_max_realtime_priority(struct pw_rtkit_bus *system_bus);

/** Retreive the minimum value of nice level available. High prio requests
 * below this value will fail. The returned value is a negative errno
 * style error code, or 0 on success.
 * \memberof pw_rtkit
 */
int
pw_rtkit_get_min_nice_level(struct pw_rtkit_bus *system_bus, int *min_nice_level);

/** Return the maximum value of RLIMIT_RTTIME to set before attempting a
 * realtime request. A negative value is an errno style error code.
 * \memberof pw_rtkit
 */
long long
pw_rtkit_get_rttime_usec_max(struct pw_rtkit_bus *system_bus);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_RTKIT_H__ */
