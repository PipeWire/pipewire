/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "pwtest.h"

#include <pipewire/pipewire.h>
#include <pipewire/impl-client.h>

#define TEST_FUNC(a,b,func)	\
do {				\
	a.func = b.func;	\
	pwtest_ptr_eq(SPA_PTRDIFF(&a.func, &a), SPA_PTRDIFF(&b.func, &b)); \
} while(0)

PWTEST(client_abi)
{
	static const struct {
		uint32_t version;
		void (*destroy) (void *data);
		void (*free) (void *data);
		void (*initialized) (void *data);
		void (*info_changed) (void *data, const struct pw_client_info *info);
		void (*resource_added) (void *data, struct pw_resource *resource);
		void (*resource_removed) (void *data, struct pw_resource *resource);
		void (*busy_changed) (void *data, bool busy);
	} test = { PW_VERSION_IMPL_CLIENT_EVENTS, NULL };

	struct pw_impl_client_events ev;

	TEST_FUNC(ev, test, destroy);
	TEST_FUNC(ev, test, free);
	TEST_FUNC(ev, test, initialized);
	TEST_FUNC(ev, test, info_changed);
	TEST_FUNC(ev, test, resource_added);
	TEST_FUNC(ev, test, resource_removed);
	TEST_FUNC(ev, test, busy_changed);

	pwtest_int_eq(PW_VERSION_IMPL_CLIENT_EVENTS, 0);
	pwtest_int_eq(sizeof(ev), sizeof(test));

	return PWTEST_PASS;
}

PWTEST_SUITE(client)
{
	pwtest_add(client_abi, PWTEST_NOARG);

	return PWTEST_PASS;
}
