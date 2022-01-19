/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#ifdef __FreeBSD__
#include <sys/thr.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/resource.h>

#include "config.h"

#include <spa/support/dbus.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/impl.h>
#include <pipewire/thread.h>

/** \page page_module_rt PipeWire Module: RT
 *
 * The `rt` module uses the operating system's scheduler to enable realtime
 * scheduling for certain threads to assist with low latency audio processing.
 * This requires `RLIMIT_RTPRIO` to be set to a value that's equal to this
 * module's `rt.prio` parameter or higher. Most distros will come with some
 * package that configures this for certain groups or users. If this is not set
 * up and DBus is available, then this module will fall back to using RTKit.
 */

#define NAME "rt"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define REALTIME_POLICY         SCHED_FIFO
#ifdef SCHED_RESET_ON_FORK
#define PW_SCHED_RESET_ON_FORK  SCHED_RESET_ON_FORK
#else
/* FreeBSD compat */
#define PW_SCHED_RESET_ON_FORK  0
#endif

#define IS_VALID_NICE_LEVEL(l)	((l)>=-20 && (l)<=19)

#define DEFAULT_NICE_LEVEL      20
#define DEFAULT_RT_PRIO		88
#define DEFAULT_RT_TIME_SOFT	-1
#define DEFAULT_RT_TIME_HARD	-1

#define MODULE_USAGE	"[nice.level=<priority: default "SPA_STRINGIFY(DEFAULT_NICE_LEVEL)"(don't change)>] "	\
			"[rt.prio=<priority: default "SPA_STRINGIFY(DEFAULT_RT_PRIO)">] "		\
			"[rt.time.soft=<in usec: default "SPA_STRINGIFY(DEFAULT_RT_TIME_SOFT)"] "	\
			"[rt.time.hard=<in usec: default "SPA_STRINGIFY(DEFAULT_RT_TIME_HARD)"] "

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Use realtime thread scheduling, falling back to RTKit" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct pw_rtkit_bus;

struct thread {
	struct impl *impl;
	struct spa_list link;
	pthread_t thread;
	pid_t pid;
	void *(*start)(void*);
	void *arg;
};

struct impl {
	struct pw_context *context;

	struct spa_thread_utils thread_utils;

	int nice_level;
	int rt_prio;
	rlim_t rt_time_soft;
	rlim_t rt_time_hard;

	struct spa_hook module_listener;

	bool use_rtkit;
	struct pw_rtkit_bus *system_bus;

	/* These are only for the RTKit implementation to fill in the `thread`
	 * struct. Since there's barely any overhead here we'll do this
	 * regardless of which backend is used. */
	pthread_mutex_t lock;
	pthread_cond_t cond;
	struct spa_list threads_list;
};

/***
  Copyright 2009 Lennart Poettering
  Copyright 2010 David Henningsson <diwic@ubuntu.com>

  Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software and associated documentation files
  (the "Software"), to deal in the Software without restriction,
  including without limitation the rights to use, copy, modify, merge,
  publish, distribute, sublicense, and/or sell copies of the Software,
  and to permit persons to whom the Software is furnished to do so,
  subject to the following conditions:

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
  ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
***/

#include <dbus/dbus.h>

#include "config.h"

#include <sys/syscall.h>

#define RTKIT_SERVICE_NAME "org.freedesktop.RealtimeKit1"
#define RTKIT_OBJECT_PATH "/org/freedesktop/RealtimeKit1"

#ifndef RLIMIT_RTTIME
#define RLIMIT_RTTIME 15
#endif

/** \cond */
struct pw_rtkit_bus {
	DBusConnection *bus;
};
/** \endcond */

struct pw_rtkit_bus *pw_rtkit_bus_get_system(void)
{
	struct pw_rtkit_bus *bus;
	DBusError error;

	if (getenv("DISABLE_RTKIT")) {
		errno = ENOTSUP;
		return NULL;
	}

	dbus_error_init(&error);

