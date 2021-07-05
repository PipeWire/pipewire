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

#ifndef PIPEWIRE_THREAD_H
#define PIPEWIRE_THREAD_H

#ifdef __cplusplus
extern "C" {
#endif

#include <string.h>
#include <errno.h>

#include <spa/utils/dict.h>
#include <pipewire/type.h>

/** \defgroup pw_thread Thread related functions
 *
 * \brief functions to manipulate threads
 */

#define PW_TYPE_INTERFACE_ThreadUtils	PW_TYPE_INFO_INTERFACE_BASE "ThreadUtils"

/** a thread object.
 * This can be cast to a platform native thread, like pthread on posix systems
 */
struct pw_thread;

#define PW_VERSION_THREAD_UTILS		0
struct pw_thread_utils { struct spa_interface iface; };

/** thread utils */
struct pw_thread_utils_methods {
#define PW_VERSION_THREAD_UTILS_METHODS	0
	uint32_t version;

	/** create a new thread that runs \a start with \a arg */
	struct pw_thread * (*create) (void *data, const struct spa_dict *props,
			void *(*start)(void*), void *arg);
	/** stop and join a thread */
	int (*join)(void *data, struct pw_thread *thread, void **retval);

	/** get realtime priority range for threads created with \a props */
	int (*get_rt_range) (void *data, const struct spa_dict *props, int *min, int *max);
	/** acquire realtime priority */
	int (*acquire_rt) (void *data, struct pw_thread *thread, int priority);
	/** drop realtime priority */
	int (*drop_rt) (void *data, struct pw_thread *thread);
};

void pw_thread_utils_set_impl(struct pw_thread_utils *impl);
struct pw_thread_utils *pw_thread_utils_get_impl(void);

static inline struct pw_thread *pw_thread_utils_create(const struct spa_dict *props,
		void *(*start_routine)(void*), void *arg)
{
	struct pw_thread *res = NULL;
	spa_interface_call_res(&pw_thread_utils_get_impl()->iface,
			struct pw_thread_utils_methods, res, create, 0,
			props, start_routine, arg);
	return res;
}

static inline int pw_thread_utils_join(struct pw_thread *thread, void **retval)
{
	int res = -ENOTSUP;
	spa_interface_call_res(&pw_thread_utils_get_impl()->iface,
			struct pw_thread_utils_methods, res, join, 0,
			thread, retval);
	return res;
}

static inline int pw_thread_utils_get_rt_range(const struct spa_dict *props, int *min, int *max)
{
	int res = -ENOTSUP;
	spa_interface_call_res(&pw_thread_utils_get_impl()->iface,
			struct pw_thread_utils_methods, res, get_rt_range, 0,
			props, min, max);
	return res;
}

static inline int pw_thread_utils_acquire_rt(struct pw_thread *thread, int priority)
{
	int res = -ENOTSUP;
	spa_interface_call_res(&pw_thread_utils_get_impl()->iface,
			struct pw_thread_utils_methods, res, acquire_rt, 0,
			thread, priority);
	return res;
}

static inline int pw_thread_utils_drop_rt(struct pw_thread *thread)
{
	int res = -ENOTSUP;
	spa_interface_call_res(&pw_thread_utils_get_impl()->iface,
			struct pw_thread_utils_methods, res, drop_rt, 0, thread);
	return res;
}

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* PIPEWIRE_THREAD_H */
