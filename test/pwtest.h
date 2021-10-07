/* PipeWire
 *
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

#include "config.h"

#ifndef PWTEST_H
#define PWTEST_H

#ifdef __cplusplus
extern "C" {
#endif

#include <limits.h>
#include <stddef.h>
#include <stdbool.h>
#include <math.h>

#include <spa/utils/string.h>
#include <spa/utils/dict.h>
#include "spa/support/plugin.h"

/**
 * \defgroup pwtest Test Suite
 * \brief `pwtest` is a test runner framework for PipeWire.
 *
 * It's modelled after other
 * test suites like [check](https://libcheck.github.io/check/)
 * and draws a lot of inspiration from the [libinput test
 * suite](https://wayland.freedesktop.org/libinput/doc/latest/).
 *
 * `pwtest` captures logs from the tests (and the pipewire daemon, if
 * applicable) and collects the output into YAML files printed to `stderr`.
 *
 * ## Tests
 *
 * A `pwtest` test is declared with the `PWTEST()` macro and must return one of
 * the `pwtest` status codes. Those codes are:
 * - \ref PWTEST_PASS for a successful test
 * - \ref PWTEST_FAIL for a test case failure. Usually you should not return this
 *   value but rely on the `pwtest` macros to handle this case.
 * - \ref PWTEST_SKIP to skip the current test
 * - \ref PWTEST_SYSTEM_ERROR in case of an error that would cause the test to not run properly. This is not a test case failure but some required precondition not being met.
 *
 * ```c
 * #include "pwtest.h"
 *
 * PWTEST(some_test)
 * {
 *	int var = 10;
 *	const char *str = "foo";
 *
 *	if (access("/", R_OK))
 *	    pwtest_error_with_message("Oh dear, no root directory?");
 *
 *	if (today_is_monday)
 *	     return PWTEST_SKIP;
 *
 *	pwtest_int_lt(var, 20);
 *	pwtest_ptr_notnull(&var);
 *	pwtest_str_ne(str, "bar");
 *
 *	return PWTEST_PASS;
 * }
 * ...
 * ```
 *
 * `pwtest` provides comparison macros for most basic data types with the `lt`,
 * `le`, `eq`, `gt`, `ge` suffixes (`<, <=, ==, >, >=`). Tests usually should not
 * return `PWTEST_FAIL` directly, use the `pwtest_fail()` macros if .
 *
 * By default, a test runs in a forked process, any changes to the
 * process'environment, etc. are discarded in the next test.
 *
 * ## Suites
 *
 * Tests are grouped into suites and declared with the PWTEST_SUITE() macro.
 * Each test must be added with the required arguments, it is acceptable to
 * add the same test multiple times with different arguments.
 *
 * ```c
 * ...
 * PWTEST_SUITE(misc)
 * {
 *	if (today_is_monday)
 *	     return PWTEST_SKIP;
 *
 *	// simple test
 *	pwtest_add(some_test, PWTEST_NOARG);
 *	// starts with its own pipewire daemon instance
 *	pwtest_add(some_test, PWTEST_ARG_DAEMON);
 *
 *	return PWTEST_SUCCESS;
 * }
 * ```
 * For a list of potential arguments, see \ref pwtest_arg and the
 * `test-examples.c` file in the source directory.
 *
 * Suites are auto-discovered, they do not have to be manually added to a test run.
 *
 * ## Running tests
 *
 * The `pwtest` framework is built into each test binary, so just execute the
 * matching binary. See the `--help` output for the full argument list.
 *
 * The most useful arguments when running the test suite:
 * - `--verbose` to enable logs even when tests pass or are skipped
 * - `--filter-test=glob`, `--filter-suite=glob` uses an `fnmatch()` glob to limit which tests or suites are run
 * - `--no-fork` - see "Debugging test-case failures"
 *
 * ## Debugging test-case failures
 *
 * To debug a single test, disable forking and run the test through gdb:
 *
 * ```
 * $ gdb path/to/test
 * (gdb) break test_function_name
 * Breakpoint 1 at 0xfffffffffffff: file ../test/test-file.c, line 123
 * (gdb) r --no-fork --filter-test=test_function_name
 * ```
 * Disabling forking makes it easy to debug but should always be used with
 * `--filter-test`. Any test that modifies its environment will affect
 * subsequent tests and may invalidate the test results.
 *
 * Where a test has multiple iterations, use `--filter-iteration` to only run
 * one single iteration.
 */

