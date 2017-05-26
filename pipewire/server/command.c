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

#include <string.h>

#include <pipewire/client/pipewire.h>
#include <pipewire/client/utils.h>
#include <pipewire/server/module.h>

#include "command.h"

typedef bool(*pw_command_func_t) (struct pw_command *command, struct pw_core *core, char **err);

static bool execute_command_module_load(struct pw_command *command,
					struct pw_core *core, char **err);

typedef struct pw_command *(*pw_command_parse_func_t) (const char *line, char **err);

static struct pw_command *parse_command_module_load(const char *line, char **err);

struct impl {
	struct pw_command this;

	pw_command_func_t func;
	char **args;
	int n_args;
};

struct command_parse {
	const char *name;
	pw_command_parse_func_t func;
};

static const struct command_parse parsers[] = {
	{"load-module", parse_command_module_load},
	{NULL, NULL}
};

static const char whitespace[] = " \t";

static struct pw_command *parse_command_module_load(const char *line, char **err)
{
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto no_mem;

	impl->func = execute_command_module_load;
	impl->args = pw_split_strv(line, whitespace, 3, &impl->n_args);

	if (impl->args[1] == NULL)
		goto no_module;

	impl->this.name = impl->args[0];

	return &impl->this;

      no_module:
	asprintf(err, "%s requires a module name", impl->args[0]);
	pw_free_strv(impl->args);
	return NULL;
      no_mem:
	asprintf(err, "no memory");
	return NULL;
}

static bool
execute_command_module_load(struct pw_command *command, struct pw_core *core, char **err)
{
	struct impl *impl = SPA_CONTAINER_OF(command, struct impl, this);

	return pw_module_load(core, impl->args[1], impl->args[2], err) != NULL;
}

/**
 * pw_command_free:
 * @command: A #struct pw_command
 *
 * Free all resources assicated with @command.
 */
void pw_command_free(struct pw_command *command)
{
	struct impl *impl = SPA_CONTAINER_OF(command, struct impl, this);

	spa_list_remove(&command->link);
	pw_free_strv(impl->args);
	free(impl);
}

/**
 * pw_command_parse:
 * @line: command line to parse
 * @err: Return location for an error
 *
 * Parses a command line, @line, and return the parsed command.
 * A command can later be executed with pw_command_run().
 *
 * Returns: The command or %NULL when @err is set.
 */
struct pw_command *pw_command_parse(const char *line, char **err)
{
	struct pw_command *command = NULL;
	const struct command_parse *parse;
	char *name;
	size_t len;

	len = strcspn(line, whitespace);

	name = strndup(line, len);

	for (parse = parsers; parse->name != NULL; parse++) {
		if (strcmp(name, parse->name) == 0) {
			command = parse->func(line, err);
			goto out;
		}
	}

	asprintf(err, "Command \"%s\" does not exist", name);
      out:
	free(name);
	return command;
}

/**
 * pw_command_run:
 * @command: A #struct pw_comman
 * @core: A #struct pw_core
 * @err: Return location for a #GError, or %NULL
 *
 * Run @command.
 *
 * Returns: %true if @command was executed successfully, %false otherwise.
 */
bool pw_command_run(struct pw_command *command, struct pw_core *core, char **err)
{
	struct impl *impl = SPA_CONTAINER_OF(command, struct impl, this);

	return impl->func(command, core, err);
}

/**
 * pw_command_get_name:
 * @command: A #struct pw_command
 *
 * Get the name of @command.
 *
 * Returns: The name of @command.
 */
const char *pw_command_get_name(struct pw_command *command)
{
	struct impl *impl = SPA_CONTAINER_OF(command, struct impl, this);

	return impl->args[0];
}
