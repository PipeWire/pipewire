/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Red Hat, Inc. */
/* SPDX-License-Identifier: MIT */

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
