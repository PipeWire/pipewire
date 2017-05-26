/* PipeWire
 * Copyright (C) 2016 Axis Communications <dev-gstreamer@axis.com>
 * @author Linus Svensson <linus.svensson@axis.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <errno.h>

#include <pipewire/client/pipewire.h>
#include <pipewire/server/command.h>

#include "pipewire/daemon/daemon-config.h"

#define DEFAULT_CONFIG_FILE PIPEWIRE_CONFIG_DIR "/pipewire.conf"

static bool
parse_line(struct pw_daemon_config *config,
	   const char *filename, char *line, unsigned int lineno, char **err)
{
	struct pw_command *command = NULL;
	char *p;
	bool ret = true;
	char *local_err = NULL;

	/* search for comments */
	if ((p = strchr(line, '#')))
		*p = '\0';

	/* remove whitespaces */
	pw_strip(line, "\n\r \t");

	if (*line == '\0')	/* empty line */
		return true;

	if ((command = pw_command_parse(line, &local_err)) == NULL) {
		asprintf(err, "%s:%u: %s", filename, lineno, local_err);
		free(local_err);
		ret = false;
	} else {
		spa_list_insert(config->commands.prev, &command->link);
	}

	return ret;
}

/**
 * pw_daemon_config_new:
 *
 * Returns a new empty #struct pw_daemon_config.
 */
struct pw_daemon_config *pw_daemon_config_new(void)
{
	struct pw_daemon_config *config;

	config = calloc(1, sizeof(struct pw_daemon_config));
	spa_list_init(&config->commands);

	return config;
}

/**
 * pw_daemon_config_free:
 * @config: A #struct pw_daemon_config
 *
 * Free all resources associated to @config.
 */
void pw_daemon_config_free(struct pw_daemon_config *config)
{
	struct pw_command *cmd, *tmp;

	spa_list_for_each_safe(cmd, tmp, &config->commands, link)
	    pw_command_free(cmd);

	free(config);
}

/**
 * pw_daemon_config_load_file:
 * @config: A #struct pw_daemon_config
 * @filename: A filename
 * @err: Return location for an error string
 *
 * Loads PipeWire config from @filename.
 *
 * Returns: %true on success, otherwise %false and @err is set.
 */
bool pw_daemon_config_load_file(struct pw_daemon_config *config, const char *filename, char **err)
{
	unsigned int line;
	FILE *f;
	char buf[4096];

	pw_log_debug("deamon-config %p: loading configuration file '%s'", config, filename);

	if ((f = fopen(filename, "r")) == NULL) {
		asprintf(err, "failed to open configuration file '%s': %s", filename,
			 strerror(errno));
		goto open_error;
	}

	line = 0;

	while (!feof(f)) {
		if (!fgets(buf, sizeof(buf), f)) {
			if (feof(f))
				break;

			asprintf(err, "failed to read configuration file '%s': %s",
				 filename, strerror(errno));
			goto read_error;
		}

		line++;

		if (!parse_line(config, filename, buf, line, err))
			goto parse_failed;
	}
	fclose(f);

	return true;

      parse_failed:
      read_error:
	fclose(f);
      open_error:
	return false;
}

/**
 * pw_daemon_config_load:
 * @config: A #struct pw_daemon_config
 * @err: Return location for a #GError, or %NULL
 *
 * Loads the default config file for PipeWire. The filename can be overridden with
 * an evironment variable PIPEWIRE_CONFIG_FILE.
 *
 * Return: %true on success, otherwise %false and @err is set.
 */
bool pw_daemon_config_load(struct pw_daemon_config * config, char **err)
{
	const char *filename;

	filename = getenv("PIPEWIRE_CONFIG_FILE");
	if (filename != NULL && *filename != '\0') {
		pw_log_debug("PIPEWIRE_CONFIG_FILE set to: %s", filename);
	} else {
		filename = DEFAULT_CONFIG_FILE;
	}
	return pw_daemon_config_load_file(config, filename, err);
}

/**
 * pw_daemon_config_run_commands:
 * @config: A #struct pw_daemon_config
 * @core: A #struct pw_core
 *
 * Run all commands that have been parsed. The list of commands will be cleared
 * when this function has been called.
 *
 * Returns: %true if all commands where executed with success, otherwise %false.
 */
bool pw_daemon_config_run_commands(struct pw_daemon_config * config, struct pw_core * core)
{
	char *err = NULL;
	bool ret = true;
	struct pw_command *command, *tmp;

	spa_list_for_each(command, &config->commands, link) {
		if (!pw_command_run(command, core, &err)) {
			pw_log_warn("could not run command %s: %s", command->name, err);
			free(err);
			ret = false;
		}
	}

	spa_list_for_each_safe(command, tmp, &config->commands, link) {
		pw_command_free(command);
	}

	return ret;
}
