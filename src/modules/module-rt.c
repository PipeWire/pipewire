/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */
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

#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#if defined(__FreeBSD__) || defined(__MidnightBSD__)
#include <sys/thr.h>
#endif
#if defined(__GNU__)
#include <hurd.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>

#include <pipewire/impl.h>
#include <pipewire/thread.h>

#ifdef HAVE_DBUS
#include <spa/support/dbus.h>
#include <dbus/dbus.h>
#endif

/** \page page_module_rt RT
 *
 * The `rt` modules can give real-time priorities to processing threads.
 *
 * It uses the operating system's scheduler to enable realtime scheduling
 * for certain threads to assist with low latency audio processing.
 * This requires `RLIMIT_RTPRIO` to be set to a value that's equal to this
 * module's `rt.prio` parameter or higher. Most distros will come with some
 * package that configures this for certain groups or users. If this is not set
 * up and DBus is available, then this module will fall back to using the Portal
 * Realtime DBus API or RTKit.
 *
 * ## Module Name
 *
 * `libpipewire-module-rt`
 *
 * ## Module Options
 *
 * - `nice.level`: The nice value set for the application thread. It improves
 *                 performance of the communication with the pipewire daemon.
 * - `rt.prio`: The realtime priority of the data thread. Higher values are
 *              higher priority.
 * - `rt.time.soft`, `rt.time.hard`: The amount of CPU time an RT thread can
 *              consume without doing any blocking calls before the kernel kills
 *              the thread. This is a safety measure to avoid lockups of the complete
 *              system when some thread consumes 100%.
 * - `rlimits.enabled`: enable the use of rtlimits, default true.
 * - `rtportal.enabled`: enable the use of realtime portal, default true
 * - `rtkit.enabled`: enable the use of rtkit, default true
 * - `uclamp.min`: the minimum utilisation value the scheduler should consider
 * - `uclamp.max`: the maximum utilisation value the scheduler should consider

 * The nice level is by default set to an invalid value so that clients don't
 * automatically have the nice level raised.
 *
 * The PipeWire server processes are explicitly configured with a valid nice level.
 *
 * ## Example configuration
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-rt
 *     args = {
 *         #nice.level   = 20
 *         #rt.prio      = 88
 *         #rt.time.soft = -1
 *         #rt.time.hard = -1
 *         #rlimits.enabled = true
 *         #rtportal.enabled = true
 *         #rtkit.enabled = true
 *         #uclamp.min = 0
 *         #uclamp.max = 1024
 *     }
 *     flags = [ ifexists nofail ]
 * }
 * ]
 *\endcode
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

#define MIN_NICE_LEVEL		-20
#define MAX_NICE_LEVEL		19
#define IS_VALID_NICE_LEVEL(l)	((l)>=MIN_NICE_LEVEL && (l)<=MAX_NICE_LEVEL)

#define DEFAULT_NICE_LEVEL	20 	/* invalid value by default, see above */
#define DEFAULT_RT_PRIO_MIN	11
#define DEFAULT_RT_PRIO		RTPRIO_CLIENT
#define DEFAULT_RT_TIME_SOFT	-1
#define DEFAULT_RT_TIME_HARD	-1

#define DEFAULT_UCLAMP_MIN      0
#define DEFAULT_UCLAMP_MAX      1024

#define MODULE_USAGE	"( nice.level=<priority: default "SPA_STRINGIFY(DEFAULT_NICE_LEVEL)"(don't change)> ) "	\
			"( rt.prio=<priority: default "SPA_STRINGIFY(DEFAULT_RT_PRIO)"> ) "		\
			"( rt.time.soft=<in usec: default "SPA_STRINGIFY(DEFAULT_RT_TIME_SOFT)"> ) "	\
			"( rt.time.hard=<in usec: default "SPA_STRINGIFY(DEFAULT_RT_TIME_HARD)"> ) "	\
			"( rlimits.enabled=<default true> ) " \
			"( rtportal.enabled=<default true> ) " \
			"( rtkit.enabled=<default true> ) " \
			"( uclamp.min=<default "SPA_STRINGIFY(DEFAULT_UCLAMP_MIN)"> ) " \
			"( uclamp.max=<default "SPA_STRINGIFY(DEFAULT_UCLAMP_MAX)"> )"

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Use realtime thread scheduling, falling back to RTKit" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#ifdef HAVE_DBUS
#define RTKIT_SERVICE_NAME "org.freedesktop.RealtimeKit1"
#define RTKIT_OBJECT_PATH "/org/freedesktop/RealtimeKit1"
#define RTKIT_INTERFACE "org.freedesktop.RealtimeKit1"

