/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Red Hat, Inc. */
/* SPDX-License-Identifier: MIT */

#include "pwtest.h"

#include <errno.h>
#include <fcntl.h>
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

	fp = fopen(fname, "re");
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

	fp = fopen(fname, "re");
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
	char fname[PATH_MAX];
	char buffer[1024];
	FILE *fp;
	bool above_level_found = false;
	bool below_level_found = false;
	bool current_level_found = false;
	char *oldenv = getenv("PIPEWIRE_LOG");

	pwtest_mkstemp(fname);
	setenv("PIPEWIRE_LOG", fname, 1);

	pw_init(0, NULL);

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

	fp = fopen(fname, "re");
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
	pw_deinit();

	if (oldenv)
		setenv("PIPEWIRE_LOG", oldenv, 1);
	else
		unsetenv("PIPEWIRE_LOG");
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
	const char *lvl = NULL;
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

PWTEST(logger_debug_env_topic_all)
{
	enum spa_log_level level = pwtest_get_iteration(current_test);
	enum spa_log_level default_level = pw_log_level;
	struct spa_log *default_logger = pw_log_get();
	char *oldenv = getenv("PIPEWIRE_DEBUG");
	char lvlstr[32];
	const char *lvl = "X";

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

	/* Check that the * glob works to enable all topics */
	spa_scnprintf(lvlstr, sizeof(lvlstr), "*:%s", lvl);
	setenv("PIPEWIRE_DEBUG", lvlstr, 1);

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

PWTEST(logger_debug_env_invalid)
{
	enum spa_log_level default_level = pw_log_level;
	struct spa_log *default_logger = pw_log_get();
	char *oldenv = getenv("PIPEWIRE_DEBUG");
	char fname[PATH_MAX];
	char buf[1024] = {0};
	int fd;
	int rc;
	bool error_message_found = false;
	long unsigned int which = pwtest_get_iteration(current_test);
	const char *envvars[] = {
		"invalid value",
		"*:5,some invalid value",
		"*:W,foo.bar:3,invalid:",
		"*:W,2,foo.bar:Q",
		"*:W,7,foo.bar:D",
		"*:W,Q,foo.bar:5",
		"*:W,D,foo.bar:8",
	};

	pwtest_int_lt(which, SPA_N_ELEMENTS(envvars));

	if (oldenv)
		oldenv = strdup(oldenv);

	/* The error message during pw_init() will go to stderr because no
	 * logger has been set up yet. Intercept that in our temp file */
	pwtest_mkstemp(fname);
	fd = open(fname, O_RDWR);
	pwtest_errno_ok(fd);
	rc = dup2(fd, STDERR_FILENO);
	setlinebuf(stderr);
	pwtest_errno_ok(rc);

	setenv("PIPEWIRE_DEBUG", envvars[which], 1);
	pw_init(0, NULL);

	fsync(STDERR_FILENO);
	lseek(fd, SEEK_SET, 0);
	while ((rc = read(fd, buf, sizeof(buf) - 1) > 0)) {
		if (strstr(buf, "Ignoring invalid format in PIPEWIRE_DEBUG")) {
		    error_message_found = true;
		    break;
		}
	}
	pwtest_errno_ok(rc);
	close(fd);
	pwtest_bool_true(error_message_found);

	if (oldenv) {
		setenv("PIPEWIRE_DEBUG", oldenv, 1);
		free(oldenv);
	} else {
		unsetenv("PIPEWIRE_DEBUG");
	}

	pw_log_set(default_logger);
	pw_log_set_level(default_level);
	pw_deinit();

	return PWTEST_PASS;
}

PWTEST(logger_topics)
{
	struct pwtest_spa_plugin *plugin;
	void *iface;
	char fname[PATH_MAX];
	struct spa_dict_item items[2];
	struct spa_dict info;
	char buffer[1024];
	FILE *fp;
	bool mark_line_found = false;
	struct spa_log_topic topic = {
		.version = 0,
		.topic = "my topic",
		.level = SPA_LOG_LEVEL_DEBUG,
	};

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

	spa_logt_info(iface, &topic, "MARK\n");

	fp = fopen(fname, "re");
	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		if (strstr(buffer, "MARK")) {
			mark_line_found = true;
			pwtest_str_contains(buffer, "my topic");
		}
	}

	pwtest_bool_true(mark_line_found);
	pwtest_spa_plugin_destroy(plugin);

	fclose(fp);

	return PWTEST_PASS;
}

