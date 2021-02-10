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
#include <limits.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/wait.h>

#include <spa/utils/result.h>
#include <spa/utils/json.h>

#include <pipewire/impl.h>

#include "config.h"

#define NAME "daemon"

#define DEFAULT_CONFIG_FILE "pipewire.conf"

struct data {
	struct pw_context *context;
	struct pw_main_loop *loop;

	const char *daemon_name;
};

static void do_quit(void *data, int signal_number)
{
	struct data *d = data;
	pw_main_loop_quit(d->loop);
}

static void show_help(struct data *d, const char *name)
{
	fprintf(stdout, "%s [options]\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -n, --name                            Daemon name (Default %s)\n",
		name,
		d->daemon_name);
}

int main(int argc, char *argv[])
{
	struct data d;
	struct pw_properties *properties;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ "name",	required_argument,	NULL, 'n' },

		{ NULL, 0, NULL, 0}
	};
	int c;

	if (setenv("PIPEWIRE_INTERNAL", "1", 1) < 0)
		fprintf(stderr, "can't set PIPEWIRE_INTERNAL env: %m");

	spa_zero(d);
	pw_init(&argc, &argv);

	d.daemon_name = getenv("PIPEWIRE_CORE");
	if (d.daemon_name == NULL)
		d.daemon_name = PW_DEFAULT_REMOTE;

	while ((c = getopt_long(argc, argv, "hVn:", long_options, NULL)) != -1) {
		switch (c) {
		case 'h' :
			show_help(&d, argv[0]);
			return 0;
		case 'V' :
			fprintf(stdout, "%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'n' :
			d.daemon_name = optarg;
			fprintf(stdout, "set name %s\n", d.daemon_name);
			break;
		default:
			return -1;
		}
	}

	properties = pw_properties_new(
                                PW_KEY_CORE_NAME, d.daemon_name,
                                PW_KEY_CONFIG_NAME, "pipewire-uninstalled.conf",
                                PW_KEY_CORE_DAEMON, "true", NULL);

	d.loop = pw_main_loop_new(&properties->dict);
	if (d.loop == NULL) {
		pw_log_error("failed to create main-loop: %m");
		return -1;
	}

	pw_loop_add_signal(pw_main_loop_get_loop(d.loop), SIGINT, do_quit, &d);
	pw_loop_add_signal(pw_main_loop_get_loop(d.loop), SIGTERM, do_quit, &d);

	d.context = pw_context_new(pw_main_loop_get_loop(d.loop), properties, 0);
	if (d.context == NULL) {
		pw_log_error("failed to create context: %m");
		return -1;
	}

	pw_log_info("start main loop");
	pw_main_loop_run(d.loop);
	pw_log_info("leave main loop");

	pw_context_destroy(d.context);
	pw_main_loop_destroy(d.loop);
	pw_deinit();

	return 0;
}
