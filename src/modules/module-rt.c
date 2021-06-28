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

#include <pipewire/impl.h>
#include <spa/utils/dict.h>

#include "config.h"

/** \page page_module_rt PipeWire Module: RT
 */

#define DEFAULT_NICE_LEVEL -11
#define DEFAULT_RT_PRIO 88
#define DEFAULT_RT_TIME_SOFT 200000
#define DEFAULT_RT_TIME_HARD 200000

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

	struct spa_loop *loop;
	struct spa_system *system;
	struct spa_source source;

	int rt_prio;
	rlim_t rt_time_soft;
	rlim_t rt_time_hard;

	struct spa_hook module_listener;
};

static int do_remove_source(struct spa_loop *loop, bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct spa_source *source = user_data;

	spa_loop_remove_source(loop, source);

	return 0;
}

static void module_destroy(void *data)
{
	struct impl *impl = data;

	spa_hook_remove(&impl->module_listener);

	if (impl->source.fd != -1) {
		spa_loop_invoke(impl->loop, do_remove_source, SPA_ID_INVALID, NULL, 0, true, &impl->source);
		spa_system_close(impl->system, impl->source.fd);
		impl->source.fd = -1;
	}
	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static void idle_func(struct spa_source *source)
{
	struct impl *impl = source->data;
	uint64_t count;
	int policy = SCHED_FIFO;
	int rtprio = impl->rt_prio;
	struct rlimit rl;
	struct sched_param sp;

	if (SPA_UNLIKELY(spa_system_eventfd_read(impl->system, impl->source.fd, &count) < 0))
		pw_log_warn("read failed: %m");

        if (rtprio < sched_get_priority_min(policy) ||
            rtprio > sched_get_priority_max(policy)) {
		pw_log_warn("invalid priority %d for policy %d", rtprio, policy);
		return;
	}

	rl.rlim_cur = impl->rt_time_soft;
	rl.rlim_max = impl->rt_time_hard;
	if (setrlimit(RLIMIT_RTTIME, &rl) < 0)
		pw_log_warn("could not set rlimit: %m");
	else
		pw_log_debug("rt.prio %d, rt.time.soft %"PRIi64", rt.time.hard %"PRIi64,
			rtprio, (int64_t)rl.rlim_cur, (int64_t)rl.rlim_max);

	spa_zero(sp);
	sp.sched_priority = rtprio;
        if (sched_setscheduler(0, policy | SCHED_RESET_ON_FORK, &sp) < 0) {
		pw_log_warn("could not make thread realtime: %m");
		return;
        }

	pw_log_info("processing thread has realtime priority %d", rtprio);
}

static void set_nice(struct impl *impl, int nice_level)
{
	long tid;
	int res;

	tid = syscall(SYS_gettid);
	if (tid < 0) {
		pw_log_warn("could not get main thread id: %m");
		tid = 0; /* means current thread in setpriority() on linux */
	}
	res = setpriority(PRIO_PROCESS, (id_t)tid, nice_level);
	if (res < 0)
		pw_log_warn("could not set nice-level to %d: %m", nice_level);
	else
		pw_log_info("main thread nice level set to %d", nice_level);
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

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	struct spa_loop *loop;
	struct spa_system *system;
	const struct spa_support *support;
	uint32_t n_support;
	struct pw_properties *props;
	int nice_level;
	int res;

	support = pw_context_get_support(context, &n_support);

	loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
        if (loop == NULL)
                return -ENOTSUP;

	system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataSystem);
        if (system == NULL)
                return -ENOTSUP;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

	pw_log_debug("module %p: new %s", impl, args);

	impl->context = context;
	impl->loop = loop;
	impl->system = system;
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

	impl->source.loop = loop;
	impl->source.func = idle_func;
	impl->source.data = impl;
	impl->source.fd = spa_system_eventfd_create(system, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	impl->source.mask = SPA_IO_IN;
	if (impl->source.fd == -1) {
		res = -errno;
		goto error;
	}

	spa_loop_add_source(impl->loop, &impl->source);
	if (SPA_UNLIKELY(spa_system_eventfd_write(system, impl->source.fd, 1) < 0))
		pw_log_warn("write failed: %m");

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
