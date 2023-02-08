/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Red Hat, Inc. */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <signal.h>
#include <unistd.h>

#include <pipewire/pipewire.h>

#include "pwtest.h"

/* The simplest test example (test passes)  */
PWTEST(successful_test)
{
	int x = 10, y = 20, z = 10;
	bool t = true, f = false;
	const char *a = "foo", *b = "bar", *c = "baz";

	pwtest_int_lt(x, y);
	pwtest_int_le(x, y);
	pwtest_int_gt(y, x);
	pwtest_int_ge(y, x);
	pwtest_int_eq(x, z);
	pwtest_int_ne(y, z);

	pwtest_bool_true(t);
	pwtest_bool_false(f);
	pwtest_bool_eq(t, !f);
	pwtest_bool_ne(t, f);

	pwtest_str_eq(a, a);
	pwtest_str_ne(a, b);
	pwtest_str_eq_n(b, c, 2);
	pwtest_str_ne_n(b, c, 3);

	return PWTEST_PASS;
}

/* Demo failure of an integer comparison (test will fail) */
PWTEST(failing_test_int)
{
	int x = 10, y = 20;
	pwtest_int_gt(x, y);
	return PWTEST_PASS;
}

/* Demo failure of a bool comparison (test will fail) */
PWTEST(failing_test_bool)
{
	bool oops = true;
	pwtest_bool_false(oops);
	return PWTEST_PASS;
}

/* Demo failure of a string comparison (test will fail) */
PWTEST(failing_test_string)
{
	const char *what = "yes";
	pwtest_str_eq(what, "no");
	return PWTEST_PASS;
}

/* Demo custom failure (test will fail) */
PWTEST(general_fail_test)
{
	/* pwtest_fail(); */
	pwtest_fail_with_msg("Some condition wasn't met");
	return PWTEST_PASS;
}

/* Demo failure (test will fail) */
PWTEST(failing_test_if_reached)
{
	pwtest_fail_if_reached();
	return PWTEST_SYSTEM_ERROR;
}

/* Demo a system error (test will fail) */
PWTEST(system_error_test)
{
	return PWTEST_SYSTEM_ERROR;
}

/* Demo signal handling of SIGSEGV (test will fail) */
PWTEST(catch_segfault_test)
{
	int *x = NULL;
	*x = 20;
	return PWTEST_PASS;
}

/* Demo signal handling of abort (test will fail) */
PWTEST(catch_abort_signal_test)
{
	abort();
	return PWTEST_PASS;
}

/* Demo a timeout (test will fail with default timeout of 30) */
PWTEST(timeout_test)
{
	/* run with --timeout=1 to make this less annoying */
	sleep(60);
	return PWTEST_PASS;
}

/* Demo a ranged test (test passes, skips the last 2)  */
PWTEST(ranged_test)
{
	struct pwtest_test *t = current_test;
	int iteration = pwtest_get_iteration(t);

	pwtest_int_lt(iteration, 10); /* see pwtest_add */

	printf("Ranged test iteration %d\n", iteration);
	if (iteration >= 8)
		return PWTEST_SKIP;

	return PWTEST_PASS;
}

/* Demo the properties passed to tests (test passes)  */
PWTEST(property_test)
{
	struct pwtest_test *t = current_test;
	struct pw_properties *p = pwtest_get_props(t);

	pwtest_ptr_notnull(p);
	pwtest_str_eq(pw_properties_get(p, "myprop"), "somevalue");
	pwtest_str_eq(pw_properties_get(p, "prop2"), "other");

	return PWTEST_PASS;
}

/* Demo the environment passed to tests (test passes)  */
PWTEST(env_test)
{
	pwtest_str_eq(getenv("myenv"), "envval");
	pwtest_str_eq(getenv("env2"), "val");

	/* Set by pwtest */
	pwtest_str_eq(getenv("PWTEST"), "1");

	return PWTEST_PASS;
}

/* Demo the environment passed to tests (test passes)  */
PWTEST(env_reset_test)
{
	/* If run after env_test even with --no-fork this test should
	 * succeed */
	pwtest_str_eq(getenv("myenv"), NULL);
	pwtest_str_eq(getenv("env2"), NULL);

	return PWTEST_PASS;
}

PWTEST(default_env_test)
{
	/* This one is set automatically */
	pwtest_str_eq(getenv("PWTEST"), "1");
	/* Default value */
	pwtest_str_eq(getenv("PIPEWIRE_REMOTE"), "test-has-no-daemon");

	return PWTEST_PASS;
}

PWTEST(daemon_test)
{
	struct pw_context *ctx;
        struct pw_core *core;
	struct pw_loop *loop;

	pwtest_str_eq_n(getenv("PIPEWIRE_REMOTE"), "pwtest-pw-", 10);

	pw_init(0, NULL);
	loop = pw_loop_new(NULL);
	ctx = pw_context_new(loop, NULL, 0);
	pwtest_ptr_notnull(ctx);
        core = pw_context_connect(ctx, NULL, 0);
	pwtest_ptr_notnull(core);

	pw_loop_iterate(loop, -1);
	pw_core_disconnect(core);
        pw_context_destroy(ctx);
	pw_loop_destroy(loop);

	return PWTEST_PASS;
}

/* If not started with a daemon, we can't connect to a daemon (test will fail)  */
PWTEST(daemon_test_without_daemon)
{
	struct pw_context *ctx;
        struct pw_core *core;
	struct pw_loop *loop;

	pw_init(0, NULL);
	loop = pw_loop_new(NULL);
	ctx = pw_context_new(loop, NULL, 0);
	pwtest_ptr_notnull(ctx);
        core = pw_context_connect(ctx, NULL, 0);

	pwtest_ptr_notnull(core); /* Expect this to fail because we don't have a daemon */

	pw_loop_iterate(loop, -1);
	pw_core_disconnect(core);
        pw_context_destroy(ctx);
	pw_loop_destroy(loop);

	return PWTEST_PASS;
}

PWTEST_SUITE(example_tests)
{
	pwtest_add(successful_test, PWTEST_NOARG);
	pwtest_add(failing_test_int, PWTEST_NOARG);
	pwtest_add(failing_test_bool, PWTEST_NOARG);
	pwtest_add(failing_test_string, PWTEST_NOARG);
	pwtest_add(failing_test_if_reached, PWTEST_NOARG);
	pwtest_add(general_fail_test, PWTEST_NOARG);
	pwtest_add(system_error_test, PWTEST_NOARG);
	pwtest_add(catch_segfault_test, PWTEST_NOARG);
	pwtest_add(catch_abort_signal_test, PWTEST_ARG_SIGNAL, SIGABRT);
	pwtest_add(ranged_test, PWTEST_ARG_RANGE, 0, 10);
	pwtest_add(property_test,
		   PWTEST_ARG_PROP, "myprop", "somevalue",
		   PWTEST_ARG_PROP, "prop2", "other");
	pwtest_add(env_test,
		   PWTEST_ARG_ENV, "env2", "val",
		   PWTEST_ARG_ENV, "myenv", "envval");
	pwtest_add(env_reset_test, PWTEST_NOARG);
	pwtest_add(default_env_test, PWTEST_NOARG);
	pwtest_add(daemon_test, PWTEST_ARG_DAEMON);
	pwtest_add(daemon_test_without_daemon, PWTEST_NOARG);

	/* Run this one last so it doesn't matter if we forget --timeout */
	pwtest_add(timeout_test, PWTEST_NOARG);

	return PWTEST_PASS;
}