#ifdef HAVE_SYSTEMD
static enum pwtest_result
find_in_journal(sd_journal *journal, const char *needle, char *out, size_t out_sz)
{
	int rc;
	int i;

	/* We give ourselves up to a second for our message to appear */
	for (i = 0; i < 10; i++) {
		int activity = sd_journal_wait(journal, 100000);

		pwtest_neg_errno_ok(activity);
		switch (activity) {
		case SD_JOURNAL_NOP:
			break;
		case SD_JOURNAL_INVALIDATE:
		case SD_JOURNAL_APPEND:
			while ((rc = sd_journal_next(journal)) > 0) {
				char buffer[1024] = {0};
				const char *d;
				size_t l;
				int r = sd_journal_get_data(journal, "MESSAGE", (const void **)&d, &l);
				if (r == -ENOENT || r == -E2BIG || r == -EBADMSG)
					continue;

				pwtest_neg_errno_ok(r);
				spa_scnprintf(buffer, sizeof(buffer), "%.*s", (int) l, d);
				if (strstr(buffer, needle)) {
					spa_scnprintf(out, out_sz, "%s", buffer);
					return PWTEST_PASS;
				}
			}
			pwtest_neg_errno_ok(rc);
			break;
		default:
			break;
		}
	}
	return PWTEST_FAIL;
}
#endif

PWTEST(logger_journal)
{
	enum pwtest_result result = PWTEST_SKIP;
#ifdef HAVE_SYSTEMD
	struct pwtest_spa_plugin *plugin;
	void *iface;
	struct spa_dict_item items[2];
	struct spa_dict info;
	struct spa_log_topic topic = {
		.version = 0,
		.topic = "pwtest journal",
		.level = SPA_LOG_LEVEL_DEBUG,
	};
	char buffer[1024] = {0};
	sd_journal *journal;
	int rc;
	char token[64];

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
	sd_journal_previous(journal);

	spa_scnprintf(token, sizeof(token), "MARK %s:%d", __func__, __LINE__);
	spa_logt_info(iface, &topic, "%s", token);

	result = find_in_journal(journal, token, buffer, sizeof(buffer));
	pwtest_int_eq((int)result, PWTEST_PASS);
	pwtest_str_contains(buffer, "pwtest journal");

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
	struct spa_log_topic topic = {
		.version = 0,
		.topic = "pwtest journal",
		.level = SPA_LOG_LEVEL_DEBUG,
	};
	sd_journal *journal;
	int rc;
	char token[64];

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
	sd_journal_previous(journal);

	spa_scnprintf(token, sizeof(token), "MARK %s:%d", __func__, __LINE__);

	spa_logt_info(iface, &topic, "%s", token);
	result = find_in_journal(journal, token, buffer, sizeof(buffer));
	pwtest_int_eq((int)result, PWTEST_PASS);
	pwtest_str_contains(buffer, "pwtest journal");

	/* Now check that the line is in the chained file logger too */
	spa_memzero(buffer, sizeof(buffer));
	mark_line_found = false;
	fp = fopen(fname, "re");
	while (fgets(buffer, sizeof(buffer), fp) != NULL) {
		if (strstr(buffer, token)) {
			mark_line_found = true;
			pwtest_ptr_null(strstr(buffer, SPA_ANSI_RESET));
			pwtest_ptr_null(strstr(buffer, SPA_ANSI_RED));
			pwtest_ptr_null(strstr(buffer, SPA_ANSI_BRIGHT_RED));
			pwtest_ptr_null(strstr(buffer, SPA_ANSI_BOLD_RED));
		}
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
	pwtest_add(logger_debug_env_topic_all,
		   PWTEST_ARG_RANGE, SPA_LOG_LEVEL_NONE, SPA_LOG_LEVEL_TRACE + 1,
		   PWTEST_NOARG);
	pwtest_add(logger_debug_env_invalid,
		   PWTEST_ARG_RANGE, 0, 7, /* see the test */
		   PWTEST_NOARG);
	pwtest_add(logger_topics, PWTEST_NOARG);
	pwtest_add(logger_journal, PWTEST_NOARG);
	pwtest_add(logger_journal_chain, PWTEST_NOARG);

	return PWTEST_PASS;
}
