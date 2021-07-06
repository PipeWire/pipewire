/* PipeWire
 *
 * Copyright © 2021 Axis Communications AB
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

#include <errno.h>
#include <sched.h>
#include <stdbool.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <pthread.h>

#include <spa/utils/dict.h>
#include <spa/utils/result.h>

#include <pipewire/impl.h>
#include <pipewire/thread.h>

#include "config.h"

/** \page page_module_rt PipeWire Module: RT
 */

#define DEFAULT_POLICY	SCHED_FIFO

#define DEFAULT_NICE_LEVEL -11
#define DEFAULT_RT_PRIO 88
#define DEFAULT_RT_TIME_SOFT 2000000
#define DEFAULT_RT_TIME_HARD 2000000

#define MODULE_USAGE \
	"[nice.level=<priority: default " SPA_STRINGIFY(DEFAULT_NICE_LEVEL) ">] " \
	"[rt.prio=<priority: default " SPA_STRINGIFY(DEFAULT_RT_PRIO) ">] " \
	"[rt.time.soft=<in usec: default " SPA_STRINGIFY(DEFAULT_RT_TIME_SOFT)"] " \
	"[rt.time.hard=<in usec: default " SPA_STRINGIFY(DEFAULT_RT_TIME_HARD)"] "

#ifndef RLIMIT_RTTIME
#define RLIMIT_RTTIME 15
#endif

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Jonas Holmberg <jonashg@axis.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Set thread priorities" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_context *context;

	struct spa_thread_utils thread_utils;

	int rt_prio;
	rlim_t rt_time_soft;
	rlim_t rt_time_hard;

	struct spa_hook module_listener;
};

static void module_destroy(void *data)
{
	struct impl *impl = data;
	pw_thread_utils_set(NULL);
	spa_hook_remove(&impl->module_listener);
	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static int set_nice(struct impl *impl, int nice_level)
{
	long tid;
	int res = 0;

	tid = syscall(SYS_gettid);
	if (tid < 0) {
		pw_log_warn("could not get main thread id: %m");
		tid = 0; /* means current thread in setpriority() on linux */
	}
	if (setpriority(PRIO_PROCESS, (id_t)tid, nice_level) < 0)
		res = -errno;

	if (res < 0)
		pw_log_warn("could not set nice-level to %d: %s",
				nice_level, spa_strerror(res));
	else
		pw_log_info("main thread nice level set to %d",
				nice_level);

	return res;
}

static int set_rlimit(struct impl *impl)
{
	struct rlimit rl;
	int res = 0;

	rl.rlim_cur = impl->rt_time_soft;
	rl.rlim_max = impl->rt_time_hard;

	if (setrlimit(RLIMIT_RTTIME, &rl) < 0)
		res = -errno;

	if (res < 0)
		pw_log_warn("could not set rlimit: %s", spa_strerror(res));
	else
		pw_log_debug("rt.time.soft %"PRIi64", rt.time.hard %"PRIi64,
				(int64_t)rl.rlim_cur, (int64_t)rl.rlim_max);

	return res;
}

static int get_default_int(struct pw_properties *properties, const char *name, int def)
{
	const char *str;
	int val;
	bool set_default = true;

	if ((str = pw_properties_get(properties, name)) != NULL) {
		char *endptr;

		val = (int)strtol(str, &endptr, 10);
		if (*endptr == '\0')
			set_default = false;
		else
			pw_log_warn("invalid integer value '%s' of property %s, using default (%d) instead", str, name, def);
	}

	if (set_default) {
		val = def;
		pw_properties_setf(properties, name, "%d", val);
	}

	return val;
}

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
	int policy = DEFAULT_POLICY;
	if (min)
		*min = sched_get_priority_min(policy);
	if (max)
		*max = sched_get_priority_max(policy);
	return 0;
}

static int impl_acquire_rt(void *data, struct spa_thread *thread, int priority)
{
	int err, policy = DEFAULT_POLICY;
	int rtprio = priority;
	struct sched_param sp;
	pthread_t pt = (pthread_t)thread;

        if (rtprio < sched_get_priority_min(policy) ||
            rtprio > sched_get_priority_max(policy)) {
		pw_log_warn("invalid priority %d for policy %d", rtprio, policy);
		return -EINVAL;
	}

	spa_zero(sp);
	sp.sched_priority = rtprio;
	if ((err = pthread_setschedparam(pt, policy | SCHED_RESET_ON_FORK,
                                &sp)) != 0) {
		pw_log_warn("%p: could not make thread realtime: %s", thread, strerror(err));
		return -err;
        }
	pw_log_info("thread %p has realtime priority %d", thread, rtprio);
	return 0;
}

static int impl_drop_rt(void *data, struct spa_thread *thread)
{
	struct sched_param sp;
	pthread_t pt = (pthread_t)thread;
	int err;

	spa_zero(sp);
	if ((err = pthread_setschedparam(pt,
				SCHED_OTHER | SCHED_RESET_ON_FORK, &sp)) != 0) {
		pw_log_warn("%p: could not drop realtime: %s", thread, strerror(err));
		return -err;
        }
	pw_log_info("thread %p dropped realtime priority", thread);
	return 0;
}

static const struct spa_thread_utils_methods impl_thread_utils = {
	SPA_VERSION_THREAD_UTILS_METHODS,
	.create = impl_create,
	.join = impl_join,
	.get_rt_range = impl_get_rt_range,
	.acquire_rt = impl_acquire_rt,
	.drop_rt = impl_drop_rt,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	struct pw_properties *props;
	int nice_level;
	int res;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

	pw_log_debug("module %p: new %s", impl, args);

	impl->context = context;
	props = args ? pw_properties_new_string(args) : pw_properties_new(NULL, NULL);
	if (props == NULL) {
		res = -errno;
		goto error;
	}

	nice_level = get_default_int(props, "nice.level", DEFAULT_NICE_LEVEL);
	set_nice(impl, nice_level);

	impl->rt_prio = get_default_int(props, "rt.prio", DEFAULT_RT_PRIO);
	impl->rt_time_soft = get_default_int(props, "rt.time.soft", DEFAULT_RT_TIME_SOFT);
	impl->rt_time_hard = get_default_int(props, "rt.time.hard", DEFAULT_RT_TIME_HARD);

	set_rlimit(impl);

	impl->thread_utils.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_ThreadUtils,
			SPA_VERSION_THREAD_UTILS,
			&impl_thread_utils, impl);

	pw_thread_utils_set(&impl->thread_utils);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));
	pw_impl_module_update_properties(module, &props->dict);
	pw_properties_free(props);

	return 0;

error:
	pw_properties_free(props);
	free(impl);
	return res;
}