	bus = calloc(1, sizeof(struct pw_rtkit_bus));
	if (bus == NULL)
		return NULL;

	bus->bus = dbus_bus_get_private(DBUS_BUS_SYSTEM, &error);
	if (bus->bus == NULL)
		goto error;

	dbus_connection_set_exit_on_disconnect(bus->bus, false);

	return bus;

error:
	free(bus);
	pw_log_error("Failed to connect to system bus: %s", error.message);
	dbus_error_free(&error);
	errno = ECONNREFUSED;
	return NULL;
}

void pw_rtkit_bus_free(struct pw_rtkit_bus *system_bus)
{
	dbus_connection_close(system_bus->bus);
	dbus_connection_unref(system_bus->bus);
	free(system_bus);
}

static pid_t _gettid(void)
{
#if defined(HAVE_GETTID)
	return (pid_t) gettid();
#elif defined(__linux__)
	return syscall(SYS_gettid);
#elif defined(__FreeBSD__)
	long pid;
	thr_self(&pid);
	return (pid_t)pid;
#else
#error "No gettid impl"
#endif
}

static int translate_error(const char *name)
{
	pw_log_warn("RTKit error: %s", name);

	if (spa_streq(name, DBUS_ERROR_NO_MEMORY))
		return -ENOMEM;
	if (spa_streq(name, DBUS_ERROR_SERVICE_UNKNOWN) ||
	    spa_streq(name, DBUS_ERROR_NAME_HAS_NO_OWNER))
		return -ENOENT;
	if (spa_streq(name, DBUS_ERROR_ACCESS_DENIED) ||
	    spa_streq(name, DBUS_ERROR_AUTH_FAILED))
		return -EACCES;

	return -EIO;
}

static long long rtkit_get_int_property(struct pw_rtkit_bus *connection, const char *propname,
					long long *propval)
{
	DBusMessage *m = NULL, *r = NULL;
	DBusMessageIter iter, subiter;
	dbus_int64_t i64;
	dbus_int32_t i32;
	DBusError error;
	int current_type;
	long long ret;
	const char *interfacestr = "org.freedesktop.RealtimeKit1";

	dbus_error_init(&error);

	if (!(m = dbus_message_new_method_call(RTKIT_SERVICE_NAME,
					       RTKIT_OBJECT_PATH,
					       "org.freedesktop.DBus.Properties", "Get"))) {
		ret = -ENOMEM;
		goto finish;
	}

	if (!dbus_message_append_args(m,
				      DBUS_TYPE_STRING, &interfacestr,
				      DBUS_TYPE_STRING, &propname, DBUS_TYPE_INVALID)) {
		ret = -ENOMEM;
		goto finish;
	}

	if (!(r = dbus_connection_send_with_reply_and_block(connection->bus, m, -1, &error))) {
		ret = translate_error(error.name);
		goto finish;
	}

	if (dbus_set_error_from_message(&error, r)) {
		ret = translate_error(error.name);
		goto finish;
	}

	ret = -EBADMSG;
	dbus_message_iter_init(r, &iter);
	while ((current_type = dbus_message_iter_get_arg_type(&iter)) != DBUS_TYPE_INVALID) {

		if (current_type == DBUS_TYPE_VARIANT) {
			dbus_message_iter_recurse(&iter, &subiter);

			while ((current_type =
				dbus_message_iter_get_arg_type(&subiter)) != DBUS_TYPE_INVALID) {

				if (current_type == DBUS_TYPE_INT32) {
					dbus_message_iter_get_basic(&subiter, &i32);
					*propval = i32;
					ret = 0;
				}

				if (current_type == DBUS_TYPE_INT64) {
					dbus_message_iter_get_basic(&subiter, &i64);
					*propval = i64;
					ret = 0;
				}

				dbus_message_iter_next(&subiter);
			}
		}
		dbus_message_iter_next(&iter);
	}

finish:

	if (m)
		dbus_message_unref(m);

	if (r)
		dbus_message_unref(r);

	dbus_error_free(&error);

	return ret;
}

