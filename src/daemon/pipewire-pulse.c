/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
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

#include <spa/utils/result.h>

#include <pipewire/impl.h>

#include "config.h"

static const char *address;

static void do_quit(void *data, int signal_number)
{
	struct pw_main_loop *loop = data;
	pw_main_loop_quit(loop);
}

static void show_help(const char *name)
{
	fprintf(stdout, "%s [options]\n\n"
		"Start a pulseaudio compatible daemon.\n\n"
		"  -h, --help                            Show this help\n"
		"      --version                         Show version\n"
		"  -a  --address                         comma separated list of addresses (Default %s)\n"
		"                                           unix:<socket-name>\n"
		"                                           tcp:[<ip>][:<port>]\n",
		name,
		address);
}

int main(int argc, char *argv[])
{
	struct pw_context *context;
	struct pw_main_loop *loop;
	struct pw_properties *properties;
	char *args;
	static const struct option long_options[] = {
		{ "help",	no_argument,		NULL, 'h' },
		{ "version",	no_argument,		NULL, 'V' },
		{ "address",	required_argument,	NULL, 'a' },
		{ NULL, 0, NULL, 0}
	};
	int c;

	pw_init(&argc, &argv);

	address = "unix:native";

	while ((c = getopt_long(argc, argv, "hVa:", long_options, NULL)) != -1) {
		switch (c) {
		case 'h':
			show_help(argv[0]);
			return 0;
		case 'V':
			fprintf(stdout, "%s\n"
				"Compiled with libpipewire %s\n"
				"Linked with libpipewire %s\n",
				argv[0],
				pw_get_headers_version(),
				pw_get_library_version());
			return 0;
		case 'a':
			address = optarg;
			fprintf(stdout, "set address %s\n", address);
			break;
		default:
			return -1;
		}
	}

	properties = pw_properties_new(
				PW_KEY_CONTEXT_PROFILE_MODULES, "default,rtkit",
				NULL);

	loop = pw_main_loop_new(&properties->dict);
	if (loop == NULL) {
		pw_log_error("failed to create main-loop: %m");
		return -1;
	}

	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGINT, do_quit, loop);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGTERM, do_quit, loop);

	context = pw_context_new(pw_main_loop_get_loop(loop), properties, 0);
	if (context == NULL) {
		pw_log_error("failed to create context: %m");
		return -1;
	}

	args = spa_aprintf("server.address=\"%s\"", address);
	if (pw_context_load_module(context,
				"libpipewire-module-protocol-pulse",
				args, NULL) == NULL) {
		pw_log_error("failed to create pulse module: %m");
		return -1;
	}

	pw_log_info("start main loop");
	pw_main_loop_run(loop);
	pw_log_info("leave main loop");

	free(args);
	pw_context_destroy(context);
	pw_main_loop_destroy(loop);
	pw_deinit();

	return 0;
}
