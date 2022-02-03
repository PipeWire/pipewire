/* PipeWire
 *
 * Copyright © 2019 Wim Taymans
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

#include "pwtest.h"

#include <spa/utils/string.h>
#include <spa/support/dbus.h>
#include <spa/support/cpu.h>

#include <pipewire/pipewire.h>
#include <pipewire/global.h>

#define TEST_FUNC(a,b,func)	\
do {				\
	a.func = b.func;	\
	pwtest_ptr_eq(SPA_PTRDIFF(&a.func, &a), SPA_PTRDIFF(&b.func, &b)); \
} while(0)

PWTEST(context_abi)
{
	struct pw_context_events ev;
	struct {
		uint32_t version;
		void (*destroy) (void *data);
		void (*free) (void *data);
		void (*check_access) (void *data, struct pw_impl_client *client);
		void (*global_added) (void *data, struct pw_global *global);
		void (*global_removed) (void *data, struct pw_global *global);
	} test = { PW_VERSION_CONTEXT_EVENTS, NULL };

	pw_init(0, NULL);

	TEST_FUNC(ev, test, destroy);
	TEST_FUNC(ev, test, free);
	TEST_FUNC(ev, test, check_access);
	TEST_FUNC(ev, test, global_added);
	TEST_FUNC(ev, test, global_removed);

	pwtest_int_eq(PW_VERSION_CONTEXT_EVENTS, 0);
	pwtest_int_eq(sizeof(ev), sizeof(test));

	pw_deinit();

	return PWTEST_PASS;
}

static void context_destroy_error(void *data)
{
	pwtest_fail_if_reached();
}
static void context_free_error(void *data)
{
	pwtest_fail_if_reached();
}
static void context_check_access_error(void *data, struct pw_impl_client *client)
{
	pwtest_fail_if_reached();
}
static void context_global_added_error(void *data, struct pw_global *global)
{
	pwtest_fail_if_reached();
}
static void context_global_removed_error(void *data, struct pw_global *global)
{
	pwtest_fail_if_reached();
}

static const struct pw_context_events context_events_error =
{
	PW_VERSION_CONTEXT_EVENTS,
	.destroy = context_destroy_error,
	.free = context_free_error,
	.check_access = context_check_access_error,
	.global_added = context_global_added_error,
	.global_removed = context_global_removed_error,
};

static int destroy_count = 0;
static void context_destroy_count(void *data)
{
	destroy_count++;
}
static int free_count = 0;
static void context_free_count(void *data)
{
	free_count++;
}
static int global_removed_count = 0;
static void context_global_removed_count(void *data, struct pw_global *global)
{
	global_removed_count++;
}
static int context_foreach_count = 0;
static int context_foreach(void *data, struct pw_global *global)
{
	context_foreach_count++;
	return 0;
}
static int context_foreach_error(void *data, struct pw_global *global)
{
	context_foreach_count++;
	return -1;
}
PWTEST(context_create)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	struct spa_hook listener = { { NULL }, };
	struct pw_context_events context_events = context_events_error;
	int res;

	pw_init(0, NULL);

	loop = pw_main_loop_new(NULL);
	pwtest_ptr_notnull(loop);

	context = pw_context_new(pw_main_loop_get_loop(loop),
			pw_properties_new(
				PW_KEY_CONFIG_NAME, "null",
				NULL), 12);
	pwtest_ptr_notnull(context);
	pw_context_add_listener(context, &listener, &context_events, context);

	/* check main loop */
	pwtest_ptr_eq(pw_context_get_main_loop(context), pw_main_loop_get_loop(loop));
	/* check user data */
	pwtest_ptr_notnull(pw_context_get_user_data(context));

	/* iterate globals */
	pwtest_int_eq(context_foreach_count, 0);
	res = pw_context_for_each_global(context, context_foreach, context);
	pwtest_int_eq(res, 0);
	pwtest_int_eq(context_foreach_count, 2);
	res = pw_context_for_each_global(context, context_foreach_error, context);
	pwtest_int_eq(res, -1);
	pwtest_int_eq(context_foreach_count, 3);

	/* check destroy */
	context_events.destroy = context_destroy_count;
	context_events.free = context_free_count;
	context_events.global_removed = context_global_removed_count;

	pwtest_int_eq(destroy_count, 0);
	pwtest_int_eq(free_count, 0);
	pwtest_int_eq(global_removed_count, 0);
	pw_context_destroy(context);
	pwtest_int_eq(destroy_count, 1);
	pwtest_int_eq(free_count, 1);
	pwtest_int_eq(global_removed_count, 2);
	pw_main_loop_destroy(loop);

	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(context_properties)
{
	struct pw_main_loop *loop;
	struct pw_context *context;
	const struct pw_properties *props;
	struct spa_hook listener = { { NULL }, };
	struct pw_context_events context_events = context_events_error;
	struct spa_dict_item items[3];

	pw_init(0, NULL);

	loop = pw_main_loop_new(NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop),
			pw_properties_new("foo", "bar",
					  "biz", "fuzz",
					  NULL),
			0);
	pwtest_ptr_notnull(context);
	pwtest_ptr_null(pw_context_get_user_data(context));
	pw_context_add_listener(context, &listener, &context_events, context);

	props = pw_context_get_properties(context);
	pwtest_ptr_notnull(props);
	pwtest_str_eq(pw_properties_get(props, "foo"), "bar");
	pwtest_str_eq(pw_properties_get(props, "biz"), "fuzz");
	pwtest_str_eq(pw_properties_get(props, "buzz"), NULL);

	/* remove foo */
	items[0] = SPA_DICT_ITEM_INIT("foo", NULL);
	/* change biz */
	items[1] = SPA_DICT_ITEM_INIT("biz", "buzz");
	/* add buzz */
	items[2] = SPA_DICT_ITEM_INIT("buzz", "frizz");
	pw_context_update_properties(context, &SPA_DICT_INIT(items, 3));

	pwtest_ptr_eq(props, pw_context_get_properties(context));
	pwtest_str_eq(pw_properties_get(props, "foo"), NULL);
	pwtest_str_eq(pw_properties_get(props, "biz"), "buzz");
	pwtest_str_eq(pw_properties_get(props, "buzz"), "frizz");

	spa_hook_remove(&listener);
	pw_context_destroy(context);
	pw_main_loop_destroy(loop);

	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(context_support)
{
	static const char * const types[] = {
		SPA_TYPE_INTERFACE_DataSystem,
		SPA_TYPE_INTERFACE_DataLoop,
		SPA_TYPE_INTERFACE_System,
		SPA_TYPE_INTERFACE_Loop,
		SPA_TYPE_INTERFACE_LoopUtils,
		SPA_TYPE_INTERFACE_Log,
#ifdef HAVE_DBUS
		SPA_TYPE_INTERFACE_DBus,
#endif
		SPA_TYPE_INTERFACE_CPU
	};

	struct pw_main_loop *loop;
	struct pw_context *context;
	const struct spa_support *support;
	uint32_t n_support;
	size_t i;

	pw_init(0, NULL);

	loop = pw_main_loop_new(NULL);
	context = pw_context_new(pw_main_loop_get_loop(loop), NULL, 0);

	support = pw_context_get_support(context, &n_support);
	pwtest_ptr_notnull(support);
	pwtest_int_gt(n_support, 0U);

	for (i = 0; i < SPA_N_ELEMENTS(types); i++) {
		pwtest_ptr_notnull(spa_support_find(support, n_support, types[i]));
	}

	pw_context_destroy(context);
	pw_main_loop_destroy(loop);

	pw_deinit();

	return PWTEST_PASS;
}

PWTEST_SUITE(context)
{
	pwtest_add(context_abi, PWTEST_NOARG);
	pwtest_add(context_create, PWTEST_NOARG);
	pwtest_add(context_properties, PWTEST_NOARG);
	pwtest_add(context_support, PWTEST_NOARG);

	return PWTEST_PASS;
}
