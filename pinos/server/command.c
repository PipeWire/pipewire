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

#include <glib.h>
#include <string.h>

#include <pinos/client/pinos.h>
#include <pinos/server/module.h>

#include "command.h"

GQuark
pinos_command_error_quark (void)
{
  static GQuark quark = 0;

  if (quark == 0) {
    quark = g_quark_from_static_string ("pinos_command_error");
  }

  return quark;
}

typedef gboolean (*PinosCommandFunc)                    (PinosCommand  *command,
                                                         PinosCore     *core,
                                                         GError       **err);

static gboolean  execute_command_module_load            (PinosCommand  *command,
                                                         PinosCore     *core,
                                                         GError       **err);

typedef PinosCommand * (*PinosCommandParseFunc)         (const gchar   *line,
                                                         GError       **err);

static PinosCommand *  parse_command_module_load        (const gchar  *line,
                                                         GError      **err);

struct _PinosCommand
{
  PinosCommandFunc func;
  gchar **args;
  gint    n_args;
};

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

static gchar **
tokenize (const gchar * line, gint max_tokens, gint * n_tokens)
{
  gchar **res;
  GSList *tokens, *walk;
  const gchar *s;
  gint num;

  s = line + strspn (line, whitespace);
  num = 0;
  tokens = NULL;

  while (*s != '\0' && num + 1 < max_tokens) {
    gsize len;
    gchar *token;

    len = strcspn (s, whitespace);
    token = g_strndup (s, len);
    tokens = g_slist_prepend (tokens, token);
    num++;

    s += len;
    s += strspn (s, whitespace);
  }

  if (*s != '\0') {
    tokens = g_slist_prepend (tokens, g_strdup (s));
    num++;
  }

  res = g_new (gchar *, num + 1);
  res[num] = NULL;
  for (walk = tokens; walk != NULL; walk = walk->next) {
    res[--num] = walk->data;
  }
  g_slist_free (tokens);

  *n_tokens = num;

  return res;
}

static PinosCommand *
parse_command_module_load (const gchar * line, GError ** err)
{
  PinosCommand *command;

  command = g_new0 (PinosCommand, 1);

  command->func = execute_command_module_load;
  command->args = tokenize (line, 3, &command->n_args);

  if (command->args[1] == NULL)
    goto no_module;

  return command;

no_module:
  g_set_error (err, PINOS_COMMAND_ERROR, PINOS_COMMAND_ERROR_PARSE,
      "%s requires a module name", command->args[0]);

  g_strfreev (command->args);

  return NULL;
}

static gboolean
execute_command_module_load (PinosCommand  *command,
                             PinosCore     *core,
                             GError       **err)
{
  gchar *module;
  gchar *args;

  module = command->args[1];
  args = command->args[2];

  return pinos_module_load (core, module, args, err) != NULL;
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
  g_return_if_fail (command != NULL);

  g_strfreev (command->args);

  g_free (command);
}

/**
 * pinos_command_parse:
 * @command: (out): Return location for a #PinosCommand
 * @line: command line to parse
 * @err: Return location for a #GError, or %NULL
 *
 * Parses a command line, @line, and store the parsed command in @command.
 * A command can later be executed with pinos_command_run().
 *
 * Returns: %TRUE on success, %FALSE otherwise.
 */
gboolean
pinos_command_parse (PinosCommand ** command,
                     gchar         * line,
                     GError       ** err)
{
  const CommandParse *parse;
  gchar *name;
  gsize len;
  gboolean ret = FALSE;

  g_return_val_if_fail (command != NULL && *command == NULL, FALSE);
  g_return_val_if_fail (line != NULL && *line != '\0', FALSE);

  len = strcspn (line, whitespace);

  name = g_strndup (line, len);

  for (parse = parsers; parse->name != NULL; parse++) {
    if (g_strcmp0 (name, parse->name) == 0) {
      *command = parse->func (line, err);
      if (*command != NULL)
        ret = TRUE;
      goto out;
    }
  }

  g_set_error (err, PINOS_COMMAND_ERROR, PINOS_COMMAND_ERROR_NO_SUCH_COMMAND,
      "Command \"%s\" does not exist", name);

out:
  g_free (name);

  return ret;
}

/**
 * pinos_command_run:
 * @command: A #PinosCommand
 * @core: A #PinosCore
 * @err: Return location for a #GError, or %NULL
 *
 * Run @command.
 *
 * Returns: %TRUE if @command was executed successfully, %FALSE otherwise.
 */
gboolean
pinos_command_run (PinosCommand  *command,
                   PinosCore     *core,
                   GError       **err)
{
  g_return_val_if_fail (command != NULL, FALSE);
  g_return_val_if_fail (core, FALSE);

  return command->func (command, core, err);
}

/**
 * pinos_command_get_name:
 * @command: A #PinosCommand
 *
 * Get the name of @command.
 *
 * Returns: The name of @command.
 */
const gchar *
pinos_command_get_name (PinosCommand * command)
{
  g_return_val_if_fail (command != NULL, NULL);

  return command->args[0];
}
