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

#ifdef HAVE_SYSTEMD
#include <systemd/sd-journal.h>
#endif

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
	pw_deinit();

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
	pw_deinit();

	return PWTEST_PASS;
}

static void
test_log_levels(enum spa_log_level level)
{
	struct pwtest_spa_plugin *plugin;
	void *iface;
	char fname[PATH_MAX];
	struct spa_dict_item items[2];
	struct spa_dict info;
	char buffer[1024];
	FILE *fp;
	bool above_level_found = false;
	bool below_level_found = false;
	bool current_level_found = false;

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
	pw_log_set(iface);

	/* current level is whatever the iteration is. Log one line
	 * with our level, one with a level above (should never show up)
	 * and one with a level below (should show up).
	 */
	if (level > SPA_LOG_LEVEL_NONE)
		pw_log(level, "CURRENT");
	if (level > SPA_LOG_LEVEL_ERROR)
		pw_log(level - 1, "BELOW");
	if (level < SPA_LOG_LEVEL_TRACE)
		pw_log(level + 1, "ABOVE");

	fp = fopen(fname, "r");
	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		if (strstr(buffer, "CURRENT"))
			current_level_found = true;
		else if (strstr(buffer, "ABOVE"))
			above_level_found = true;
		else if (strstr(buffer, "BELOW"))
			below_level_found = true;
	}

	fclose(fp);

	/* Anything on a higher level than ours should never show up */
	pwtest_bool_false(above_level_found);
	if (level == SPA_LOG_LEVEL_NONE) {
		pwtest_bool_false(current_level_found);
		pwtest_bool_false(below_level_found);
	} else if (level == SPA_LOG_LEVEL_ERROR) {
		pwtest_bool_true(current_level_found);
		pwtest_bool_false(below_level_found);
	} else {
		pwtest_bool_true(current_level_found);
		pwtest_bool_true(below_level_found);
	}
	pwtest_spa_plugin_destroy(plugin);
	pw_deinit();
}

PWTEST(logger_levels)
{
	enum spa_log_level level = pwtest_get_iteration(current_test);
	enum spa_log_level default_level = pw_log_level;
	struct spa_log *default_logger = pw_log_get();
	char *oldenv = getenv("PIPEWIRE_DEBUG");

	if (oldenv)
		oldenv = strdup(oldenv);
	unsetenv("PIPEWIRE_DEBUG");

	pw_log_set_level(level);

	test_log_levels(level);

	if (oldenv) {
		setenv("PIPEWIRE_DEBUG", oldenv, 1);
		free(oldenv);
	}

	pw_log_set(default_logger);
	pw_log_set_level(default_level);

	return PWTEST_PASS;
}

PWTEST(logger_debug_env)
{
	enum spa_log_level level = pwtest_get_iteration(current_test);
	enum spa_log_level default_level = pw_log_level;
	struct spa_log *default_logger = pw_log_get();
	char lvl[2] = {0};
	char *oldenv = getenv("PIPEWIRE_DEBUG");

	if (oldenv)
		oldenv = strdup(oldenv);

	spa_scnprintf(lvl, sizeof(lvl), "%d", level);
	setenv("PIPEWIRE_DEBUG", lvl, 1);

	/* Disable logging, let PIPEWIRE_DEBUG set the level */
	pw_log_set_level(SPA_LOG_LEVEL_NONE);

	test_log_levels(level);

	if (oldenv) {
		setenv("PIPEWIRE_DEBUG", oldenv, 1);
		free(oldenv);
	} else {
		unsetenv("PIPEWIRE_DEBUG");
	}

	pw_log_set(default_logger);
	pw_log_set_level(default_level);

	return PWTEST_PASS;
}

PWTEST(logger_debug_env_alpha)
{
	enum spa_log_level level = pwtest_get_iteration(current_test);
	enum spa_log_level default_level = pw_log_level;
	struct spa_log *default_logger = pw_log_get();
	char *lvl = NULL;
	char *oldenv = getenv("PIPEWIRE_DEBUG");

	if (oldenv)
		oldenv = strdup(oldenv);

	switch(level) {
		case SPA_LOG_LEVEL_NONE:  lvl = "X"; break;
		case SPA_LOG_LEVEL_ERROR: lvl = "E"; break;
		case SPA_LOG_LEVEL_WARN:  lvl = "W"; break;
		case SPA_LOG_LEVEL_INFO:  lvl = "I"; break;
		case SPA_LOG_LEVEL_DEBUG: lvl = "D"; break;
		case SPA_LOG_LEVEL_TRACE: lvl = "T"; break;
		default:
			pwtest_fail_if_reached();
			break;
	}
	setenv("PIPEWIRE_DEBUG", lvl, 1);

	/* Disable logging, let PIPEWIRE_DEBUG set the level */
	pw_log_set_level(SPA_LOG_LEVEL_NONE);

	test_log_levels(level);

	if (oldenv) {
		setenv("PIPEWIRE_DEBUG", oldenv, 1);
		free(oldenv);
	} else {
		unsetenv("PIPEWIRE_DEBUG");
	}

	pw_log_set(default_logger);
	pw_log_set_level(default_level);

	return PWTEST_PASS;
}