/**
 * \addtogroup pwtest
 * \{
 */


/** \struct pwtest_context */
struct pwtest_context;
/** \struct pwtest_suite */
struct pwtest_suite;
/** \struct pwtest_test */
struct pwtest_test;

#include "pwtest-implementation.h"

/**
 * Result returned from tests or suites.
 */
enum pwtest_result {
	PWTEST_PASS = 75,		/**< test successful */
	PWTEST_FAIL = 76,		/**< test failed. Should not be returned directly,
					     Use the pwtest_ macros instead */
	PWTEST_SKIP = 77,		/**< test was skipped */
	PWTEST_TIMEOUT = 78,		/**< test aborted after timeout */
	PWTEST_SYSTEM_ERROR = 79,	/**< unrelated error occurred */
};

/**
 * If the test was added with a range (see \ref PWTEST_ARG_RANGE), this
 * function returns the current iteration within that range. Otherwise, this
 * function returns zero.
 */
int pwtest_get_iteration(struct pwtest_test *t);

/**
 * If the test had properties set (see \ref PWTEST_ARG_PROP), this function
 * returns the \ref pw_properties. Otherwise, this function returns NULL.
 */
struct pw_properties *pwtest_get_props(struct pwtest_test *t);

struct pwtest_context *pwtest_get_context(struct pwtest_test *t);

/** Fail the current test */
#define pwtest_fail() \
	_pwtest_fail_condition(PWTEST_FAIL, __FILE__, __LINE__, __func__, "aborting", "")

/** Same as above but more expressive in the code */
#define pwtest_fail_if_reached() \
	_pwtest_fail_condition(PWTEST_FAIL, __FILE__, __LINE__, __func__, "This line is supposed to be unreachable", "")

/** Fail the current test with the given message */
#define pwtest_fail_with_msg(...) \
	_pwtest_fail_condition(PWTEST_FAIL, __FILE__, __LINE__, __func__, \
			       "aborting", __VA_ARGS__)

/** Error out of the current test with the given message */
#define pwtest_error_with_msg(...) \
	_pwtest_fail_condition(PWTEST_SYSTEM_ERROR, __FILE__, __LINE__, __func__, \
			       "error", __VA_ARGS__)

/** Assert r is not -1 and if it is, print the errno */
#define pwtest_errno_ok(r_) \
	pwtest_errno_check(r_, 0);

/** Assert r is -1 and the errno is the given one */
#define pwtest_errno(r_, errno_) \
	pwtest_errno_check(r_, errno_);

/** Assert r is not < 0 and if it is assume it's a negative errno */
#define pwtest_neg_errno_ok(r_) \
	pwtest_neg_errno_check(r_, 0);

/** Assert r is < 0 and the given negative errno */
#define pwtest_neg_errno(r_, errno_) \
	pwtest_neg_errno_check(r_, errno_);

/** Assert boolean (a == b) */
#define pwtest_bool_eq(a_, b_) \
	pwtest_comparison_bool_(a_, ==, b_)

/** Assert boolean (a != b) */
#define pwtest_bool_ne(a_, b_) \
	pwtest_comparison_bool_(a_, !=, b_)

/** Assert cond to be true. Convenience wrapper for readability */
#define pwtest_bool_true(cond_) \
	pwtest_comparison_bool_(cond_, ==, true)

