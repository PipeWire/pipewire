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

#ifndef __PINOS_COMMAND_H__
#define __PINOS_COMMAND_H__

#include <glib.h>

G_BEGIN_DECLS

#include <pinos/server/daemon.h>

typedef struct _PinosCommand PinosCommand;

GQuark pinos_command_error_quark (void);
/**
 * PINOS_COMMAND_ERROR:
 *
 * Pinos command error.
 */
#define PINOS_COMMAND_ERROR (pinos_command_error_quark ())

/**
 * PinosCommandError:
 * @PINOS_COMMAND_ERROR_GENERIC: Generic command error.
 * @PINOS_COMMAND_ERROR_NO_SUCH_COMMAND: No such command.
 * @PINOS_COMMAND_ERROR_PARSE: Failed to parse command.
 * @PINOS_COMMAND_ERROR_FAILED: Command failed to execute.
 *
 * Error codes for Pinos command.
 */
typedef enum
{
  PINOS_COMMAND_ERROR_GENERIC,
  PINOS_COMMAND_ERROR_NO_SUCH_COMMAND,
  PINOS_COMMAND_ERROR_PARSE,
  PINOS_COMMAND_ERROR_FAILED,
} PinosCommandError;

void               pinos_command_free                 (PinosCommand *command);

gboolean           pinos_command_parse                (PinosCommand **command,
                                                       gchar         *line,
                                                       GError       **err);
gboolean           pinos_command_run                  (PinosCommand  *command,
                                                       PinosDaemon   *daemon,
                                                       GError       **err);
const gchar *      pinos_command_get_name             (PinosCommand  *command);

G_END_DECLS

#endif /* __PINOS_COMMAND_H__ */