#define XDG_PORTAL_SERVICE_NAME "org.freedesktop.portal.Desktop"
#define XDG_PORTAL_OBJECT_PATH "/org/freedesktop/portal/desktop"
#define XDG_PORTAL_INTERFACE "org.freedesktop.portal.Realtime"

/** \cond */
struct pw_rtkit_bus {
	DBusConnection *bus;
};
/** \endcond */

struct thread {
	struct impl *impl;
	struct spa_list link;
	pthread_t thread;
	pid_t pid;
	void *(*start)(void*);
	void *arg;
};
#endif /* HAVE_DBUS */

struct impl {
	struct pw_context *context;

	struct spa_thread_utils thread_utils;

	pid_t main_pid;
	struct rlimit rl;
	int nice_level;
	int rt_prio;
	rlim_t rt_time_soft;
	rlim_t rt_time_hard;

	int uclamp_min;
	int uclamp_max;

	struct spa_hook module_listener;

	unsigned rlimits_enabled:1;
	unsigned rtportal_enabled:1;
	unsigned rtkit_enabled:1;

#ifdef HAVE_DBUS
	bool use_rtkit;
	/* For D-Bus. These are const static. */
	const char* service_name;
	const char* object_path;
	const char* interface;
	struct pw_rtkit_bus *rtkit_bus;
	struct pw_thread_loop *thread_loop;
	int max_rtprio;
	int min_nice_level;
	rlim_t rttime_max;

	/* These are only for the RTKit implementation to fill in the `thread`
	 * struct. Since there's barely any overhead here we'll do this
	 * regardless of which backend is used. */
	pthread_mutex_t lock;
	pthread_cond_t cond;
	struct spa_list threads_list;
#endif
};

#ifndef RLIMIT_RTTIME
#define RLIMIT_RTTIME 15
#endif

static pthread_mutex_t rlimit_lock = PTHREAD_MUTEX_INITIALIZER;

static pid_t _gettid(void)
{
#if defined(HAVE_GETTID)
	return (pid_t) gettid();
#elif defined(__linux__)
	return syscall(SYS_gettid);
#elif defined(__FreeBSD__) || defined(__MidnightBSD__)
	long pid;
	thr_self(&pid);
	return (pid_t)pid;
#elif defined(__GNU__)
       mach_port_t thread = hurd_thread_self();
       return (pid_t)thread;
#else
#error "No gettid impl"
#endif
}

#ifdef HAVE_DBUS
static struct pw_rtkit_bus *pw_rtkit_bus_get(DBusBusType bus_type)
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

	bus->bus = dbus_bus_get_private(bus_type, &error);
	if (bus->bus == NULL)
		goto error;

	dbus_connection_set_exit_on_disconnect(bus->bus, false);

	return bus;

error:
	free(bus);
	pw_log_error("Failed to connect to %s bus: %s",
		     bus_type == DBUS_BUS_SYSTEM ? "system" : "session", error.message);
	dbus_error_free(&error);
	errno = ECONNREFUSED;
	return NULL;
}

static struct pw_rtkit_bus *pw_rtkit_bus_get_system(void)
{
	return pw_rtkit_bus_get(DBUS_BUS_SYSTEM);
}

static struct pw_rtkit_bus *pw_rtkit_bus_get_session(void)
{
	return pw_rtkit_bus_get(DBUS_BUS_SESSION);
}

static bool pw_rtkit_check_xdg_portal(struct pw_rtkit_bus *system_bus)
{
	if (!dbus_bus_name_has_owner(system_bus->bus, XDG_PORTAL_SERVICE_NAME, NULL)) {
		pw_log_info("Can't find %s. Is xdg-desktop-portal running?", XDG_PORTAL_SERVICE_NAME);
		return false;
	}

	return true;
}

static void pw_rtkit_bus_free(struct pw_rtkit_bus *system_bus)
{
	dbus_connection_close(system_bus->bus);
	dbus_connection_unref(system_bus->bus);
	free(system_bus);
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
	if (spa_streq(name, DBUS_ERROR_IO_ERROR))
		return -EIO;
	if (spa_streq(name, DBUS_ERROR_NOT_SUPPORTED))
		return -ENOTSUP;
	if (spa_streq(name, DBUS_ERROR_INVALID_ARGS))
		return -EINVAL;
	if (spa_streq(name, DBUS_ERROR_TIMED_OUT))
		return -ETIMEDOUT;
	return -EIO;
}

