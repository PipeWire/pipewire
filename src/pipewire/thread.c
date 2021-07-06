/* PipeWire
 *
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
 */

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>

#include <spa/utils/defs.h>
#include <spa/utils/list.h>

#include <pipewire/log.h>

#include "thread.h"

static struct spa_thread *impl_create(void *data,
			const struct spa_dict *props,
			void *(*start)(void*), void *arg)
{
	pthread_t pt;
	int err;
	if ((err = pthread_create(&pt, NULL, start, arg)) != 0) {
		errno = err;
		return NULL;
	}
	return (struct spa_thread*)pt;
}

static int impl_join(void *data, struct spa_thread *thread, void **retval)
{
	pthread_t pt = (pthread_t)thread;
	return pthread_join(pt, retval);
}

static int impl_get_rt_range(void *data, const struct spa_dict *props,
		int *min, int *max)
{
	if (min)
		*min = sched_get_priority_min(SCHED_OTHER);
	if (max)
		*max = sched_get_priority_max(SCHED_OTHER);
	return 0;
}

static struct {
	struct spa_thread_utils utils;
	struct spa_thread_utils_methods methods;
} default_impl = {
	{ { SPA_TYPE_INTERFACE_ThreadUtils,
		 SPA_VERSION_THREAD_UTILS,
		 SPA_CALLBACKS_INIT(&default_impl.methods,
				 &default_impl) } },
	{ SPA_VERSION_THREAD_UTILS_METHODS,
		.create = impl_create,
		.join = impl_join,
		.get_rt_range = impl_get_rt_range
	}
};

static struct spa_thread_utils *global_impl = &default_impl.utils;

SPA_EXPORT
void pw_thread_utils_set(struct spa_thread_utils *impl)
{
	if (impl == NULL)
		impl = &default_impl.utils;
	global_impl = impl;
}

SPA_EXPORT
struct spa_thread_utils *pw_thread_utils_get(void)
{
	return global_impl;
}