PWTEST(logger_journal)
{
	enum pwtest_result result = PWTEST_SKIP;
#ifdef HAVE_SYSTEMD
	struct pwtest_spa_plugin *plugin;
	void *iface;
	struct spa_dict_item items[2];
	struct spa_dict info;
	bool mark_line_found = false;
	sd_journal *journal;
	int rc;

	pw_init(0, NULL);

	items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_LEVEL, "4"); /* debug */
	info = SPA_DICT_INIT(items, 1);
	plugin = pwtest_spa_plugin_new();
	iface = pwtest_spa_plugin_load_interface(plugin, "support/libspa-journal",
						 SPA_NAME_SUPPORT_LOG, SPA_TYPE_INTERFACE_Log,
						 &info);
	pwtest_ptr_notnull(iface);

	rc = sd_journal_open(&journal, SD_JOURNAL_LOCAL_ONLY|SD_JOURNAL_CURRENT_USER);
	pwtest_neg_errno_ok(rc);

	sd_journal_seek_head(journal);
	if (sd_journal_next(journal) == 0) { /* No entries? We don't have a journal */
		goto cleanup;
	}

	sd_journal_seek_tail(journal);

	spa_log_info(iface, "MARK\n");
	while ((rc = sd_journal_next(journal)) > 0) {
		char buffer[1024] = {0};
		const char *d;
		size_t l;
		int r = sd_journal_get_data(journal, "MESSAGE", (const void **)&d, &l);
		pwtest_neg_errno_ok(r);
		spa_scnprintf(buffer, sizeof(buffer), "%.*s", (int) l, d);
		if (strstr(buffer, "MARK"))
			mark_line_found = true;
	}
	pwtest_neg_errno_ok(rc);
	pwtest_bool_true(mark_line_found);

	result = PWTEST_PASS;

cleanup:
	sd_journal_close(journal);
	pwtest_spa_plugin_destroy(plugin);
	pw_deinit();
#endif
	return result;
}

PWTEST(logger_journal_chain)
{
	enum pwtest_result result = PWTEST_SKIP;
#ifdef HAVE_SYSTEMD
	struct pwtest_spa_plugin *plugin;
	void *iface_log;
	void *iface;
	char fname[PATH_MAX];
	char buffer[1024];
	FILE *fp;
	struct spa_dict_item items[2];
	struct spa_dict info;
	bool mark_line_found = false;
	sd_journal *journal;
	int rc;

	pw_init(0, NULL);
	pwtest_mkstemp(fname);

	/* Load a normal logger interface first, writing to fname */
	items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_FILE, fname);
	items[1] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_LEVEL, "4"); /* debug */
	info = SPA_DICT_INIT(items, 2);
	plugin = pwtest_spa_plugin_new();
	iface_log = pwtest_spa_plugin_load_interface(plugin, "support/libspa-support",
						 SPA_NAME_SUPPORT_LOG, SPA_TYPE_INTERFACE_Log,
						 &info);
	pwtest_ptr_notnull(iface_log);

	/* Load the journal logger, it should chain to the above */
	items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_LEVEL, "4"); /* debug */
	info = SPA_DICT_INIT(&items[1], 1);
	iface = pwtest_spa_plugin_load_interface(plugin, "support/libspa-journal",
						 SPA_NAME_SUPPORT_LOG, SPA_TYPE_INTERFACE_Log,
						 &info);
	pwtest_ptr_notnull(iface);

	rc = sd_journal_open(&journal, SD_JOURNAL_LOCAL_ONLY);
	pwtest_neg_errno_ok(rc);
	sd_journal_seek_head(journal);
	if (sd_journal_next(journal) == 0) { /* No entries? We don't have a journal */
		goto cleanup;
	}

	sd_journal_seek_tail(journal);

	spa_log_info(iface, "MARK\n");
	while ((rc = sd_journal_next(journal)) > 0) {
		char buffer[1024] = {0};
		const char *d;
		size_t l;
		int r = sd_journal_get_data(journal, "MESSAGE", (const void **)&d, &l);
		pwtest_neg_errno_ok(r);
		spa_scnprintf(buffer, sizeof(buffer), "%.*s", (int) l, d);
		if (strstr(buffer, "MARK"))
			mark_line_found = true;
	}
	pwtest_neg_errno_ok(rc);
	pwtest_bool_true(mark_line_found);

	/* Now check that the line is in the chained file logger too */
	mark_line_found = false;
	fp = fopen(fname, "r");
	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		if (strstr(buffer, "MARK"))
			mark_line_found = true;
	}

	fclose(fp);

	result = PWTEST_PASS;
	pwtest_bool_true(mark_line_found);

cleanup:
	sd_journal_close(journal);
	pwtest_spa_plugin_destroy(plugin);
	pw_deinit();

#endif
	return result;
}

PWTEST_SUITE(logger)
{
	pwtest_add(logger_truncate_long_lines, PWTEST_NOARG);
	pwtest_add(logger_no_ansi, PWTEST_NOARG);
	pwtest_add(logger_levels,
		   PWTEST_ARG_RANGE, SPA_LOG_LEVEL_NONE, SPA_LOG_LEVEL_TRACE + 1,
		   PWTEST_NOARG);
	pwtest_add(logger_debug_env,
		   PWTEST_ARG_RANGE, SPA_LOG_LEVEL_NONE, SPA_LOG_LEVEL_TRACE + 1,
		   PWTEST_NOARG);
	pwtest_add(logger_debug_env_alpha,
		   PWTEST_ARG_RANGE, SPA_LOG_LEVEL_NONE, SPA_LOG_LEVEL_TRACE + 1,
		   PWTEST_NOARG);
	pwtest_add(logger_journal, PWTEST_NOARG);
	pwtest_add(logger_journal_chain, PWTEST_NOARG);

	return PWTEST_PASS;
}