int pw_rtkit_get_max_realtime_priority(struct pw_rtkit_bus *connection)
{
	long long retval;
	int err;

	err = rtkit_get_int_property(connection, "MaxRealtimePriority", &retval);
	return err < 0 ? err : retval;
}

int pw_rtkit_get_min_nice_level(struct pw_rtkit_bus *connection, int *min_nice_level)
{
	long long retval;
	int err;

	err = rtkit_get_int_property(connection, "MinNiceLevel", &retval);
	if (err >= 0)
		*min_nice_level = retval;
	return err;
}

long long pw_rtkit_get_rttime_usec_max(struct pw_rtkit_bus *connection)
{
	long long retval;
	int err;

	err = rtkit_get_int_property(connection, "RTTimeUSecMax", &retval);
	return err < 0 ? err : retval;
}

int pw_rtkit_make_realtime(struct pw_rtkit_bus *connection, pid_t thread, int priority)
{
	DBusMessage *m = NULL, *r = NULL;
	dbus_uint64_t u64;
	dbus_uint32_t u32;
	DBusError error;
	int ret;

	dbus_error_init(&error);

	if (thread == 0)
		thread = _gettid();

	if (!(m = dbus_message_new_method_call(RTKIT_SERVICE_NAME,
					       RTKIT_OBJECT_PATH,
					       "org.freedesktop.RealtimeKit1",
					       "MakeThreadRealtime"))) {
		ret = -ENOMEM;
		goto finish;
	}

	u64 = (dbus_uint64_t) thread;
	u32 = (dbus_uint32_t) priority;

	if (!dbus_message_append_args(m,
				      DBUS_TYPE_UINT64, &u64,
				      DBUS_TYPE_UINT32, &u32, DBUS_TYPE_INVALID)) {
		ret = -ENOMEM;
		goto finish;
	}

	if (!(r = dbus_connection_send_with_reply_and_block(connection->bus, m, -1, &error))) {
		ret = translate_error(error.name);
		goto finish;
	}


	if (dbus_set_error_from_message(&error, r)) {
		ret = translate_error(error.name);
		goto finish;
	}

	ret = 0;

finish:

	if (m)
		dbus_message_unref(m);

	if (r)
		dbus_message_unref(r);

	dbus_error_free(&error);

	return ret;
}

int pw_rtkit_make_high_priority(struct pw_rtkit_bus *connection, pid_t thread, int nice_level)
{
	DBusMessage *m = NULL, *r = NULL;
	dbus_uint64_t u64;
	dbus_int32_t s32;
	DBusError error;
	int ret;

	dbus_error_init(&error);

	if (thread == 0)
		thread = _gettid();

	if (!(m = dbus_message_new_method_call(RTKIT_SERVICE_NAME,
					       RTKIT_OBJECT_PATH,
					       "org.freedesktop.RealtimeKit1",
					       "MakeThreadHighPriority"))) {
		ret = -ENOMEM;
		goto finish;
	}

	u64 = (dbus_uint64_t) thread;
	s32 = (dbus_int32_t) nice_level;

	if (!dbus_message_append_args(m,
				      DBUS_TYPE_UINT64, &u64,
				      DBUS_TYPE_INT32, &s32, DBUS_TYPE_INVALID)) {
		ret = -ENOMEM;
		goto finish;
	}



	if (!(r = dbus_connection_send_with_reply_and_block(connection->bus, m, -1, &error))) {
		ret = translate_error(error.name);
		goto finish;
	}


	if (dbus_set_error_from_message(&error, r)) {
		ret = translate_error(error.name);
		goto finish;
	}

	ret = 0;

finish:

	if (m)
		dbus_message_unref(m);

	if (r)
		dbus_message_unref(r);

	dbus_error_free(&error);

	return ret;
}

