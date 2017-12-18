/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <signal.h>

#include <pipewire/pipewire.h>
#include <pipewire/core.h>
#include <pipewire/module.h>

#include "daemon-config.h"

static void do_quit(void *data, int signal_number)
{
	struct pw_main_loop *loop = data;
	pw_main_loop_quit(loop);
}

int main(int argc, char *argv[])
{
	struct pw_core *core;
	struct pw_main_loop *loop;
	struct pw_daemon_config *config;
	char *err = NULL;
	struct pw_properties *props;

	pw_init(&argc, &argv);

	/* parse configuration */
	config = pw_daemon_config_new();
	if (pw_daemon_config_load(config, &err) < 0) {
		pw_log_error("failed to parse config: %s", err);
		free(err);
		return -1;
	}

	props = pw_properties_new(PW_CORE_PROP_NAME, "pipewire-0",
				  PW_CORE_PROP_DAEMON, "1", NULL);

	loop = pw_main_loop_new(props);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGINT, do_quit, loop);
	pw_loop_add_signal(pw_main_loop_get_loop(loop), SIGTERM, do_quit, loop);

	core = pw_core_new(pw_main_loop_get_loop(loop), props);

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
