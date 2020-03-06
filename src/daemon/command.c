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
static struct pw_command *parse_command_create_object(struct pw_properties *properties, const char *line, char **err);
static struct pw_command *parse_command_exec(struct pw_properties *properties, const char *line, char **err);

struct impl {
	struct pw_command this;
	int first_arg;
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
	{"create-object", "Create an object from a factory", parse_command_create_object},
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
	*err = spa_aprintf("alloc failed: %m");
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
	*err = spa_aprintf("%s requires <property-name> <value>", this->args[0]);
	pw_free_strv(this->args);
	free(impl);
	return NULL;
error_alloc:
	*err = spa_aprintf("alloc failed: %m");
	return NULL;
}

static int
execute_command_add_spa_lib(struct pw_command *command, struct pw_context *context, char **err)
{
	int res;

	res = pw_context_add_spa_lib(context, command->args[1], command->args[2]);
	if (res < 0) {
		*err = spa_aprintf("could not add spa library \"%s\"", command->args[1]);
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
	*err = spa_aprintf("%s requires <factory-regex> <library-name>", this->args[0]);
	pw_free_strv(this->args);
	free(impl);
	return NULL;
no_mem:
	*err = spa_aprintf("alloc failed: %m");
	return NULL;
}

static bool has_option(struct pw_command *this, int first_arg, const char *option)
{
	int arg;
	for (arg = 1; arg < first_arg; arg++) {
		if (strstr(this->args[arg], "-") == this->args[arg]) {
			if (strcmp(this->args[arg], option) == 0)
				return true;
		}
	}
	return false;
}

static int
execute_command_module_load(struct pw_command *command, struct pw_context *context, char **err)
{
	struct pw_impl_module *module;
	struct impl *impl = SPA_CONTAINER_OF(command, struct impl, this);
	int arg = impl->first_arg;

	module = pw_context_load_module(context, command->args[arg], command->args[arg+1], NULL);
	if (module == NULL) {
		if (errno == ENOENT && has_option(command, arg, "-ifexists")) {
			pw_log_debug("skipping unavailable module %s", command->args[arg]);
			return 0;
		}
		*err = spa_aprintf("could not load module \"%s\": %m", command->args[arg]);
		return -errno;
	}
	return 0;
}

static struct pw_command *parse_command_module_load(struct pw_properties *properties, const char *line, char **err)
{
	struct impl *impl;
	struct pw_command *this;
	int arg;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto no_mem;

	this = &impl->this;
	this->func = execute_command_module_load;

	this->args = pw_split_strv(line, whitespace, INT_MAX, &this->n_args);

	for (arg = 1; arg < this->n_args; arg++) {
		if (strstr(this->args[arg], "-") != this->args[arg])
			break;
	}
	if (arg + 1 > this->n_args)
		goto no_module;

	pw_free_strv(this->args);
	this->args = pw_split_strv(line, whitespace, arg + 2, &this->n_args);

	impl->first_arg = arg;

	return this;

no_module:
	*err = spa_aprintf("%s requires a module name", this->args[0]);
	pw_free_strv(this->args);
	free(impl);
	return NULL;
no_mem:
	*err = spa_aprintf("alloc failed: %m");
	return NULL;
}

static int
execute_command_create_object(struct pw_command *command, struct pw_context *context, char **err)
{
	struct pw_impl_factory *factory;
	struct impl *impl = SPA_CONTAINER_OF(command, struct impl, this);
	int arg = impl->first_arg;
	void *obj;

	pw_log_debug("find factory %s", command->args[arg]);
	factory = pw_context_find_factory(context, command->args[arg]);
	if (factory == NULL) {
		if (has_option(command, arg, "-nofail"))
			return 0;
		pw_log_error("can't find factory %s", command->args[arg]);
		return -ENOENT;
	}

	pw_log_debug("create object with args %s", command->args[arg+1]);
	obj = pw_impl_factory_create_object(factory,
			NULL, NULL, 0,
			pw_properties_new_string(command->args[arg+1]),
			SPA_ID_INVALID);
	if (obj == NULL) {
		if (has_option(command, arg, "-nofail"))
			return 0;
		pw_log_error("can't create object from factory %s: %m", command->args[arg]);
		return -errno;
	}
	return 0;

}

static struct pw_command *parse_command_create_object(struct pw_properties *properties, const char *line, char **err)
{
	struct impl *impl;
	struct pw_command *this;
	int arg;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto no_mem;

	this = &impl->this;
	this->func = execute_command_create_object;
	this->args = pw_split_strv(line, whitespace, INT_MAX, &this->n_args);

	for (arg = 1; arg < this->n_args; arg++) {
		if (strstr(this->args[arg], "-") != this->args[arg])
			break;
	}
	if (arg > this->n_args)
		goto no_factory;

	pw_free_strv(this->args);
	this->args = pw_split_strv(line, whitespace, arg + 2, &this->n_args);

	impl->first_arg = arg;

	return this;

no_factory:
	*err = spa_aprintf("%s requires <factory-name> [<key>=<value> ...]", this->args[0]);
	pw_free_strv(this->args);
	free(impl);
	return NULL;
no_mem:
	*err = spa_aprintf("alloc failed: %m");
	return NULL;
}

static int
execute_command_exec(struct pw_command *command, struct pw_context *context, char **err)
{
	int pid, res;

	pid = fork();

	if (pid == 0) {
		pw_log_info("exec %s", command->args[1]);
		res = execvp(command->args[1], command->args);
		if (res == -1) {
			res = -errno;
			*err = spa_aprintf("'%s': %m", command->args[1]);
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
	*err = spa_aprintf("requires an executable name");
	pw_free_strv(this->args);
	free(impl);
	return NULL;
no_mem:
	*err = spa_aprintf("alloc failed: %m");
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

	*err = spa_aprintf("Command \"%s\" does not exist", name);
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
