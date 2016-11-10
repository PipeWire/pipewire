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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <string.h>

#include <pinos/client/pinos.h>
#include <pinos/server/command.h>

#include "pinos/daemon/daemon-config.h"

#define DEFAULT_CONFIG_FILE PINOS_CONFIG_DIR G_DIR_SEPARATOR_S "pinos.conf"

GQuark
pinos_daemon_config_error_quark (void)
{
  static GQuark quark = 0;

  if (quark == 0) {
    quark = g_quark_from_static_string ("pinos_daemon_config_error");
  }

  return quark;
}

static gboolean
parse_line (PinosDaemonConfig  * config,
            const gchar        * filename,
            gchar              * line,
            guint                lineno,
            GError            ** err)
{
  PinosCommand *command = NULL;
  gchar *p;
  gboolean ret = TRUE;
  GError *local_err = NULL;

  /* search for comments */
  if ((p = strchr (line, '#')))
    *p = '\0';

  /* remove whitespaces */
  g_strstrip (line);

  if (*line == '\0') /* empty line */
    return TRUE;

  if (!pinos_command_parse (&command, line, &local_err)) {
    g_set_error (err, PINOS_DAEMON_CONFIG_ERROR,
        PINOS_DAEMON_CONFIG_ERROR_COMMAND, "%s:%u: %s", filename, lineno,
        local_err->message);
    g_error_free (local_err);
    ret = FALSE;
  } else {
    config->commands = g_list_append (config->commands, command);
  }

  return ret;
}

/**
 * pinos_daemon_config_new:
 *
 * Returns a new empty #PinosDaemonConfig.
 */
PinosDaemonConfig *
pinos_daemon_config_new (void)
{
  PinosDaemonConfig *config;

  config = g_new (PinosDaemonConfig, 1);

  config->commands = NULL;

  return config;
}

/**
 * pinos_daemon_config_free:
 * @config: A #PinosDaemonConfig
 *
 * Free all resources associated to @config.
 */
void
pinos_daemon_config_free (PinosDaemonConfig * config)
{
  g_return_if_fail (config != NULL);

  g_list_free_full (config->commands, (GDestroyNotify) pinos_command_free);

  g_free (config);
}

/**
 * pinos_daemon_config_load_file:
 * @config: A #PinosDaemonConfig
 * @filename: A filename
 * @err: Return location for a #GError, or %NULL
 *
 * Loads pinos config from @filename.
 *
 * Returns: %TRUE on success, otherwise %FALSE and @err is set.
 */
gboolean
pinos_daemon_config_load_file (PinosDaemonConfig  * config,
                               const gchar        * filename,
                               GError            ** err)
{
  gchar *data;
  gchar **lines;
  gboolean ret = TRUE;
  guint i;

  g_return_val_if_fail (config != NULL, FALSE);
  g_return_val_if_fail (filename != NULL && *filename != '\0', FALSE);

  pinos_log_debug ("deamon-config %p loading file %s", config, filename);

  if (!g_file_get_contents (filename, &data, NULL, err)) {
    return FALSE;
  }

  lines = g_strsplit (data, "\n", 0);
  for (i = 0; lines[i] != NULL; i++) {
    if (!parse_line (config, filename, lines[i], i+1, err)) {
      ret = FALSE;
      break;
    }
  }

  g_strfreev (lines);
  g_free (data);

  return ret;
}

/**
 * pinos_daemon_config_load:
 * @config: A #PinosDaemonConfig
 * @err: Return location for a #GError, or %NULL
 *
 * Loads the default config file for pinos. The filename can be overridden with
 * an evironment variable PINOS_CONFIG_FILE.
 *
 * Return: %TRUE on success, otherwise %FALSE and @err is set.
 */
gboolean
pinos_daemon_config_load (PinosDaemonConfig  * config,
                          GError            ** err)
{
  const gchar *filename;

  g_return_val_if_fail (config != NULL, FALSE);

  filename = g_getenv ("PINOS_CONFIG_FILE");
  if (filename != NULL && *filename != '\0') {
    pinos_log_debug ("PINOS_CONFIG_FILE set to: %s", filename);
  } else {
    filename = DEFAULT_CONFIG_FILE;
  }

  return pinos_daemon_config_load_file (config, filename, err);
}

/**
 * pinos_daemon_config_run_commands:
 * @config: A #PinosDaemonConfig
 * @daemon: A #PinosDaemon
 *
 * Run all commands that have been parsed. The list of commands will be cleared
 * when this function has been called.
 *
 * Returns: %TRUE if all commands where executed with success, otherwise %FALSE.
 */
gboolean
pinos_daemon_config_run_commands (PinosDaemonConfig  * config,
                                  PinosDaemon        * daemon)
{
  GList *walk;
  GError *err = NULL;
  gboolean ret = TRUE;

  g_return_val_if_fail (config != NULL, FALSE);

  for (walk = config->commands; walk != NULL; walk = walk->next) {
    PinosCommand *command = (PinosCommand *)walk->data;
    if (!pinos_command_run (command, daemon->core, &err)) {
      pinos_log_warn ("could not run command %s: %s",
          pinos_command_get_name (command), err->message);
      g_clear_error (&err);
      ret = FALSE;
    }
  }

  g_list_free_full (config->commands, (GDestroyNotify) pinos_command_free);

  return ret;
}
