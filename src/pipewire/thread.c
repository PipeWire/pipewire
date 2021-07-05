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


static struct pw_thread *impl_create(void *data,
			const struct spa_dict *props,
			void *(*start)(void*), void *arg)
{
	pthread_t pt;
	int err;
	if ((err = pthread_create(&pt, NULL, start, arg)) != 0) {
		errno = err;
		return NULL;
	}
	return (struct pw_thread*)pt;
}

static int impl_join(void *data, struct pw_thread *thread, void **retval)
{
	pthread_t pt = (pthread_t)thread;
	return pthread_join(pt, retval);
}

static struct {
	struct pw_thread_utils utils;
	struct pw_thread_utils_methods methods;
} default_impl = {
	{ { PW_TYPE_INTERFACE_ThreadUtils,
		 PW_VERSION_THREAD_UTILS,
		 SPA_CALLBACKS_INIT(&default_impl.methods,
				 &default_impl) } },
	{ PW_VERSION_THREAD_UTILS_METHODS,
            .create = impl_create,
	    .join = impl_join, }
};

static struct pw_thread_utils *global_impl = &default_impl.utils;

SPA_EXPORT
void pw_thread_utils_set_impl(struct pw_thread_utils *impl)
{
	if (impl == NULL)
		impl = &default_impl.utils;
	global_impl = impl;
}

SPA_EXPORT
struct pw_thread *pw_thread_utils_create(const struct spa_dict *props,
		void *(*start_routine)(void*), void *arg)
{
	struct pw_thread *res = NULL;
	spa_interface_call_res(&global_impl->iface,
			struct pw_thread_utils_methods, res, create, 0,
			props, start_routine, arg);
	return res;
}

SPA_EXPORT
int pw_thread_utils_join(struct pw_thread *thread, void **retval)
{
	int res = -ENOTSUP;
	spa_interface_call_res(&global_impl->iface,
			struct pw_thread_utils_methods, res, join, 0,
			thread, retval);
	return res;
}

SPA_EXPORT
int pw_thread_utils_acquire_rt(struct pw_thread *thread, int priority)
{
	int res = -ENOTSUP;
	spa_interface_call_res(&global_impl->iface,
			struct pw_thread_utils_methods, res, acquire_rt, 0,
			thread, priority);
	return res;
}

SPA_EXPORT
int pw_thread_utils_drop_rt(struct pw_thread *thread)
{
	int res = -ENOTSUP;
	spa_interface_call_res(&global_impl->iface,
			struct pw_thread_utils_methods, res, drop_rt, 0, thread);
	return res;
}
