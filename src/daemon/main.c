/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
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

#include <signal.h>
#include <getopt.h>

#include <pipewire/pipewire.h>
#include <pipewire/core.h>
#include <pipewire/module.h>

#include "config.h"
#include "daemon-config.h"

static const char *daemon_name = "pipewire-0";

static void do_quit(void *data, int signal_number)
{
	struct pw_main_loop *loop = data;
	pw_main_loop_quit(loop);
}

static void show_help(const char *name)
{
	fprintf(stdout, "%s [options]\n"
             "  -h, --help                            Show this help\n"
             "  -v, --version                         Show version\n"
             "  -n, --name                            Daemon name (Default %s)\n",
	     name,
	     daemon_name);
}

int main(int argc, char *argv[])
{
	struct pw_core *core;
	struct pw_main_loop *loop;
	struct pw_daemon_config *config;
	char *err = NULL;
	struct pw_properties *props;
	static const struct option long_options[] = {
		{"help",	0, NULL, 'h'},
		{"version",	0, NULL, 'v'},
		{"name",	1, NULL, 'n'},
		{NULL,		0, NULL, 0}
	};
	int c;

	pw_init(&argc, &argv);

	while ((c = getopt_long(argc, argv, "hvn:", long_options, NULL)) != -1) {
		switch (c) {
		case 'h' :
			show_help(argv[0]);
			return 0;
		case 'v' :
			fprintf(stdout, "%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'n' :
			daemon_name = optarg;
			fprintf(stdout, "set name %s\n", daemon_name);
			break;
		default:
			return -1;
		}
	}

	/* parse configuration */
	config = pw_daemon_config_new();
	if (pw_daemon_config_load(config, &err) < 0) {
		pw_log_error("failed to parse config: %s", err);
		free(err);
		return -1;
	}

	props = pw_properties_new(PW_CORE_PROP_NAME, daemon_name,
				  PW_CORE_PROP_DAEMON, "1", NULL);

	loop = pw_main_loop_new(props);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGINT, do_quit, loop);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGTERM, do_quit, loop);

	core = pw_core_new(pw_main_loop_get_loop(loop), props, 0);

	if (pw_daemon_config_run_commands(config, core) < 0) {
		pw_log_error("failed to run config commands");
		return -1;
	}

	pw_log_info("start main loop");
	pw_main_loop_run(loop);
	pw_log_info("leave main loop");

	pw_daemon_config_free(config);
	pw_core_destroy(core);
	pw_main_loop_destroy(loop);

	return 0;
}
