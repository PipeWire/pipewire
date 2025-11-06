/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>
#ifdef __FreeBSD__
#include <pthread_np.h>
#endif

#include <spa/utils/dict.h>
#include <spa/utils/defs.h>
#include <spa/utils/list.h>
#include <spa/utils/json.h>

#define PW_API_THREAD_IMPL SPA_EXPORT
#include <pipewire/log.h>
#include <pipewire/private.h>
#include <pipewire/thread.h>

#define CHECK(expression,label)						\
do {									\
	if ((errno = (expression)) != 0) {				\
		res = -errno;						\
		pw_log_error(#expression ": %s", strerror(errno));	\
		goto label;						\
	}								\
} while(false);

static int parse_affinity(const char *affinity, cpu_set_t *set)
{
	struct spa_json it[1];
	int v;

	CPU_ZERO(set);
	if (spa_json_begin_array_relax(&it[0], affinity, strlen(affinity)) <= 0)
		return 0;

	while (spa_json_get_int(&it[0], &v) > 0) {
		if (v >= 0 && v < CPU_SETSIZE)
			CPU_SET(v, set);
        }
	return 0;
}

SPA_EXPORT
void *pw_thread_fill_attr(const struct spa_dict *props, void *_attr)
{
	pthread_attr_t *attr = _attr;
	const char *str;
	int res;

	if (props == NULL)
		return NULL;

	pthread_attr_init(attr);
	if ((str = spa_dict_lookup(props, SPA_KEY_THREAD_STACK_SIZE)) != NULL)
		CHECK(pthread_attr_setstacksize(attr, atoi(str)), error);
	return attr;
error:
	errno = -res;
	return NULL;
}

#if defined(__FreeBSD__) || defined(__MidnightBSD__)
#include <sys/param.h>
#if __FreeBSD_version < 1202000 || defined(__MidnightBSD__)
int pthread_setname_np(pthread_t thread, const char *name)
{
	pthread_set_name_np(thread, name);
	return 0;
}
#endif
#endif
#if defined(__GNU__)
int pthread_setname_np(pthread_t thread, const char *name) { return 0; }
#endif

static int thread_setaffinity(pthread_t thread, const char *affinity)
{
	cpu_set_t set;
	parse_affinity(affinity, &set);
	return -pthread_setaffinity_np(thread, sizeof(set), &set);
}

static struct spa_thread *impl_create(void *object,
			const struct spa_dict *props,
			void *(*start)(void*), void *arg)
{
	pthread_t pt;
	pthread_attr_t *attr = NULL, attributes;
	const char *str;
	int err, old_policy, new_policy;
	int (*create_func)(pthread_t *, const pthread_attr_t *attr, void *(*start)(void*), void *) = NULL;
	struct sched_param sp;
	bool reset_on_fork = true;

	attr = pw_thread_fill_attr(props, &attributes);

	if (props == NULL ||
	   (str = spa_dict_lookup(props, SPA_KEY_THREAD_CREATOR)) == NULL ||
	   sscanf(str, "pointer:%p", &create_func) != 1)
		create_func = pthread_create;

	err = create_func(&pt, attr, start, arg);

	if (attr)
		pthread_attr_destroy(attr);

	if (err != 0) {
		errno = err;
		return NULL;
	}
	if (props) {
		if ((str = spa_dict_lookup(props, SPA_KEY_THREAD_NAME)) != NULL &&
		    (err = pthread_setname_np(pt, str)) != 0)
			pw_log_warn("pthread_setname error: %s", strerror(err));
		if ((str = spa_dict_lookup(props, SPA_KEY_THREAD_AFFINITY)) != NULL &&
		    (err = thread_setaffinity(pt, str)) != 0)
			pw_log_warn("pthread_setaffinity error: %s", strerror(-err));
		if ((str = spa_dict_lookup(props, SPA_KEY_THREAD_RESET_ON_FORK)) != NULL)
			reset_on_fork = spa_atob(str);
	}

	pthread_getschedparam(pt, &old_policy, &sp);
	new_policy = old_policy;
	SPA_FLAG_UPDATE(new_policy, SCHED_RESET_ON_FORK, reset_on_fork);
	if (old_policy != new_policy)
		pthread_setschedparam(pt, new_policy, &sp);

	return (struct spa_thread*)pt;
}

static int impl_join(void *object, struct spa_thread *thread, void **retval)
{
	pthread_t pt = (pthread_t)thread;
	return -pthread_join(pt, retval);
}

static int impl_get_rt_range(void *object, const struct spa_dict *props,
		int *min, int *max)
{
	if (min) {
		*min = sched_get_priority_min(SCHED_OTHER);
		if (*min < 0)
			return -errno;
	}

	if (max) {
		*max = sched_get_priority_max(SCHED_OTHER);
		if (*max < 0)
			return -errno;
	}

	return 0;
}
static int impl_acquire_rt(void *object, struct spa_thread *thread, int priority)
{
	pw_log_info("acquire_rt thread:%p prio:%d not implemented", thread, priority);
	return -ENOTSUP;
}

static int impl_drop_rt(void *object, struct spa_thread *thread)
{
	pw_log_info("drop_rt thread:%p not implemented", thread);
	return -ENOTSUP;
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
		.get_rt_range = impl_get_rt_range,
		.acquire_rt = impl_acquire_rt,
		.drop_rt = impl_drop_rt,
	}
};

static struct spa_thread_utils *global_impl = &default_impl.utils;

SPA_EXPORT
void pw_thread_utils_set(struct spa_thread_utils *impl)
{
	pw_log_warn("pw_thread_utils_set is deprecated and does nothing anymore");
}

SPA_EXPORT
struct spa_thread_utils *pw_thread_utils_get(void)
{
	return global_impl;
}