static long long rtkit_get_int_property(struct impl *impl, const char *propname,
					long long *propval)
{
	DBusMessage *m = NULL, *r = NULL;
	DBusMessageIter iter, subiter;
	dbus_int64_t i64;
	dbus_int32_t i32;
	DBusError error;
	int current_type;
	long long ret;
	struct pw_rtkit_bus *connection = impl->rtkit_bus;

	dbus_error_init(&error);

	if (!(m = dbus_message_new_method_call(impl->service_name,
					       impl->object_path,
					       "org.freedesktop.DBus.Properties", "Get"))) {
		ret = -ENOMEM;
		goto finish;
	}

	if (!dbus_message_append_args(m,
				      DBUS_TYPE_STRING, &impl->interface,
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

static int pw_rtkit_make_realtime(struct impl *impl, pid_t thread, int priority)
{
	DBusMessage *m = NULL;
	dbus_uint64_t pid;
	dbus_uint64_t u64;
	dbus_uint32_t u32, serial;
	DBusError error;
	int ret;
	struct pw_rtkit_bus *connection = impl->rtkit_bus;

	dbus_error_init(&error);

	if (thread == 0)
		thread = _gettid();

	if (!(m = dbus_message_new_method_call(impl->service_name,
					       impl->object_path, impl->interface,
					       "MakeThreadRealtimeWithPID"))) {
		ret = -ENOMEM;
		goto finish;
	}

	pid = (dbus_uint64_t) getpid();
	u64 = (dbus_uint64_t) thread;
	u32 = (dbus_uint32_t) priority;

	if (!dbus_message_append_args(m,
				      DBUS_TYPE_UINT64, &pid,
				      DBUS_TYPE_UINT64, &u64,
				      DBUS_TYPE_UINT32, &u32, DBUS_TYPE_INVALID)) {
		ret = -ENOMEM;
		goto finish;
	}

	if (!dbus_connection_send(connection->bus, m, &serial)) {
		ret = translate_error(error.name);
		goto finish;
	}
	ret = 0;

finish:

	if (m)
		dbus_message_unref(m);

	return ret;
}


static int pw_rtkit_make_high_priority(struct impl *impl, pid_t thread, int nice_level)
{
	DBusMessage *m = NULL;
	dbus_uint64_t pid;
	dbus_uint64_t u64;
	dbus_int32_t s32;
	dbus_uint32_t serial;
	int ret;
	struct pw_rtkit_bus *connection = impl->rtkit_bus;

	if (thread == 0)
		thread = _gettid();

	if (!(m = dbus_message_new_method_call(impl->service_name,
					       impl->object_path, impl->interface,
					       "MakeThreadHighPriorityWithPID"))) {
		ret = -ENOMEM;
		goto finish;
	}

	pid = (dbus_uint64_t) getpid();
	u64 = (dbus_uint64_t) thread;
	s32 = (dbus_int32_t) nice_level;

	if (!dbus_message_append_args(m,
				      DBUS_TYPE_UINT64, &pid,
				      DBUS_TYPE_UINT64, &u64,
				      DBUS_TYPE_INT32, &s32, DBUS_TYPE_INVALID)) {
		ret = -ENOMEM;
		goto finish;
	}
	if (!dbus_connection_send(connection->bus, m, &serial)) {
		ret = -EIO;
		goto finish;
	}
	ret = 0;

finish:

	if (m)
		dbus_message_unref(m);

	return ret;
}
#endif /* HAVE_DBUS */

static void module_destroy(void *data)
{
	struct impl *impl = data;
	pw_context_set_object(impl->context, SPA_TYPE_INTERFACE_ThreadUtils, NULL);
	spa_hook_remove(&impl->module_listener);

#ifdef HAVE_DBUS
	if (impl->thread_loop)
		pw_thread_loop_destroy(impl->thread_loop);
	if (impl->rtkit_bus)
		pw_rtkit_bus_free(impl->rtkit_bus);
#endif

	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static int get_rt_priority_range(int *out_min, int *out_max)
{
	int min, max;

	if ((min = sched_get_priority_min(REALTIME_POLICY)) < 0)
		return -errno;
	if ((max = sched_get_priority_max(REALTIME_POLICY)) < 0)
		return -errno;

	if (out_min)
		*out_min = min;
	if (out_max)
		*out_max = max;

	return 0;
}
/**
 * Check if the current user has permissions to use realtime scheduling at the
 * specified priority.
 */
static bool check_realtime_privileges(struct impl *impl)
{
	rlim_t priority = impl->rt_prio;
	int err, old_policy, new_policy, min, max;
	struct sched_param old_sched_params;
	struct sched_param new_sched_params;
	struct rlimit old_rlim;
	struct rlimit no_rlim = { -1, -1 };
	int try = 0;
	bool ret = false;

	if (!impl->rlimits_enabled)
		return ret;

	while (!ret && try++ < 2) {
		/* We could check `RLIMIT_RTPRIO`, but the BSDs generally don't have
		 * that available, and there are also other ways to use realtime
		 * scheduling without that rlimit being set such as `CAP_SYS_NICE` or
		 * running as root. Instead of checking a bunch of preconditions, we
		 * just try if setting realtime scheduling works or not. */
		if ((err = pthread_getschedparam(pthread_self(), &old_policy, &old_sched_params)) != 0) {
			pw_log_warn("Failed to check RLIMIT_RTPRIO: %s", strerror(err));
			break;
		}
		if ((err = get_rt_priority_range(&min, &max)) < 0) {
			pw_log_warn("Failed to get priority range: %s", strerror(err));
			break;
		}
		if (try == 2) {
#ifdef RLIMIT_RTPRIO
			struct rlimit rlim;
			/* second try, try to clamp to RLIMIT_RTPRIO */
			if (getrlimit(RLIMIT_RTPRIO, &rlim) == 0 && max > (int)rlim.rlim_max) {
				pw_log_info("Clamp rtprio %d to %d", (int)priority, (int)rlim.rlim_max);
				max = (int)rlim.rlim_max;
			}
			else
#endif
				break;
		}
		if (max < DEFAULT_RT_PRIO_MIN) {
			pw_log_info("Priority max (%d) must be at least %d", max, DEFAULT_RT_PRIO_MIN);
			break;
		}

		/* If the current scheduling policy has `SCHED_RESET_ON_FORK` set, then
		 * this also needs to be set here or `pthread_setschedparam()` will return
		 * an error code. Similarly, if it is not set, then we don't want to set
		 * it here as it would irreversible change the current thread's
		 * scheduling policy. */
		spa_zero(new_sched_params);
		new_sched_params.sched_priority = SPA_CLAMP((int)priority, min, max);
		new_policy = REALTIME_POLICY;
		if ((old_policy & PW_SCHED_RESET_ON_FORK) != 0)
			new_policy |= PW_SCHED_RESET_ON_FORK;

		/* Disable RLIMIT_RTTIME in a thread safe way and hope that the application
		 * doesn't also set RLIMIT_RTTIME while trying new_policy. */
		pthread_mutex_lock(&rlimit_lock);
		if (getrlimit(RLIMIT_RTTIME, &old_rlim) < 0)
			pw_log_info("getrlimit() failed: %m");
		if (setrlimit(RLIMIT_RTTIME, &no_rlim) < 0)
			pw_log_info("setrlimit() failed: %m");
		if ((err = pthread_setschedparam(pthread_self(), new_policy, &new_sched_params)) == 0) {
			impl->rt_prio = new_sched_params.sched_priority;
			pthread_setschedparam(pthread_self(), old_policy, &old_sched_params);
			ret = true;
		} else
			pw_log_info("failed to set realtime policy: %s", strerror(err));
		if (setrlimit(RLIMIT_RTTIME, &old_rlim) < 0)
			pw_log_info("setrlimit() failed: %m");
		pthread_mutex_unlock(&rlimit_lock);
	}

	if (ret)
		pw_log_debug("can set rt prio to %d", (int)priority);
	else
		pw_log_info("can't set rt prio to %d (try increasing rlimits)", (int)priority);
	return ret;
}

static int sched_set_nice(pid_t pid, int nice_level)
{
	if (setpriority(PRIO_PROCESS, pid, nice_level) == 0)
		return 0;
	else
		return -errno;
}

static int set_nice(struct impl *impl, int nice_level, bool warn)
{
	int res = 0;

#ifdef HAVE_DBUS
	if (impl->use_rtkit) {
		if (nice_level < impl->min_nice_level) {
			pw_log_info("clamped nice level %d to %d",
					nice_level, impl->min_nice_level);
			nice_level = impl->min_nice_level;
		}
		res = pw_rtkit_make_high_priority(impl, impl->main_pid, nice_level);
	}
	else
#endif
	if (impl->rlimits_enabled)
		res = sched_set_nice(impl->main_pid, nice_level);
	else
		res = -ENOTSUP;

	if (res < 0) {
		if (warn)
			pw_log_warn("could not set nice-level to %d: %s",
					nice_level, spa_strerror(res));
	} else if (res > 0) {
		pw_log_info("main thread setting nice level to %d: %s",
				nice_level, spa_strerror(-res));
	} else {
		pw_log_info("main thread nice level set to %d",
				nice_level);
	}
	return res;
}

static int set_rlimit(struct rlimit *rlim)
{
	int res = 0;

	pthread_mutex_lock(&rlimit_lock);
	if (setrlimit(RLIMIT_RTTIME, rlim) < 0)
		res = -errno;
	pthread_mutex_unlock(&rlimit_lock);

	if (res < 0)
		pw_log_info("setrlimit() failed: %s", spa_strerror(res));
	else
		pw_log_debug("rt.time.soft:%"PRIi64" rt.time.hard:%"PRIi64,
				(int64_t)rlim->rlim_cur, (int64_t)rlim->rlim_max);

	return res;
}

static int acquire_rt_sched(struct spa_thread *thread, int priority)
{
	int err, min, max;
	struct sched_param sp;
	pthread_t pt = (pthread_t)thread;

	if ((err = get_rt_priority_range(&min, &max)) < 0)
		return err;

	if (priority < min || priority > max) {
		pw_log_info("clamping priority %d to range %d - %d for policy %d",
				priority, min, max, REALTIME_POLICY);
		priority = SPA_CLAMP(priority, min, max);
	}

	spa_zero(sp);
	sp.sched_priority = priority;
	if ((err = pthread_setschedparam(pt, REALTIME_POLICY | PW_SCHED_RESET_ON_FORK, &sp)) != 0) {
		pw_log_warn("could not make thread %p realtime: %s", thread, strerror(err));
		return -err;
	}

	pw_log_info("acquired realtime priority %d for thread %p", priority, thread);
	return 0;
}

static int impl_drop_rt_generic(void *object, struct spa_thread *thread)
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

#ifdef HAVE_DBUS
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

static struct spa_thread *impl_create(void *object, const struct spa_dict *props,
		void *(*start_routine)(void*), void *arg)
{
	struct impl *impl = object;
	struct thread *this;
	struct spa_thread *thread;

	this = calloc(1, sizeof(*this));
	this->impl = impl;
	this->start = start_routine;
	this->arg = arg;

	/* This thread list is only used for the RTKit implementation */
	pthread_mutex_lock(&impl->lock);
	thread = pw_thread_utils_create(props, custom_start, this);
	if (thread == NULL)
		goto exit;

	this->thread = (pthread_t)thread;
	pthread_cond_wait(&impl->cond, &impl->lock);

	spa_list_append(&impl->threads_list, &this->link);
exit:
	pthread_mutex_unlock(&impl->lock);

	if (thread == NULL) {
		free(this);
		return NULL;
	}
	return thread;
}

static int impl_join(void *object, struct spa_thread *thread, void **retval)
{
	struct impl *impl = object;
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


static int get_rtkit_priority_range(struct impl *impl, int *min, int *max)
{
	if (min)
		*min = 1;
	if (max) {
		*max = impl->max_rtprio;
		if (*max < 1)
			*max = 1;
	}
	return 0;
}

static int impl_get_rt_range(void *object, const struct spa_dict *props,
		int *min, int *max)
{
	struct impl *impl = object;
	int res;
	if (impl->use_rtkit)
		res = get_rtkit_priority_range(impl, min, max);
	else
		res = get_rt_priority_range(min, max);
	return res;
}

struct rt_params {
	pid_t pid;
	int priority;
};

static int do_make_realtime(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	const struct rt_params *params = data;
	int err, min, max, priority = params->priority;
	pid_t pid = params->pid;

	pw_log_debug("rtkit realtime");

	if ((err = get_rtkit_priority_range(impl, &min, &max)) < 0)
		return err;

	if (priority < min || priority > max) {
		pw_log_info("clamping requested priority %d for thread %d "
				"between %d  and %d", priority, pid, min, max);
		priority = SPA_CLAMP(priority, min, max);
	}

	if ((err = pw_rtkit_make_realtime(impl, pid, priority)) < 0) {
		pw_log_warn("could not make thread %d realtime using RTKit: %s", pid, spa_strerror(err));
		return err;
	}

	pw_log_info("acquired realtime priority %d for thread %d using RTKit", priority, pid);
	return 0;
}

static int impl_acquire_rt(void *object, struct spa_thread *thread, int priority)
{
	struct impl *impl = object;
	struct sched_param sp;
	pthread_t pt = (pthread_t)thread;
	int res;

	/* See the docstring on `spa_thread_utils_methods::acquire_rt` */
	if (priority == -1) {
		priority = impl->rt_prio;
	}
	if (impl->use_rtkit) {
		struct rt_params params;
		struct thread *thr;

		spa_zero(sp);
		if (pthread_setschedparam(pt, SCHED_OTHER | PW_SCHED_RESET_ON_FORK, &sp) == 0) {
			pw_log_debug("SCHED_OTHER|SCHED_RESET_ON_FORK worked.");
		}

		params.priority = priority;

		pthread_mutex_lock(&impl->lock);
		if ((thr = find_thread_by_pt(impl, pt)) != NULL)
			params.pid = thr->pid;
		else
			params.pid = _gettid();

		res = pw_loop_invoke(pw_thread_loop_get_loop(impl->thread_loop),
				do_make_realtime, 0, &params, sizeof(params), false, impl);
		pthread_mutex_unlock(&impl->lock);

		return res;
	} else {
		return acquire_rt_sched(thread, priority);
	}
}

static const struct spa_thread_utils_methods impl_thread_utils = {
	SPA_VERSION_THREAD_UTILS_METHODS,
	.create = impl_create,
	.join = impl_join,
	.get_rt_range = impl_get_rt_range,
	.acquire_rt = impl_acquire_rt,
	.drop_rt = impl_drop_rt_generic,
};

#else /* HAVE_DBUS */

static struct spa_thread *impl_create(void *object, const struct spa_dict *props,
		void *(*start_routine)(void*), void *arg)
{
	return pw_thread_utils_create(props, start_routine, arg);
}

static int impl_join(void *object, struct spa_thread *thread, void **retval)
{
	return pw_thread_utils_join(thread, retval);
}

static int impl_get_rt_range(void *object, const struct spa_dict *props,
		int *min, int *max)
{
	return get_rt_priority_range(min, max);
}

static int impl_acquire_rt(void *object, struct spa_thread *thread, int priority)
{
	struct impl *impl = object;

	/* See the docstring on `spa_thread_utils_methods::acquire_rt` */
	if (priority == -1)
		priority = impl->rt_prio;

	return acquire_rt_sched(thread, priority);
}

static const struct spa_thread_utils_methods impl_thread_utils = {
	SPA_VERSION_THREAD_UTILS_METHODS,
	.create = impl_create,
	.join = impl_join,
	.get_rt_range = impl_get_rt_range,
	.acquire_rt = impl_acquire_rt,
	.drop_rt = impl_drop_rt_generic,
};
#endif /* HAVE_DBUS */


#ifdef HAVE_DBUS
static int check_rtkit(struct impl *impl, struct pw_context *context, bool *can_use_rtkit)
{
	const struct pw_properties *context_props;
	const char *str;

	*can_use_rtkit = true;

	if ((context_props = pw_context_get_properties(context)) != NULL &&
	    (str = pw_properties_get(context_props, "support.dbus")) != NULL &&
	    !pw_properties_parse_bool(str))
		*can_use_rtkit = false;

	return 0;
}

static int rtkit_get_bus(struct impl *impl)
{
	int res;

	pw_log_debug("enter rtkit get bus");

	/* Checking xdg-desktop-portal. It works fine in all situations. */
	if (impl->rtportal_enabled)
		impl->rtkit_bus = pw_rtkit_bus_get_session();
	else
		pw_log_info("Portal Realtime disabled");

	if (impl->rtkit_bus != NULL) {
		if (pw_rtkit_check_xdg_portal(impl->rtkit_bus)) {
			impl->service_name = XDG_PORTAL_SERVICE_NAME;
			impl->object_path = XDG_PORTAL_OBJECT_PATH;
			impl->interface = XDG_PORTAL_INTERFACE;
		} else {
			pw_log_info("found session bus but no portal, trying RTKit fallback");
			pw_rtkit_bus_free(impl->rtkit_bus);
			impl->rtkit_bus = NULL;
		}
	}
	/* Failed to get xdg-desktop-portal, try to use rtkit. */
	if (impl->rtkit_bus == NULL) {
		if (impl->rtkit_enabled)
			impl->rtkit_bus = pw_rtkit_bus_get_system();
		else
			pw_log_info("RTkit disabled");

		if (impl->rtkit_bus != NULL) {
			impl->service_name = RTKIT_SERVICE_NAME;
			impl->object_path = RTKIT_OBJECT_PATH;
			impl->interface = RTKIT_INTERFACE;
		} else {
			res = -errno;
			pw_log_warn("Realtime scheduling disabled: unsufficient realtime privileges, "
				"Portal not found on session bus, and no system bus for RTKit: %m");
			return res;
		}
	}

	return 0;
}

static int do_rtkit_setup(struct spa_loop *loop, bool async, uint32_t seq,
		const void *data, size_t size, void *user_data)
{
	struct impl *impl = user_data;
	long long retval;

	pw_log_debug("enter rtkit setup");

	/* get some properties */
	if (rtkit_get_int_property(impl, "MaxRealtimePriority", &retval) < 0) {
		retval = 1;
		pw_log_warn("RTKit does not give us MaxRealtimePriority, using %lld", retval);
	}
	impl->max_rtprio = retval;
	if (rtkit_get_int_property(impl, "MinNiceLevel", &retval) < 0) {
		retval = 0;
		pw_log_warn("RTKit does not give us MinNiceLevel, using %lld", retval);
	}
	impl->min_nice_level = retval;
	if (rtkit_get_int_property(impl, "RTTimeUSecMax", &retval) < 0) {
		retval = impl->rl.rlim_cur;
		pw_log_warn("RTKit does not give us RTTimeUSecMax, using %lld", retval);
	}
	impl->rttime_max = retval;

	/* Retry set_nice with rtkit */
	if (IS_VALID_NICE_LEVEL(impl->nice_level))
		set_nice(impl, impl->nice_level, true);

	/* Set rlimit with rtkit limits */
	if (impl->rttime_max < impl->rl.rlim_cur) {
		pw_log_debug("clamping rt.time.soft from %llu to %lld because of RTKit",
			     (long long)impl->rl.rlim_cur, (long long)impl->rttime_max);
	}
	impl->rl.rlim_cur = SPA_MIN(impl->rl.rlim_cur, impl->rttime_max);
	impl->rl.rlim_max = SPA_MIN(impl->rl.rlim_max, impl->rttime_max);

	set_rlimit(&impl->rl);

	return 0;
}
#endif /* HAVE_DBUS */

int set_uclamp(int uclamp_min, int uclamp_max, pid_t pid) {
#ifdef __linux__
	int ret;
	struct sched_attr {
		uint32_t size;
		uint32_t sched_policy;
		uint64_t sched_flags;
		int32_t sched_nice;
		uint32_t sched_priority;
		uint64_t sched_runtime;
		uint64_t sched_deadline;
		uint64_t sched_period;
		uint32_t sched_util_min;
		uint32_t sched_util_max;
	} attr;

	ret = syscall(SYS_sched_getattr, pid, &attr, sizeof(struct sched_attr), 0);
	if (ret) {
		pw_log_warn("Could not retrieve scheduler attributes: %d", -errno);
		return -errno;
	}

	/* SCHED_FLAG_KEEP_POLICY |
	 * SCHED_FLAG_KEEP_PARAMS |
	 * SCHED_FLAG_UTIL_CLAMP_MIN |
	 * SCHED_FLAG_UTIL_CLAMP_MAX */
	attr.sched_flags = 0x8 | 0x10 | 0x20 | 0x40;
	attr.sched_util_min = uclamp_min;
	attr.sched_util_max = uclamp_max;

	ret = syscall(SYS_sched_setattr, pid, &attr, 0);

	if (ret) {
		pw_log_warn("Could not set scheduler attributes: %d", -errno);
		return -errno;
	}
	return 0;
#else
	pw_log_warn("Setting UCLAMP values is only supported on Linux");
	return -EOPNOTSUPP;
#endif /* __linux__ */
}


SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	struct pw_properties *props;
	int res = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

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
	impl->rlimits_enabled = pw_properties_get_bool(props, "rlimits.enabled", true);
	impl->rtportal_enabled = pw_properties_get_bool(props, "rtportal.enabled", true);
	impl->rtkit_enabled = pw_properties_get_bool(props, "rtkit.enabled", true);
	impl->uclamp_min = pw_properties_get_int32(props, "uclamp.min", DEFAULT_UCLAMP_MIN);
	impl->uclamp_max = pw_properties_get_int32(props, "uclamp.max", DEFAULT_UCLAMP_MAX);

	impl->rl.rlim_cur = impl->rt_time_soft;
	impl->rl.rlim_max = impl->rt_time_hard;
	impl->main_pid = _gettid();

	bool can_use_rtkit = false, use_rtkit = false;

	if (!IS_VALID_NICE_LEVEL(impl->nice_level)) {
		pw_log_info("invalid nice level %d (not between %d and %d). "
				"nice level will not be adjusted",
				impl->nice_level, MIN_NICE_LEVEL, MAX_NICE_LEVEL);
	}

#ifdef HAVE_DBUS
	spa_list_init(&impl->threads_list);
	pthread_mutex_init(&impl->lock, NULL);
	pthread_cond_init(&impl->cond, NULL);

	if ((res = check_rtkit(impl, context, &can_use_rtkit)) < 0)
		goto error;

#endif
	/* If the user has permissions to use regular realtime scheduling, as well as
	 * the nice level we want, then we'll use that instead of RTKit */
	if (!check_realtime_privileges(impl)) {
		if (!can_use_rtkit) {
			res = -ENOTSUP;
			pw_log_warn("regular realtime scheduling not available"
					" (Portal/RTKit fallback disabled)");
			goto error;
		}
		use_rtkit = true;
	}

	if (IS_VALID_NICE_LEVEL(impl->nice_level)) {
		if (set_nice(impl, impl->nice_level, !can_use_rtkit) < 0)
			use_rtkit = can_use_rtkit;
	}
	if (!use_rtkit)
		set_rlimit(&impl->rl);

	if (impl->uclamp_max > 1024) {
		pw_log_warn("uclamp.max out of bounds. Got %d, clamping to 1024.", impl->uclamp_max);
		impl->uclamp_max = 1024;
	}

	if (impl->uclamp_min || impl->uclamp_max < 1024)
		set_uclamp(impl->uclamp_min, impl->uclamp_max, impl->main_pid);

#ifdef HAVE_DBUS
	impl->use_rtkit = use_rtkit;
	if (impl->use_rtkit) {
		struct spa_dict_item items[] = {
			{ "thread-loop.start-signal", "true" }
		};
		if ((res = rtkit_get_bus(impl)) < 0)
			goto error;

		impl->thread_loop = pw_thread_loop_new("module-rt",
			&SPA_DICT_INIT_ARRAY(items));
		if (impl->thread_loop == NULL) {
			res = -errno;
			goto error;
		}
		pw_thread_loop_lock(impl->thread_loop);
		pw_thread_loop_start(impl->thread_loop);
		pw_thread_loop_wait(impl->thread_loop);
		pw_thread_loop_unlock(impl->thread_loop);

		pw_loop_invoke(pw_thread_loop_get_loop(impl->thread_loop),
				do_rtkit_setup, 0, NULL, 0, false, impl);

		pw_log_debug("initialized using RTKit");
	} else {
		pw_log_debug("initialized using regular realtime scheduling");
	}
#else
	pw_log_debug("initialized using regular realtime scheduling");
#endif

	impl->thread_utils.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_ThreadUtils,
			SPA_VERSION_THREAD_UTILS,
			&impl_thread_utils, impl);

	pw_context_set_object(context, SPA_TYPE_INTERFACE_ThreadUtils,
			&impl->thread_utils);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));
	pw_impl_module_update_properties(module, &props->dict);

	goto done;

error:
#ifdef HAVE_DBUS
	if (impl->thread_loop)
		pw_thread_loop_destroy(impl->thread_loop);
	if (impl->rtkit_bus)
		pw_rtkit_bus_free(impl->rtkit_bus);
#endif
	free(impl);
done:
	pw_properties_free(props);

	return res;
}
