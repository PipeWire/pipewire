/* PipeWire
 * Copyright © 2016 Axis Communications <dev-gstreamer@axis.com>
 *	@author Linus Svensson <linus.svensson@axis.com>
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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <pipewire/pipewire.h>

#include "daemon/command.h"
#include "daemon/daemon-config.h"

#define DEFAULT_CONFIG_FILE PIPEWIRE_CONFIG_DIR "/pipewire.conf"

static int
parse_line(struct pw_daemon_config *config,
	   const char *filename, char *line, unsigned int lineno, char **err)
{
	struct pw_command *command = NULL;
	char *p;
	char *local_err = NULL;

	/* search for comments */
	if ((p = strchr(line, '#')))
		*p = '\0';

	/* remove whitespaces */
	line = pw_strip(line, "\n\r \t");
	if (*line == '\0')	/* empty line */
		goto out;

	if ((command = pw_command_parse(config->properties, line, &local_err)) == NULL)
		goto error_parse;

	spa_list_append(&config->commands, &command->link);

out:
	return 0;

error_parse:
	*err = spa_aprintf("%s:%u: %s", filename, lineno, local_err);
	free(local_err);
	return -EINVAL;
}

/**
 * pw_daemon_config_new:
 *
 * Returns a new empty #struct pw_daemon_config.
 */
struct pw_daemon_config *pw_daemon_config_new(struct pw_properties *properties)
{
	struct pw_daemon_config *config;

	config = calloc(1, sizeof(struct pw_daemon_config));
	if (config == NULL)
		goto error_exit;

	config->properties = properties;
	spa_list_init(&config->commands);

	return config;

error_exit:
	return NULL;
}

/**
 * pw_daemon_config_free:
 * @config: A #struct pw_daemon_config
 *
 * Free all resources associated to @config.
 */
void pw_daemon_config_free(struct pw_daemon_config *config)
{
	struct pw_command *cmd;

	spa_list_consume(cmd, &config->commands, link)
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
 * Returns: 0 on success, otherwise < 0 and @err is set.
 */
int pw_daemon_config_load_file(struct pw_daemon_config *config, const char *filename, char **err)
{
	unsigned int line;
	FILE *f;
	char buf[4096];

	pw_log_debug("deamon-config %p: loading configuration file '%s'", config, filename);

	if ((f = fopen(filename, "r")) == NULL) {
		*err = spa_aprintf("failed to open configuration file '%s': %s", filename,
			 strerror(errno));
		goto open_error;
	}

	line = 0;

	while (!feof(f)) {
		if (!fgets(buf, sizeof(buf), f)) {
			if (feof(f))
				break;
			*err = spa_aprintf("failed to read configuration file '%s': %s",
				 filename, strerror(errno));
			goto read_error;
		}

		line++;

		if (parse_line(config, filename, buf, line, err) != 0)
			goto parse_failed;
	}
	fclose(f);

	return 0;

      parse_failed:
      read_error:
	fclose(f);
      open_error:
	return -EINVAL;
}

/**
 * pw_daemon_config_load:
 * @config: A #struct pw_daemon_config
 * @err: Return location for a #GError, or %NULL
 *
 * Loads the default config file for PipeWire. The filename can be overridden with
 * an evironment variable PIPEWIRE_CONFIG_FILE.
 *
 * Return: 0 on success, otherwise < 0 and @err is set.
 */
int pw_daemon_config_load(struct pw_daemon_config *config, char **err)
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
 * @context: A #struct pw_context
 *
 * Run all commands that have been parsed. The list of commands will be cleared
 * when this function has been called.
 *
 * Returns: 0 if all commands where executed with success, otherwise < 0.
 */
int pw_daemon_config_run_commands(struct pw_daemon_config *config, struct pw_context *context)
{
	char *err = NULL;
	int ret = 0;
	struct pw_command *command;

	spa_list_for_each(command, &config->commands, link) {
		if ((ret = pw_command_run(command, context, &err)) < 0) {
			pw_log_error("could not run command %s: %s", command->args[0], err);
			free(err);
			break;
		}
	}

	spa_list_consume(command, &config->commands, link)
		pw_command_free(command);

	return ret;
}
