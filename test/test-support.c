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

#include "pwtest.h"

#include <spa/utils/names.h>
#include <spa/support/plugin.h>
#include <spa/support/log.h>

PWTEST(pwtest_load_nonexisting)
{
	struct pwtest_spa_plugin *plugin;
	void *iface;

	plugin = pwtest_spa_plugin_new();

	pwtest_neg_errno_check(
		pwtest_spa_plugin_try_load_interface(plugin, &iface,
					"support/does_not_exist",
					SPA_NAME_SUPPORT_LOG, SPA_TYPE_INTERFACE_Log,
					NULL),
		-ENOENT);

	pwtest_neg_errno_check(
		pwtest_spa_plugin_try_load_interface(plugin, &iface,
					"support/libspa-support",
					"foo.bar", SPA_TYPE_INTERFACE_Log,
					NULL),
		-EINVAL);

	pwtest_neg_errno_check(
		pwtest_spa_plugin_try_load_interface(plugin, &iface,
					"support/libspa-support",
					SPA_NAME_SUPPORT_LOG,
					"foo", NULL),
		-ENOSYS);

	pwtest_spa_plugin_destroy(plugin);

	return PWTEST_PASS;
}

PWTEST(pwtest_load_plugin)
{
	struct pwtest_spa_plugin *plugin;
	void *iface;

	plugin = pwtest_spa_plugin_new();

	pwtest_neg_errno_ok(
		pwtest_spa_plugin_try_load_interface(plugin, &iface,
					"support/libspa-support",
					SPA_NAME_SUPPORT_LOG, SPA_TYPE_INTERFACE_Log,
					NULL)
		);

	pwtest_spa_plugin_destroy(plugin);
	return PWTEST_PASS;
}

PWTEST_SUITE(support)
{
	pwtest_add(pwtest_load_nonexisting, PWTEST_NOARG);
	pwtest_add(pwtest_load_plugin, PWTEST_NOARG);

	return PWTEST_PASS;
}
