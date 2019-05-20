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

#include <pipewire/pipewire.h>
#include <pipewire/main-loop.h>
#include <pipewire/core.h>

#define TEST_FUNC(a,b,func)	\
do {				\
	a.func = b.func;	\
	spa_assert(SPA_PTRDIFF(&a.func, &a) == SPA_PTRDIFF(&b.func, &b)); \
} while(0)

static void test_abi(void)
{
	struct pw_core_events ev;
	struct {
		uint32_t version;
		void (*destroy) (void *data);
		void (*free) (void *data);
		void (*info_changed) (void *data, const struct pw_core_info *info);
		void (*check_access) (void *data, struct pw_client *client);
		void (*global_added) (void *data, struct pw_global *global);
		void (*global_removed) (void *data, struct pw_global *global);
	} test = { PW_VERSION_CORE_EVENTS, NULL };

	TEST_FUNC(ev, test, destroy);
	TEST_FUNC(ev, test, free);
	TEST_FUNC(ev, test, info_changed);
	TEST_FUNC(ev, test, check_access);
	TEST_FUNC(ev, test, global_added);
	TEST_FUNC(ev, test, global_removed);

	spa_assert(PW_VERSION_CORE_EVENTS == 0);
	spa_assert(sizeof(ev) == sizeof(test));
}

static void core_destroy_error(void *data)
{
	spa_assert_not_reached();
}
static void core_free_error(void *data)
{
	spa_assert_not_reached();
}
static void core_info_changed_error(void *data, const struct pw_core_info *info)
{
	spa_assert_not_reached();
}
static void core_check_access_error(void *data, struct pw_client *client)
{
	spa_assert_not_reached();
}
static void core_global_added_error(void *data, struct pw_global *global)
{
	spa_assert_not_reached();
}
static void core_global_removed_error(void *data, struct pw_global *global)
{
	spa_assert_not_reached();
}

static const struct pw_core_events core_events_error =
{
	PW_VERSION_CORE_EVENTS,
	.destroy = core_destroy_error,
	.free = core_free_error,
	.info_changed = core_info_changed_error,
	.check_access = core_check_access_error,
	.global_added = core_global_added_error,
	.global_removed = core_global_removed_error,
};

static int destroy_count = 0;
static void core_destroy_count(void *data)
{
	destroy_count++;
}
static int free_count = 0;
static void core_free_count(void *data)
{
	free_count++;
}
static int global_removed_count = 0;
static void core_global_removed_count(void *data, struct pw_global *global)
{
	global_removed_count++;
}
static int core_foreach_count = 0;
static int core_foreach(void *data, struct pw_global *global)
{
	core_foreach_count++;
	return 0;
}
static int core_foreach_error(void *data, struct pw_global *global)
{
	core_foreach_count++;
	return -1;
}
static void test_create(void)
{
	struct pw_main_loop *loop;
	struct pw_core *core;
	struct spa_hook listener = { NULL, };
	struct pw_core_events core_events = core_events_error;
	struct pw_global *global;
	int res;

	loop = pw_main_loop_new(NULL);
	spa_assert(loop != NULL);

	core = pw_core_new(pw_main_loop_get_loop(loop), NULL, 12);
	spa_assert(core != NULL);
	pw_core_add_listener(core, &listener, &core_events, core);

	/* check main loop */
	spa_assert(pw_core_get_main_loop(core) == pw_main_loop_get_loop(loop));
	/* check user data */
	spa_assert(pw_core_get_user_data(core) != NULL);

	/* check info */
	spa_assert(pw_core_get_info(core) != NULL);

	/* check global */
	global = pw_core_get_global(core);
	spa_assert(global != NULL);
	spa_assert(pw_core_find_global(core, 0) == global);
	spa_assert(pw_global_get_core(global) == core);
	spa_assert(pw_global_get_owner(global) == NULL);
	spa_assert(pw_global_get_parent(global) == global);
	spa_assert(pw_global_get_type(global) == PW_TYPE_INTERFACE_Core);
	spa_assert(pw_global_get_version(global) == PW_VERSION_CORE_PROXY);
	spa_assert(pw_global_get_id(global) == 0);
	spa_assert(pw_global_get_object(global) == (void*)core);

	/* iterate globals */
	spa_assert(core_foreach_count == 0);
	res = pw_core_for_each_global(core, core_foreach, core);
	spa_assert(res == 0);
	spa_assert(core_foreach_count == 1);
	res = pw_core_for_each_global(core, core_foreach_error, core);
	spa_assert(res == -1);
	spa_assert(core_foreach_count == 2);

	/* check destroy */
	core_events.destroy = core_destroy_count;
	core_events.free = core_free_count;
	core_events.global_removed = core_global_removed_count;

	spa_assert(destroy_count == 0);
	spa_assert(free_count == 0);
	spa_assert(global_removed_count == 0);
	pw_core_destroy(core);
	spa_assert(destroy_count == 1);
	spa_assert(free_count == 1);
	spa_assert(global_removed_count == 1);
	pw_main_loop_destroy(loop);
}