/** Assert cond to be false. Convenience wrapper for readability */
#define pwtest_bool_false(cond_) \
	pwtest_comparison_bool_(cond_, ==, false)

/** Assert a == b  */
#define pwtest_int_eq(a_, b_) \
	pwtest_comparison_int_(a_, ==, b_)

/** Assert a != b  */
#define pwtest_int_ne(a_, b_) \
	pwtest_comparison_int_(a_, !=, b_)

/** Assert a < b  */
#define pwtest_int_lt(a_, b_) \
	pwtest_comparison_int_(a_, <, b_)

/** Assert a <= b  */
#define pwtest_int_le(a_, b_) \
	pwtest_comparison_int_(a_, <=, b_)

/** Assert a >= b  */
#define pwtest_int_ge(a_, b_) \
	pwtest_comparison_int_(a_, >=, b_)

/** Assert a > b  */
#define pwtest_int_gt(a_, b_) \
	pwtest_comparison_int_(a_, >, b_)

/** Assert ptr1 == ptr2  */
#define pwtest_ptr_eq(a_, b_) \
	pwtest_comparison_ptr_(a_, ==, b_)

/** Assert ptr1 != ptr2  */
#define pwtest_ptr_ne(a_, b_) \
	pwtest_comparison_ptr_(a_, !=, b_)

/** Assert ptr == NULL  */
#define pwtest_ptr_null(a_) \
	pwtest_comparison_ptr_(a_, ==, NULL)

/** Assert ptr != NULL  */
#define pwtest_ptr_notnull(a_) \
	pwtest_comparison_ptr_(a_, !=, NULL)

/** Assert a == b for a (hardcoded) epsilon */
#define pwtest_double_eq(a_, b_)\
	pwtest_comparison_double_((a_), ==, (b_))

/** Assert a != b for a (hardcoded) epsilon */
#define pwtest_double_ne(a_, b_)\
	pwtest_comparison_double_((a_), !=, (b_))

/** Assert a < b for a (hardcoded) epsilon */
#define pwtest_double_lt(a_, b_)\
	pwtest_comparison_double_((a_), <, (b_))

/** Assert a <= b for a (hardcoded) epsilon */
#define pwtest_double_le(a_, b_)\
	pwtest_comparison_double_((a_), <=, (b_))

/** Assert a >= b for a (hardcoded) epsilon */
#define pwtest_double_ge(a_, b_)\
	pwtest_comparison_double_((a_), >=, (b_))

/** Assert a > b for a (hardcoded) epsilon */
#define pwtest_double_gt(a_, b_)\
	pwtest_comparison_double_((a_), >, (b_))

#define pwtest_int(a_, op_, b_) \
	pwtest_comparison_int_(a_, op_, b_)


