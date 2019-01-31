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
#include <pipewire/remote.h>

#define TEST_FUNC(a,b,func)	\
do {				\
	a.func = b.func;	\
	spa_assert(SPA_PTRDIFF(&a.func, &a) == SPA_PTRDIFF(&b.func, &b)); \
} while(0)

static void test_abi(void)
{
	struct pw_remote_events ev;
	struct {
		uint32_t version;
		void (*destroy) (void *data);
		void (*state_changed) (void *data, enum pw_remote_state old,
				enum pw_remote_state state, const char *error);
		void (*exported) (void *data, uint32_t proxy_id, uint32_t remote_id);
	} test = { PW_VERSION_REMOTE_EVENTS, NULL };

	TEST_FUNC(ev, test, destroy);
	TEST_FUNC(ev, test, state_changed);
	TEST_FUNC(ev, test, exported);

	spa_assert(PW_VERSION_REMOTE_EVENTS == 0);
	spa_assert(sizeof(ev) == sizeof(test));

	spa_assert(PW_REMOTE_STATE_ERROR == -1);
	spa_assert(PW_REMOTE_STATE_UNCONNECTED == 0);
	spa_assert(PW_REMOTE_STATE_CONNECTING == 1);
	spa_assert(PW_REMOTE_STATE_CONNECTED == 2);

	spa_assert(pw_remote_state_as_string(PW_REMOTE_STATE_ERROR) != NULL);
	spa_assert(pw_remote_state_as_string(PW_REMOTE_STATE_UNCONNECTED) != NULL);
	spa_assert(pw_remote_state_as_string(PW_REMOTE_STATE_CONNECTING) != NULL);
	spa_assert(pw_remote_state_as_string(PW_REMOTE_STATE_CONNECTED) != NULL);
}

static void remote_destroy_error(void *data)
{
	spa_assert_not_reached();
}
static void remote_state_changed_error(void *data, enum pw_remote_state old,
		enum pw_remote_state state, const char *error)
{
	spa_assert_not_reached();
}
static void remote_exported_error(void *data, uint32_t proxy_id, uint32_t global_id)
{
	spa_assert_not_reached();
}

static const struct pw_remote_events remote_events_error =
{
	PW_VERSION_REMOTE_EVENTS,
        .destroy = remote_destroy_error,
        .state_changed = remote_state_changed_error,
        .exported = remote_exported_error,
};

static int destroy_count = 0;
static void remote_destroy_count(void *data)
{
	destroy_count++;
}
static void test_create(void)
{
	struct pw_main_loop *loop;
	struct pw_core *core;
	struct pw_remote *remote;
	struct pw_remote_events remote_events = remote_events_error;
	struct spa_hook listener = { 0, };
	const char *error = NULL;

	loop = pw_main_loop_new(NULL);
	core = pw_core_new(pw_main_loop_get_loop(loop), NULL, 12);

	remote = pw_remote_new(core, NULL, 12);
	spa_assert(remote != NULL);
	pw_remote_add_listener(remote, &listener, &remote_events, remote);

	/* check core */
	spa_assert(pw_remote_get_core(remote) == core);
	/* check user data */
	spa_assert(pw_remote_get_user_data(remote) != NULL);
	/* check state */
	spa_assert(pw_remote_get_state(remote, &error) == PW_REMOTE_STATE_UNCONNECTED);
	spa_assert(error == NULL);

	/* check core proxy, only available when connected */
	spa_assert(pw_remote_get_core_proxy(remote) == NULL);

	/* check some non-existing proxies */
	spa_assert(pw_remote_find_proxy(remote, 0) == NULL);
	spa_assert(pw_remote_find_proxy(remote, 5) == NULL);

	/* check destroy */
	destroy_count = 0;
	remote_events.destroy = remote_destroy_count;
	pw_remote_destroy(remote);
	spa_assert(destroy_count == 1);

	pw_core_destroy(core);
	pw_main_loop_destroy(loop);
}

static void test_properties(void)
{
	struct pw_main_loop *loop;
	struct pw_core *core;
	const struct pw_properties *props;
	struct pw_remote *remote;
	struct pw_remote_events remote_events = remote_events_error;
	struct spa_hook listener = { NULL, };
	struct spa_dict_item items[3];

	loop = pw_main_loop_new(NULL);
	core = pw_core_new(pw_main_loop_get_loop(loop), NULL, 0);
	remote = pw_remote_new(core,
			pw_properties_new("foo", "bar",
					  "biz", "fuzz",
					  NULL),
			0);
	spa_assert(remote != NULL);
	spa_assert(pw_remote_get_user_data(remote) == NULL);
	pw_remote_add_listener(remote, &listener, &remote_events, remote);

	props = pw_remote_get_properties(remote);
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
	/* update properties does not emit the info_changed signal
	 * because that is only emited when the remote core properties
	 * changed */
	pw_remote_update_properties(remote, &SPA_DICT_INIT(items, 3));

	spa_assert(props == pw_remote_get_properties(remote));
	spa_assert(pw_properties_get(props, "foo") == NULL);
	spa_assert(!strcmp(pw_properties_get(props, "biz"), "buzz"));
	spa_assert(!strcmp(pw_properties_get(props, "buzz"), "frizz"));

	/* check destroy */
	destroy_count = 0;
	remote_events.destroy = remote_destroy_count;
	pw_core_destroy(core);
	spa_assert(destroy_count == 1);

	pw_main_loop_destroy(loop);
}

int main(int argc, char *argv[])
{
	pw_init(&argc, &argv);

	test_abi();
	test_create();
	test_properties();

	return 0;
}