static int info_changed_count = 0;
static void core_info_changed_count(void *data, const struct pw_core_info *info)
{
	spa_assert(spa_dict_lookup(info->props, "foo") == NULL);
	spa_assert(!strcmp(spa_dict_lookup(info->props, "biz"), "buzz"));
	spa_assert(!strcmp(spa_dict_lookup(info->props, "buzz"), "frizz"));
	info_changed_count++;
}

static void test_properties(void)
{
	struct pw_main_loop *loop;
	struct pw_core *core;
	const struct pw_properties *props;
	struct spa_hook listener = { NULL, };
	struct pw_core_events core_events = core_events_error;
	struct spa_dict_item items[3];

	loop = pw_main_loop_new(NULL);
	core = pw_core_new(pw_main_loop_get_loop(loop),
			pw_properties_new("foo", "bar",
					  "biz", "fuzz",
					  NULL),
			0);
	spa_assert(core != NULL);
	spa_assert(pw_core_get_user_data(core) == NULL);
	pw_core_add_listener(core, &listener, &core_events, core);

	core_events.info_changed = core_info_changed_count;
	spa_assert(info_changed_count == 0);

	props = pw_core_get_properties(core);
	spa_assert(props != NULL);
	spa_assert(!strcmp(pw_properties_get(props, "foo"), "bar"));
	spa_assert(!strcmp(pw_properties_get(props, "biz"), "fuzz"));
	spa_assert(pw_properties_get(props, "buzz") == NULL);

	/* remove foo */
	items[0] = SPA_DICT_ITEM_INIT("foo", NULL);
	/* change biz */
	items[1] = SPA_DICT_ITEM_INIT("biz", "buzz");
	/* add buzz */
	items[2] = SPA_DICT_ITEM_INIT("buzz", "frizz");
	pw_core_update_properties(core, &SPA_DICT_INIT(items, 3));

	spa_assert(info_changed_count == 1);

	spa_assert(props == pw_core_get_properties(core));
	spa_assert(pw_properties_get(props, "foo") == NULL);
	spa_assert(!strcmp(pw_properties_get(props, "biz"), "buzz"));
	spa_assert(!strcmp(pw_properties_get(props, "buzz"), "frizz"));

	spa_hook_remove(&listener);
	pw_core_destroy(core);
	pw_main_loop_destroy(loop);
}

static void test_support(void)
{
	struct pw_main_loop *loop;
	struct pw_core *core;
	const struct spa_support *support;
	uint32_t n_support;
	uint32_t types[] = {
		SPA_TYPE_INTERFACE_DataLoop,
		SPA_TYPE_INTERFACE_MainLoop,
		SPA_TYPE_INTERFACE_LoopUtils,
		SPA_TYPE_INTERFACE_Log,
		SPA_TYPE_INTERFACE_DBus,
		SPA_TYPE_INTERFACE_CPU
	};
	size_t i;

	loop = pw_main_loop_new(NULL);
	core = pw_core_new(pw_main_loop_get_loop(loop), NULL, 0);

	support = pw_core_get_support(core, &n_support);
	spa_assert(support != NULL);
	spa_assert(n_support > 0);

	for (i = 0; i < SPA_N_ELEMENTS(types); i++) {
		spa_assert(spa_support_find(support, n_support, types[i]) != NULL);
	}

	pw_core_destroy(core);
	pw_main_loop_destroy(loop);
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);

	test_abi();
	test_create();
	test_properties();
	test_support();

	return 0;
}
