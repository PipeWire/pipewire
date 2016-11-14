/* Pinos
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

#include <pinos/client/pinos.h>
#include <pinos/client/utils.h>
#include <pinos/server/module.h>

#include "command.h"

typedef bool (*PinosCommandFunc)                    (PinosCommand  *command,
                                                         PinosCore     *core,
                                                         char         **err);

static bool  execute_command_module_load            (PinosCommand  *command,
                                                         PinosCore     *core,
                                                         char         **err);

typedef PinosCommand * (*PinosCommandParseFunc)         (const gchar   *line,
                                                         char         **err);

static PinosCommand *  parse_command_module_load        (const gchar  *line,
                                                         char         **err);

typedef struct
{
  PinosCommand this;

  PinosCommandFunc func;
  char **args;
  int    n_args;
} PinosCommandImpl;

typedef struct _CommandParse
{
  const gchar *name;
  PinosCommandParseFunc func;
} CommandParse;

static const CommandParse parsers[] = {
  {"load-module", parse_command_module_load},
  {NULL, NULL}
};

static const gchar whitespace[] = " \t";

static PinosCommand *
parse_command_module_load (const char * line, char ** err)
{
  PinosCommandImpl *impl;

  impl = calloc (1, sizeof (PinosCommandImpl));

  impl->func = execute_command_module_load;
  impl->args = pinos_split_strv (line, whitespace, 3, &impl->n_args);

  if (impl->args[1] == NULL)
    goto no_module;

  impl->this.name = impl->args[0];

  return &impl->this;

no_module:
  asprintf (err, "%s requires a module name", impl->args[0]);
  pinos_free_strv (impl->args);
  return NULL;
}

static bool
execute_command_module_load (PinosCommand  *command,
                             PinosCore     *core,
                             char         **err)
{
  PinosCommandImpl *impl = SPA_CONTAINER_OF (command, PinosCommandImpl, this);

  return pinos_module_load (core,
                            impl->args[1],
                            impl->args[2],
                            err) != NULL;
}

/**
 * pinos_command_free:
 * @command: A #PinosCommand
 *
 * Free all resources assicated with @command.
 */
void
pinos_command_free (PinosCommand * command)
{
  PinosCommandImpl *impl = SPA_CONTAINER_OF (command, PinosCommandImpl, this);

  spa_list_remove (&command->link);
  pinos_free_strv (impl->args);
  free (impl);
}

/**
 * pinos_command_parse:
 * @line: command line to parse
 * @err: Return location for an error
 *
 * Parses a command line, @line, and return the parsed command.
 * A command can later be executed with pinos_command_run().
 *
 * Returns: The command or %NULL when @err is set.
 */
PinosCommand *
pinos_command_parse (const char     *line,
                     char         **err)
{
  PinosCommand *command = NULL;
  const CommandParse *parse;
  gchar *name;
  gsize len;

  len = strcspn (line, whitespace);

  name = strndup (line, len);

  for (parse = parsers; parse->name != NULL; parse++) {
    if (strcmp (name, parse->name) == 0) {
      command = parse->func (line, err);
      goto out;
    }
  }

  asprintf (err, "Command \"%s\" does not exist", name);
out:
  free (name);
  return command;
}

/**
 * pinos_command_run:
 * @command: A #PinosCommand
 * @core: A #PinosCore
 * @err: Return location for a #GError, or %NULL
 *
 * Run @command.
 *
 * Returns: %true if @command was executed successfully, %false otherwise.
 */
bool
pinos_command_run (PinosCommand  *command,
                   PinosCore     *core,
                   char         **err)
{
  PinosCommandImpl *impl = SPA_CONTAINER_OF (command, PinosCommandImpl, this);

  return impl->func (command, core, err);
}

/**
 * pinos_command_get_name:
 * @command: A #PinosCommand
 *
 * Get the name of @command.
 *
 * Returns: The name of @command.
 */
const char *
pinos_command_get_name (PinosCommand * command)
{
  PinosCommandImpl *impl = SPA_CONTAINER_OF (command, PinosCommandImpl, this);

  return impl->args[0];
}
