/* Simple Plugin API
 * Copyright © 2021 Red Hat, Inc.
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

#include <spa/support/log.h>

#include "pwtest.h"

#define OTHER_ARGS const char *file, int line, const char *func, const char *fmt

struct data {
	bool invoked;
	const char *func;
	const char *msg;
	const struct spa_log_topic *topic;
};


static void impl_log_log(void *object, enum spa_log_level level, OTHER_ARGS, ...) {
	struct data *data = object;
	*data = (struct data) {
		.func = __func__,
		.invoked = true,
		.msg = fmt,
		.topic = NULL,
	};
};

static void impl_log_logv(void *object, enum spa_log_level level, OTHER_ARGS, va_list args) {
	struct data *data = object;
	*data = (struct data) {
		.func = __func__,
		.invoked = true,
		.msg = fmt,
		.topic = NULL,
	};
};

static void impl_log_logt(void *object, enum spa_log_level level, const struct spa_log_topic *topic, OTHER_ARGS, ...) {
	struct data *data = object;
	*data = (struct data) {
		.func = __func__,
		.invoked = true,
		.msg = fmt,
		.topic = topic,
	};
};

static void impl_log_logtv(void *object, enum spa_log_level level, const struct spa_log_topic *topic, OTHER_ARGS, va_list args) {
	struct data *data = object;
	*data = (struct data) {
		.func = __func__,
		.invoked = true,
		.msg = fmt,
		.topic = topic,
	};
};

PWTEST(utils_log_logt)
{
	struct spa_log_methods impl_log = {
		SPA_VERSION_LOG_METHODS,
		.log = impl_log_log,
		.logv = impl_log_logv,
		.logt = impl_log_logt,
		.logtv = impl_log_logtv,
	};
	struct spa_log log;
	struct data data;
	struct spa_log_topic topic = {
		.version = 0,
		.topic = "log topic",
		.level = SPA_LOG_LEVEL_DEBUG,
	};

	log.level = SPA_LOG_LEVEL_DEBUG;
	log.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Log, 0, &impl_log, &data);

	impl_log.version = 0;

	/* impl_log is v0 so we expect the non-topic function to be called */
	spa_log_debug(&log, "call v0");
	pwtest_bool_true(data.invoked);
	pwtest_str_eq(data.func, "impl_log_log");
	pwtest_str_eq(data.msg, "call v0");
	pwtest_ptr_null(data.topic);
	data.invoked = false;

	/* impl_log is v0 so we expect the topic to be ignored */
	spa_logt_debug(&log, &topic, "call v0 logt");
	pwtest_bool_true(data.invoked);
	pwtest_str_eq(data.func, "impl_log_log");
	pwtest_str_eq(data.msg, "call v0 logt");
	pwtest_ptr_null(data.topic);
	data.invoked = false;

	impl_log.version = SPA_VERSION_LOG_METHODS;

	/* impl_log is v1 so we expect logt to be called */
	spa_log_debug(&log, "call v1");
	pwtest_bool_true(data.invoked);
	pwtest_str_eq(data.func, "impl_log_logt");
	pwtest_str_eq(data.msg, "call v1");
	pwtest_ptr_null(data.topic);
	data.invoked = false;

	/* impl_log is v1 so we expect the topic to be passed through */
	spa_logt_debug(&log, &topic, "call v1 logt");
	pwtest_bool_true(data.invoked);
	pwtest_str_eq(data.func, "impl_log_logt");
	pwtest_str_eq(data.msg, "call v1 logt");
	pwtest_ptr_eq(data.topic, &topic);
	data.invoked = false;

	/* simulated:
	 * impl_log is v1 but we have an old caller that uses v0, this goes
	 * through to the non-topic log function */
	spa_interface_call(&log.iface, struct spa_log_methods, log, 0,
			   SPA_LOG_LEVEL_DEBUG, "file", 123, "function", "call from v0");
	pwtest_bool_true(data.invoked);
	pwtest_str_eq(data.func, "impl_log_log");
	pwtest_str_eq(data.msg, "call from v0");
	pwtest_ptr_null(data.topic);
	data.invoked = false;

	return PWTEST_PASS;
}

PWTEST(utils_log_logt_levels)
{
	struct spa_log_methods impl_log = {
		SPA_VERSION_LOG_METHODS,
		.log = impl_log_log,
		.logv = impl_log_logv,
		.logt = impl_log_logt,
		.logtv = impl_log_logtv,
	};
	struct spa_log log;
	struct data data;
	struct spa_log_topic topic = {
		.version = 0,
		.topic = "log topic",
		.level = SPA_LOG_LEVEL_INFO,
		.has_custom_level = true,
	};

	log.level = SPA_LOG_LEVEL_DEBUG;
	log.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Log, 0, &impl_log, &data);

	/* Topic is NULL for spa_log_*, so expect this to be invoked */
	spa_log_debug(&log, "spa_log_debug");
	pwtest_bool_true(data.invoked);
	pwtest_str_eq(data.msg, "spa_log_debug");
	pwtest_ptr_null(data.topic);
	data.invoked = false;

	spa_log_info(&log, "spa_log_info");
	pwtest_bool_true(data.invoked);
	pwtest_str_eq(data.msg, "spa_log_info");
	pwtest_ptr_null(data.topic);
	data.invoked = false;

	spa_log_warn(&log, "spa_log_warn");
	pwtest_bool_true(data.invoked);
	pwtest_str_eq(data.msg, "spa_log_warn");
	pwtest_ptr_null(data.topic);
	data.invoked = false;

	spa_logt_debug(&log, &topic, "spa_logt_debug");
	pwtest_bool_false(data.invoked);
	data.invoked = false;

	spa_logt_info(&log, &topic, "spa_logt_info");
	pwtest_bool_true(data.invoked);
	pwtest_str_eq(data.msg, "spa_logt_info");
	pwtest_ptr_eq(data.topic, &topic);
	data.invoked = false;

	spa_logt_warn(&log, &topic, "spa_logt_warn");
	pwtest_bool_true(data.invoked);
	pwtest_str_eq(data.msg, "spa_logt_warn");
	pwtest_ptr_eq(data.topic, &topic);
	data.invoked = false;

	return PWTEST_PASS;
}

PWTEST_SUITE(spa_log)
{
	pwtest_add(utils_log_logt, PWTEST_NOARG);
	pwtest_add(utils_log_logt_levels, PWTEST_NOARG);

	return PWTEST_PASS;
}
