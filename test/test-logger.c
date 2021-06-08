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

#include <unistd.h>

#include <spa/utils/ansi.h>
#include <spa/utils/names.h>
#include <spa/support/plugin.h>
#include <spa/support/log.h>
#include <pipewire/pipewire.h>

PWTEST(logger_truncate_long_lines)
{
	struct pwtest_spa_plugin *plugin;
	void *iface;
	char fname[PATH_MAX];
	struct spa_dict_item item;
	struct spa_dict info;
	char buffer[1024];
	FILE *fp;
	bool mark_line_found = false;

	pw_init(0, NULL);

	pwtest_mkstemp(fname);
	item = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_FILE, fname);
	info = SPA_DICT_INIT(&item, 1);
	plugin = pwtest_spa_plugin_new();
	iface = pwtest_spa_plugin_load_interface(plugin, "support/libspa-support",
						 SPA_NAME_SUPPORT_LOG, SPA_TYPE_INTERFACE_Log,
						 &info);
	pwtest_ptr_notnull(iface);

	/* Print a line expected to be truncated */
	spa_log_error(iface, "MARK: %1100s", "foo");

	fp = fopen(fname, "r");
	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		if (strstr(buffer, "MARK:")) {
			const char *suffix = ".. (truncated)\n";
			int len = strlen(buffer);
			pwtest_str_eq(buffer + len - strlen(suffix), suffix);
			mark_line_found = true;
			break;
		}
	}

	fclose(fp);

	pwtest_bool_true(mark_line_found);
	pwtest_spa_plugin_destroy(plugin);

	return PWTEST_PASS;
}

PWTEST(logger_no_ansi)
{
	struct pwtest_spa_plugin *plugin;
	void *iface;
	char fname[PATH_MAX];
	struct spa_dict_item items[2];
	struct spa_dict info;
	char buffer[1024];
	FILE *fp;
	bool mark_line_found = false;

	pw_init(0, NULL);

	pwtest_mkstemp(fname);
	items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_FILE, fname);
	items[1] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_COLORS, "true");
	info = SPA_DICT_INIT(items, 2);
	plugin = pwtest_spa_plugin_new();
	iface = pwtest_spa_plugin_load_interface(plugin, "support/libspa-support",
						 SPA_NAME_SUPPORT_LOG, SPA_TYPE_INTERFACE_Log,
						 &info);
	pwtest_ptr_notnull(iface);

	/* Print a line usually containing a color sequence, but we're not a
	 * tty so expect none despite colors being enabled */
	spa_log_error(iface, "MARK\n");

	fp = fopen(fname, "r");
	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		if (strstr(buffer, "MARK")) {
			mark_line_found = true;
			pwtest_ptr_null(strstr(buffer, SPA_ANSI_RESET));
			pwtest_ptr_null(strstr(buffer, SPA_ANSI_RED));
			pwtest_ptr_null(strstr(buffer, SPA_ANSI_BRIGHT_RED));
			pwtest_ptr_null(strstr(buffer, SPA_ANSI_BOLD_RED));
		}
	}

	fclose(fp);

	pwtest_bool_true(mark_line_found);
	pwtest_spa_plugin_destroy(plugin);

	return PWTEST_PASS;
}

PWTEST_SUITE(logger)
{
	pwtest_add(logger_truncate_long_lines, PWTEST_NOARG);
	pwtest_add(logger_no_ansi, PWTEST_NOARG);

	return PWTEST_PASS;
}
