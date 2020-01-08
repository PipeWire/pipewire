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

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <sys/wait.h>

#include <pipewire/impl.h>

#include "command.h"

/** \cond */

static struct pw_command *parse_command_help(struct pw_properties *properties, const char *line, char **err);
static struct pw_command *parse_command_set_prop(struct pw_properties *properties, const char *line, char **err);
static struct pw_command *parse_command_add_spa_lib(struct pw_properties *properties, const char *line, char **err);
static struct pw_command *parse_command_module_load(struct pw_properties *properties, const char *line, char **err);
static struct pw_command *parse_command_exec(struct pw_properties *properties, const char *line, char **err);

struct impl {
	struct pw_command this;
};

typedef struct pw_command *(*pw_command_parse_func_t) (struct pw_properties *properties, const char *line, char **err);

struct command_parse {
	const char *name;
	const char *description;
	pw_command_parse_func_t func;
};

static const struct command_parse parsers[] = {
	{"help", "Show this help", parse_command_help},
	{"set-prop", "Set a property", parse_command_set_prop},
	{"add-spa-lib", "Add a library that provides a spa factory name regex", parse_command_add_spa_lib},
	{"load-module", "Load a module", parse_command_module_load},
	{"exec", "Execute a program", parse_command_exec},
	{NULL, NULL, NULL }
};

static const char whitespace[] = " \t";
/** \endcond */

static int
execute_command_help(struct pw_command *command, struct pw_context *context, char **err)
{
	int i;

	fputs("Available commands:\n", stdout);
	for (i = 0; parsers[i].name; i++)
		fprintf(stdout, "    %20.20s\t%s\n", parsers[i].name, parsers[i].description);

	return 0;
}

static struct pw_command *parse_command_help(struct pw_properties *properties, const char *line, char **err)
{
	struct impl *impl;
	struct pw_command *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto no_mem;

	this = &impl->this;
	this->func = execute_command_help;
	this->args = pw_split_strv(line, whitespace, 1, &this->n_args);

	return this;

no_mem:
	asprintf(err, "alloc failed: %m");
	return NULL;
}

static int
execute_command_set_prop(struct pw_command *command, struct pw_context *context, char **err)
{
	return 0;
}

static struct pw_command *parse_command_set_prop(struct pw_properties *properties, const char *line, char **err)
{
	struct impl *impl;
	struct pw_command *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto error_alloc;

	this = &impl->this;
	this->func = execute_command_set_prop;
	this->args = pw_split_strv(line, whitespace, 4, &this->n_args);

	if (this->n_args < 3)
		goto error_arguments;

	pw_log_debug("set property: '%s' = '%s'", this->args[1], this->args[2]);
	pw_properties_set(properties, this->args[1], this->args[2]);

	return this;

error_arguments:
	asprintf(err, "%s requires <property-name> <value>", this->args[0]);
	pw_free_strv(this->args);
	return NULL;
error_alloc:
	asprintf(err, "alloc failed: %m");
	return NULL;
}

static int
execute_command_add_spa_lib(struct pw_command *command, struct pw_context *context, char **err)
{
	int res;

	res = pw_context_add_spa_lib(context, command->args[1], command->args[2]);
	if (res < 0) {
		asprintf(err, "could not add spa library \"%s\"", command->args[1]);
		return res;
	}
	return 0;
}

static struct pw_command *parse_command_add_spa_lib(struct pw_properties *properties, const char *line, char **err)
{
	struct impl *impl;
	struct pw_command *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto no_mem;

	this = &impl->this;
	this->func = execute_command_add_spa_lib;
	this->args = pw_split_strv(line, whitespace, 4, &this->n_args);

	if (this->n_args < 3)
		goto no_library;

	return this;

no_library:
	asprintf(err, "%s requires <factory-regex> <library-name>", this->args[0]);
	pw_free_strv(this->args);
	return NULL;
no_mem:
	asprintf(err, "alloc failed: %m");
	return NULL;
}

static int
execute_command_module_load(struct pw_command *command, struct pw_context *context, char **err)
{
	struct pw_impl_module *module;

	module = pw_context_load_module(context, command->args[1], command->args[2], NULL);
	if (module == NULL) {
		asprintf(err, "could not load module \"%s\": %m", command->args[1]);
		return -errno;
	}
	return 0;
}

static struct pw_command *parse_command_module_load(struct pw_properties *properties, const char *line, char **err)
{
	struct impl *impl;
	struct pw_command *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto no_mem;

	this = &impl->this;
	this->func = execute_command_module_load;
	this->args = pw_split_strv(line, whitespace, 3, &this->n_args);

	if (this->n_args < 2)
		goto no_module;

	return this;

no_module:
	asprintf(err, "%s requires a module name", this->args[0]);
	pw_free_strv(this->args);
	return NULL;
no_mem:
	asprintf(err, "alloc failed: %m");
	return NULL;
}

static int
execute_command_exec(struct pw_command *command, struct pw_context *context, char **err)
{
	int pid, res;

	pid = fork();

	if (pid == 0) {
		pw_log_info("exec %s", command->args[1]);
		res = execv(command->args[1], command->args);
		if (res == -1) {
			res = -errno;
			asprintf(err, "'%s': %m", command->args[1]);
			return res;
		}
	}
	else {
		int status;
		res = waitpid(pid, &status, WNOHANG);
		pw_log_info("exec got pid %d res:%d status:%d", pid, res, status);
	}
	return 0;
}

static struct pw_command *parse_command_exec(struct pw_properties *properties, const char *line, char **err)
{
	struct impl *impl;
	struct pw_command *this;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto no_mem;

	this = &impl->this;
	this->func = execute_command_exec;
	this->args = pw_split_strv(line, whitespace, INT_MAX, &this->n_args);

	if (this->n_args < 1)
		goto no_executable;

	return this;

no_executable:
	asprintf(err, "requires an executable name");
	pw_free_strv(this->args);
	return NULL;
no_mem:
	asprintf(err, "alloc failed: %m");
	return NULL;
}

/** Free command
 *
 * \param command a command to free
 *
 * Free all resources assicated with \a command.
 *
 * \memberof pw_command
 */
SPA_EXPORT
void pw_command_free(struct pw_command *command)
{
	struct impl *impl = SPA_CONTAINER_OF(command, struct impl, this);

	spa_list_remove(&command->link);
	pw_free_strv(command->args);
	free(impl);
}

/** Parses a command line
 * \param line command line to parse
 * \param[out] err Return location for an error
 * \return The command or NULL when \a err is set.
 *
 * Parses a command line, \a line, and return the parsed command.
 * A command can later be executed with \ref pw_command_run()
 *
 * \memberof pw_command
 */
SPA_EXPORT
struct pw_command *pw_command_parse(struct pw_properties *properties, const char *line, char **err)
{
	struct pw_command *command = NULL;
	const struct command_parse *parse;
	char *name;
	size_t len;

	len = strcspn(line, whitespace);

	name = strndup(line, len);

	for (parse = parsers; parse->name != NULL; parse++) {
		if (strcmp(name, parse->name) == 0) {
			command = parse->func(properties, line, err);
			goto out;
		}
	}

	asprintf(err, "Command \"%s\" does not exist", name);
out:
	free(name);
	return command;
}

/** Run a command
 *
 * \param command A \ref pw_command
 * \param context A \ref pw_context
 * \param err Return location for an error string, or NULL
 * \return 0 on success, < 0 on error
 *
 * \memberof pw_command
 */
SPA_EXPORT
int pw_command_run(struct pw_command *command, struct pw_context *context, char **err)
{
	return command->func(command, context, err);
}