static void module_destroy(void *data)
{
	struct impl *impl = data;

	pw_thread_utils_set(NULL);
	spa_hook_remove(&impl->module_listener);

	if (impl->system_bus)
		pw_rtkit_bus_free(impl->system_bus);
	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

/**
 * Check if the current user has permissions to use realtime scheduling at the
 * specified priority.
 */
static bool check_realtime_priviliges(rlim_t priority)
{
	int old_policy;
	struct sched_param old_sched_params;
	int new_policy = REALTIME_POLICY;
	struct sched_param new_sched_params;

	/* We could check `RLIMIT_RTPRIO`, but the BSDs generally don't have
	 * that available, and there are also other ways to use realtime
	 * scheduling without that rlimit being set such as `CAP_SYS_NICE` or
	 * running as root. Instead of checking a bunch of preconditions, we
	 * just try if setting realtime scheduling works or not. */
	if ((old_policy = sched_getscheduler(0)) < 0 ||
	    sched_getparam(0, &old_sched_params) != 0) {
		return false;
	}

	/* If the current scheduling policy has `SCHED_RESET_ON_FORK` set, then
	 * this also needs to be set here or `sched_setscheduler()` will return
	 * an error code. Similarly, if it is not set, then we don't want to set
	 * it here as it would irreversible change the current thread's
	 * scheduling policy. */
	spa_zero(new_sched_params);
	new_sched_params.sched_priority = priority;
	if ((old_policy & PW_SCHED_RESET_ON_FORK) != 0) {
		new_policy |= PW_SCHED_RESET_ON_FORK;
	}

	if (sched_setscheduler(0, new_policy, &new_sched_params) == 0) {
		sched_setscheduler(0, old_policy, &old_sched_params);
		return true;
	} else {
		return false;
	}
}

static int set_nice(struct impl *impl, int nice_level)
{
	int res = 0;
	pid_t tid;

	if (impl->use_rtkit) {
		if ((res = pw_rtkit_make_high_priority(impl->system_bus, 0, nice_level)) < 0) {
			pw_log_warn("could not set nice-level to %d: %s",
					nice_level, spa_strerror(res));
		} else {
			pw_log_info("main thread nice level set to %d", nice_level);
		}
	} else {
		tid = _gettid();
		if (setpriority(PRIO_PROCESS, tid, nice_level) == 0) {
			pw_log_info("main thread nice level set to %d",
					nice_level);
		} else {
			res = -errno;
			pw_log_warn("could not set nice-level to %d: %s",
					nice_level, spa_strerror(res));
		}
	}

	return res;
}

static int set_rlimit(struct impl *impl)
{
	struct rlimit rl;
	long long rttime;
	int res = 0;

	spa_zero(rl);
	rl.rlim_cur = impl->rt_time_soft;
	rl.rlim_max = impl->rt_time_hard;

	if (impl->use_rtkit) {
		rttime = pw_rtkit_get_rttime_usec_max(impl->system_bus);
		if (rttime >= 0) {
			if ((rlim_t)rttime < rl.rlim_cur) {
				pw_log_debug("clamping rt.time.soft from %ld to %lld because of RTKit",
					     rl.rlim_cur, rttime);
			}

			rl.rlim_cur = SPA_MIN(rl.rlim_cur, (rlim_t)rttime);
			rl.rlim_max = SPA_MIN(rl.rlim_max, (rlim_t)rttime);
		}
	}

	if (setrlimit(RLIMIT_RTTIME, &rl) < 0)
		res = -errno;

	if (res < 0)
		pw_log_debug("setrlimit() failed: %s", spa_strerror(res));
	else
		pw_log_debug("rt.time.soft:%"PRIi64" rt.time.hard:%"PRIi64,
				(int64_t)rl.rlim_cur, (int64_t)rl.rlim_max);

	return res;
}

static struct thread *find_thread_by_pt(struct impl *impl, pthread_t pt)
{
	struct thread *t;

	spa_list_for_each(t, &impl->threads_list, link) {
		if (pthread_equal(t->thread, pt))
			return t;
	}
	return NULL;
}

static void *custom_start(void *data)
{
	struct thread *this = data;
	struct impl *impl = this->impl;

	pthread_mutex_lock(&impl->lock);
	this->pid = _gettid();
	pthread_cond_broadcast(&impl->cond);
	pthread_mutex_unlock(&impl->lock);

	return this->start(this->arg);
}

static struct spa_thread *impl_create(void *data, const struct spa_dict *props,
		void *(*start_routine)(void*), void *arg)
{
	struct impl *impl = data;
	struct thread *this;
	int err;

	this = calloc(1, sizeof(*this));
	this->impl = impl;
	this->start = start_routine;
	this->arg = arg;

	/* This thread list is only used for the RTKit implementation */
	pthread_mutex_lock(&impl->lock);
	err = pthread_create(&this->thread, NULL, custom_start, this);
	if (err != 0)
		goto exit;

	pthread_cond_wait(&impl->cond, &impl->lock);

	spa_list_append(&impl->threads_list, &this->link);
exit:
	pthread_mutex_unlock(&impl->lock);

	if (err != 0) {
		errno = err;
		free(this);
		return NULL;
	}
	return (struct spa_thread*)this->thread;
}

static int impl_join(void *data, struct spa_thread *thread, void **retval)
{
	struct impl *impl = data;
	pthread_t pt = (pthread_t)thread;
	struct thread *thr;

	pthread_mutex_lock(&impl->lock);
	if ((thr = find_thread_by_pt(impl, pt)) != NULL) {
		spa_list_remove(&thr->link);
		free(thr);
	}
	pthread_mutex_unlock(&impl->lock);

	return pthread_join(pt, retval);
}

static int impl_get_rt_range(void *data, const struct spa_dict *props,
		int *min, int *max)
{
	struct impl *impl = data;
	if (impl->use_rtkit) {
		if (min)
			*min = 1;
		if (max)
			*max = pw_rtkit_get_max_realtime_priority(impl->system_bus);
	} else {
		if (min)
			*min = sched_get_priority_min(REALTIME_POLICY);
		if (max)
			*max = sched_get_priority_max(REALTIME_POLICY);
	}

	return 0;
}

static pid_t impl_gettid(struct impl *impl, pthread_t pt)
{
	struct thread *thr;
	pid_t pid;

	pthread_mutex_lock(&impl->lock);
	if ((thr = find_thread_by_pt(impl, pt)) != NULL)
		pid = thr->pid;
	else
		pid = _gettid();
	pthread_mutex_unlock(&impl->lock);

	return pid;
}

static int impl_acquire_rt(void *data, struct spa_thread *thread, int priority)
{
	struct impl *impl = data;
	struct sched_param sp;
	int err, rtprio_limit;
	pthread_t pt = (pthread_t)thread;
	pid_t pid;

	/* See the docstring on `spa_thread_utils_methods::acquire_rt` */
	if (priority == -1) {
		priority = impl->rt_prio;
	}

	if (impl->use_rtkit) {
		pid = impl_gettid(impl, pt);
		rtprio_limit = pw_rtkit_get_max_realtime_priority(impl->system_bus);
		if (rtprio_limit >= 0 && rtprio_limit < priority) {
			pw_log_info("dropping requested priority %d for thread %d down to %d because of RTKit limits", priority, pid, rtprio_limit);
			priority = rtprio_limit;
		}

		spa_zero(sp);
		sp.sched_priority = priority;

		if (pthread_setschedparam(pt, SCHED_OTHER | PW_SCHED_RESET_ON_FORK, &sp) == 0) {
			pw_log_debug("SCHED_OTHER|SCHED_RESET_ON_FORK worked.");
		}

		if ((err = pw_rtkit_make_realtime(impl->system_bus, pid, priority)) < 0) {
			pw_log_warn("could not make thread %d realtime using RTKit: %s", pid, spa_strerror(err));
		} else {
			pw_log_info("acquired realtime priority %d for thread %d using RTKit", priority, pid);
		}
	} else {
		if (priority < sched_get_priority_min(REALTIME_POLICY) ||
		    priority > sched_get_priority_max(REALTIME_POLICY)) {
			pw_log_warn("invalid priority %d for policy %d", priority, REALTIME_POLICY);
			return -EINVAL;
		}

		spa_zero(sp);
		sp.sched_priority = priority;
		if ((err = pthread_setschedparam(pt, REALTIME_POLICY | PW_SCHED_RESET_ON_FORK, &sp)) != 0) {
			pw_log_warn("could not make thread %p realtime: %s", thread, strerror(err));
			return -err;
		}
		pw_log_info("acquired realtime priority %d for thread %p", priority, thread);
	}

	return 0;
}

static int impl_drop_rt(void *data, struct spa_thread *thread)
{
	struct sched_param sp;
	pthread_t pt = (pthread_t)thread;
	int err;

	spa_zero(sp);
	if ((err = pthread_setschedparam(pt, SCHED_OTHER | PW_SCHED_RESET_ON_FORK, &sp)) != 0) {
		pw_log_debug("thread %p: SCHED_OTHER|SCHED_RESET_ON_FORK failed: %s",
				thread, strerror(err));
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
	const struct pw_properties *context_props;
	struct pw_properties *props;
	const char *str;
	bool use_rtkit = true;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	if ((context_props = pw_context_get_properties(context)) != NULL &&
	    (str = pw_properties_get(context_props, "support.dbus")) != NULL &&
	    !pw_properties_parse_bool(str))
		use_rtkit = false;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

	spa_list_init(&impl->threads_list);
	pthread_mutex_init(&impl->lock, NULL);
	pthread_cond_init(&impl->cond, NULL);

	pw_log_debug("module %p: new", impl);

	props = args ? pw_properties_new_string(args) : pw_properties_new(NULL, NULL);
	if (!props) {
		res = -errno;
		goto error;
	}

	impl->context = context;
	impl->nice_level = pw_properties_get_int32(props, "nice.level", DEFAULT_NICE_LEVEL);
	impl->rt_prio = pw_properties_get_int32(props, "rt.prio", DEFAULT_RT_PRIO);
	impl->rt_time_soft = pw_properties_get_int32(props, "rt.time.soft", DEFAULT_RT_TIME_SOFT);
	impl->rt_time_hard = pw_properties_get_int32(props, "rt.time.hard", DEFAULT_RT_TIME_HARD);

	/* If the user has permissions to use regular realtime scheduling, then
	 * we'll use that instead of RTKit */
	if (check_realtime_priviliges(impl->rt_prio)) {
		use_rtkit = false;
	} else {
		if (!use_rtkit) {
			res = -ENOTSUP;
			pw_log_warn("neither regular realtime scheduling nor RTKit are available");
			goto error;
		}

		/* TODO: Should this be pw_log_warn or pw_log_debug instead? */
		pw_log_info("could not use realtime scheduling, falling back to using RTKit instead");
	}

	impl->use_rtkit = use_rtkit;
	if (impl->use_rtkit) {
		impl->system_bus = pw_rtkit_bus_get_system();
		if (impl->system_bus == NULL) {
			res = -errno;
			pw_log_warn("could not get system bus: %m");
			goto error;
		}
	}

	if (IS_VALID_NICE_LEVEL(impl->nice_level))
		set_nice(impl, impl->nice_level);
	set_rlimit(impl);

	impl->thread_utils.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_ThreadUtils,
			SPA_VERSION_THREAD_UTILS,
			&impl_thread_utils, impl);
	pw_thread_utils_set(&impl->thread_utils);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));
	pw_impl_module_update_properties(module, &props->dict);

	if (impl->use_rtkit) {
		pw_log_debug("initialized using RTKit");
	} else {
		pw_log_debug("initialized using regular realtime scheduling");
	}
	goto done;

error:
	if (impl->system_bus)
		pw_rtkit_bus_free(impl->system_bus);
	free(impl);
done:
	pw_properties_free(props);

	return res;
}