/** Assert str1 is equal to str2 */
#define pwtest_str_eq(a_, b_) \
	do { \
		const char *_a = a_; \
		const char *_b = b_; \
		if (!spa_streq(_a, _b)) \
			_pwtest_fail_comparison_str(__FILE__, __LINE__, __func__, \
						     #a_ " equals " #b_, _a, _b); \
	} while(0)

/** Assert str1 is equal to str2 for l characters */
#define pwtest_str_eq_n(a_, b_, l_) \
	do { \
		const char *_a = a_; \
		const char *_b = b_; \
		if (!spa_strneq(_a, _b, l_)) \
			_pwtest_fail_comparison_str(__FILE__, __LINE__, __func__, \
						     #a_ " equals " #b_ ", len: " #l_, _a, _b); \
	} while(0)

/** Assert str1 is not equal to str2 */
#define pwtest_str_ne(a_, b_) \
	do { \
		const char *_a = a_; \
		const char *_b = b_; \
		if (spa_streq(_a, _b)) \
			_pwtest_fail_comparison_str(__FILE__, __LINE__, __func__, \
						    #a_ " not equal to " #b_, _a, _b); \
	} while(0)

/** Assert str1 is not equal to str2 for l characters */
#define pwtest_str_ne_n(a_, b_, l_) \
	do { \
		__typeof__(a_) _a = a_; \
		__typeof__(b_) _b = b_; \
		if (spa_strneq(_a, _b, l_)) \
			_pwtest_fail_comparison_str(__FILE__, __LINE__, __func__, \
						    #a_ " not equal to " #b_ ", len: " #l_, _a, _b); \
	} while(0)


/** Assert haystack contains needle */
#define pwtest_str_contains(haystack_, needle_) \
	do { \
		const char *_h = haystack_; \
		const char *_n = needle_; \
		if (!strstr(_h, _n)) \
			_pwtest_fail_comparison_str(__FILE__, __LINE__, __func__, \
						     #haystack_ " contains " #needle_, _h, _n); \
	} while(0)


/* Needs to be a #define NULL for SPA_SENTINEL */
enum pwtest_arg {
	PWTEST_NOARG = 0,
	/**
	 * The next argument is an int specifying the numerical signal number.
	 * The test is expected to raise that signal. The test fails if none
	 * or any other signal is raised.
	 *
	 * Example:
	 * ```c
	 *    pwtest_add(mytest, PWTEST_ARG_SIGNAL, SIGABRT);
	 * ```
	 */
	PWTEST_ARG_SIGNAL,
	/**
	 * The next two int arguments are the minimum (inclusive) and
	 * maximum (exclusive) range for this test.
	 *
	 * Example:
	 * ```c
	 *    pwtest_add(mytest, PWTEST_ARG_RANGE, -50, 50);
	 * ```
	 * Use pwtest_get_iteration() in the test function to obtain the current iteration.
	 */
	PWTEST_ARG_RANGE,
	/**
	 * The next two const char * arguments are the key and value
	 * for a property entry. This argument may be specified multiple times
	 * to add multiple properties.
	 *
	 * Use pwtest_get_props() to get the properties within the test function.
	 *
	 * Example:
	 * ```c
	 *    pwtest_add(mytest,
	 *               PWTEST_ARG_PROP, "key1", "value1",
	 *               PWTEST_ARG_PROP, "key2", "value2");
	 * ```
	 */
	PWTEST_ARG_PROP,
	/**
	 * The next two const char * arguments are the key and value
	 * for the environment variable to be set in the test. This argument
	 * may be specified multiple times to add multiple environment
	 * variables.
	 *
	 * Example:
	 * ```c
	 *    pwtest_add(mytest,
	 *               PWTEST_ARG_ENV, "env1", "value1",
	 *               PWTEST_ARG_ENV, "env2", "value2");
	 * ```
	 *
	 * These environment variables are only set for the test itself, a
	 * a pipewire daemon started with \ref PWTEST_ARG_DAEMON does not share
	 * those variables.
	 *
	 */
	PWTEST_ARG_ENV,
	/**
	 * Takes no extra arguments. If provided, the test case will start a
	 * pipewire daemon and stop the daemon when finished.
	 *
	 * The `PIPEWIRE_REMOTE` environment variable will be set in the
	 * test to point to this daemon.
	 *
	 * Example:
	 * ```c
	 *    pwtest_add(mytest, PWTEST_ARG_DAEMON);
	 * ```
	 *
	 * Environment variables specified with \ref PWTEST_ARG_ENV are
	 * **not** available to the daemon, only to the test itself.
	 */
	PWTEST_ARG_DAEMON,
};
/**
 * Add function \a func_ to the current test suite.
 *
 * This macro should be used within PWTEST_SUITE() to register the test in that suite, for example:
 *
 * ```c
 * PWTEST_SUITE(mysuite)
 * {
 *   pwtest_add(test1, PWTEST_NOARG);
 *   pwtest_add(test2, PWTEST_ARG_DAEMON);
 *   pwtest_add(test3, PWTEST_ARG_RANGE, 0, 100, PWTEST_ARG_DAEMON);
 * }
 *
 * ```
 *
 * If the test matches the given filters and the suite is executed, the test
 * will be executed with the parameters given to pwtest_add().
 *
 * Arguments take a argument-dependent number of extra parameters, see
 * see the \ref pwtest_arg documentation for details.
 */
#define pwtest_add(func_, ...) \
	_pwtest_add(ctx, suite, #func_, func_, __VA_ARGS__, NULL)


/**
 * Declare a test case. To execute the test, add the test case name with pwtest_add().
 *
 * This macro expands so each test has a struct \ref pwtest_test variable
 * named `current_test` available.
 *
 * ```c
 * PWTEST(mytest)
 * {
 *   struct pwtest_test *t = current_test;
 *
 *   ... do stuff ...
 *
 *   return PWTEST_PASS;
 * }
 *
 * PWTEST_SUITE(mysuite)
 * {
 *    pwtest_add(mytest);
 *
 *    return PWTEST_PASS;
 * }
 * ```
 */
#define PWTEST(tname) \
	static enum pwtest_result tname(struct pwtest_test *current_test)

/**
 * Initialize a test suite. A test suite is a group of related
 * tests that filters and other conditions may apply to.
 *
 * Test suites are automatically discovered at build-time.
 */
#define PWTEST_SUITE(cname) \
	static enum pwtest_result (cname##__setup)(struct pwtest_context *ctx, struct pwtest_suite *suite); \
	static const struct pwtest_suite_decl _test_suite \
	__attribute__((used)) \
	__attribute((section("pwtest_suite_section"))) = { \
	   .name = #cname, \
	   .setup = cname##__setup, \
	}; \
	static enum pwtest_result (cname##__setup)(struct pwtest_context *ctx, struct pwtest_suite *suite)

struct pwtest_spa_plugin {
#define PWTEST_PLUGIN_MAX 32
	size_t nsupport;
	struct spa_support support[PWTEST_PLUGIN_MAX];

	size_t ndlls;
	void *dlls[PWTEST_PLUGIN_MAX];

	size_t nhandles;
	struct spa_handle *handles[PWTEST_PLUGIN_MAX];
};

struct pwtest_spa_plugin* pwtest_spa_plugin_new(void);
void pwtest_spa_plugin_destroy(struct pwtest_spa_plugin *plugin);

/**
 * Identical to pwtest_spa_plugin_try_load_interface() but returns the
 * interface and fails if the interface is NULL.
 */
void*
pwtest_spa_plugin_load_interface(struct pwtest_spa_plugin *plugin,
				 const char *libname,
				 const char *factory_name,
				 const char *interface_name,
				 const struct spa_dict *info);

/**
 * Load \a interface_name from the factory in \a libname.
 * If successful, the interface is returned and added to \a plugin's
 * support items, i.e. subsequent loads of an interface will be able to
 * make use of previously loaded ones.
 *
 * \return 0 on success or a negative errno on error
 * \retval -ENOENT \a libname does not exist
 * \retval -EINVAL \a factory_name does not exist in \a libname
 * \retval -ENOSYS \a interface_name does not exist in \a factory_name
 */
int
pwtest_spa_plugin_try_load_interface(struct pwtest_spa_plugin *plugin,
				     void **iface_return,
				     const char *libname,
				     const char *factory_name,
				     const char *interface_name,
				     const struct spa_dict *info);



/**
 * Create a temporary file and copy its full path to \a path. Fails the test
 * with \ref PWTEST_SYSTEM_ERROR on error.
 *
 * This file does not need to be removed by the test, the pwtest framework
 * will take care of it on exit.
 */
void pwtest_mkstemp(char path[PATH_MAX]);


/**
 * \}
 */

#ifdef __cplusplus
}
#endif

#endif /* PWTEST_H */
