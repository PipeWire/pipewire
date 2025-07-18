/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <limits.h>
#include <signal.h>
#include <getopt.h>
#include <libgen.h>
#include <locale.h>

#include <spa/utils/result.h>
#include <spa/utils/cleanup.h>
#include <spa/debug/file.h>

#include <pipewire/pipewire.h>

#include <pipewire/i18n.h>

static void do_quit(void *data, int signal_number)
{
	struct pw_main_loop *loop = data;
	pw_main_loop_quit(loop);
}

static void show_help(const char *name, const char *config_name)
{
	fprintf(stdout, _("%s [options]\n"
		"  -h, --help                            Show this help\n"
		"  -v, --verbose                         Increase verbosity by one level\n"
		"      --version                         Show version\n"
		"  -c, --config                          Load config (Default %s)\n"
		"  -P  --properties                      Set context properties\n"),
		name,
		config_name);
}

int main(int argc, char *argv[])
{
	struct pw_context *context = NULL;
	struct pw_main_loop *loop = NULL;
	struct pw_properties *properties = NULL;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ "config",	required_argument,	NULL, 'c' },
		{ "verbose",	no_argument,		NULL, 'v' },
		{ "properties",	required_argument,	NULL, 'P' },

		{ NULL, 0, NULL, 0}
	};
	int c, res = 0;
	char path[PATH_MAX];
	const char *config_name;
	enum spa_log_level level;
	struct spa_error_location loc;

	if (setenv("PIPEWIRE_INTERNAL", "1", 1) < 0)
		fprintf(stderr, "can't set PIPEWIRE_INTERNAL env: %m");

	snprintf(path, sizeof(path), "%s.conf", argv[0]);
	config_name = basename(path);

	setlocale(LC_ALL, "");
	pw_init(&argc, &argv);

	level = pw_log_level;

	properties = pw_properties_new(
				PW_KEY_CONFIG_NAME, config_name,
				NULL);

	while ((c = getopt_long(argc, argv, "hVc:vP:", long_options, NULL)) != -1) {
		switch (c) {
		case 'v':
			if (level < SPA_LOG_LEVEL_TRACE)
				pw_log_set_level(++level);
			break;
		case 'h':
			show_help(argv[0], config_name);
			return 0;
		case 'V':
			fprintf(stdout, "%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'c':
			config_name = optarg;
			pw_properties_set(properties, PW_KEY_CONFIG_NAME, config_name);
			break;

		case 'P':
			if (pw_properties_update_string_checked(properties, optarg, strlen(optarg), &loc) < 0) {
				spa_debug_file_error_location(stderr, &loc,
						"error: syntax error in --properties: %s",
						loc.reason);
				goto done;
			}
			break;

		default:
			res = -EINVAL;
			goto done;
		}
	}

	loop = pw_main_loop_new(&properties->dict);
	if (loop == NULL) {
		pw_log_error("failed to create main-loop: %m");
		res = -errno;
		goto done;
	}

	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGINT, do_quit, loop);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGTERM, do_quit, loop);

	context = pw_context_new(pw_main_loop_get_loop(loop), spa_steal_ptr(properties), 0);

	if (context == NULL) {
		pw_log_error("failed to create context: %m");
		res = -errno;
		goto done;
	}

	pw_log_info("start main loop");
	pw_main_loop_run(loop);
	pw_log_info("leave main loop");

done:
	pw_properties_free(properties);
	if (context)
		pw_context_destroy(context);
	if (loop)
		pw_main_loop_destroy(loop);
	pw_deinit();

	return res;
}
