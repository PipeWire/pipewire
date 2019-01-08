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
		void (*info_changed) (void *data, const struct pw_core_info *info);
		void (*sync_reply) (void *data, uint32_t seq);
		void (*state_changed) (void *data, enum pw_remote_state old,
				enum pw_remote_state state, const char *error);
		void (*error) (void *data, uint32_t id, int res, const char *error);
		void (*exported) (void *data, uint32_t id);
	} test = { PW_VERSION_REMOTE_EVENTS, NULL };

	TEST_FUNC(ev, test, destroy);
	TEST_FUNC(ev, test, info_changed);
	TEST_FUNC(ev, test, sync_reply);
	TEST_FUNC(ev, test, state_changed);
	TEST_FUNC(ev, test, error);
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

int main(int argc, char *argv[])
{
	test_abi();

	return 0;
}
