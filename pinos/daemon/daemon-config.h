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

#ifndef __PINOS_DAEMON_CONFIG_H__
#define __PINOS_DAEMON_CONFIG_H__

#include <glib-object.h>

G_BEGIN_DECLS

#include <server/daemon.h>

#define PINOS_TYPE_DAEMON_CONFIG           (pinos_daemon_config_get_type ())

typedef struct _PinosDaemonConfig PinosDaemonConfig;

struct _PinosDaemonConfig {
  GList *commands;
};

GQuark pinos_daemon_config_error_quark (void);
/**
 * PINOS_DAEMON_CONFIG_ERROR:
 *
 * Pinos daemon config error.
 */
#define PINOS_DAEMON_CONFIG_ERROR (pinos_daemon_config_error_quark ())

/**
 * PinosDaemonConfigError:
 * @PINOS_DAEMON_CONFIG_ERROR_GENERIC: Generic daemon config error.
 * @PINOS_DAEMON_CONFIG_ERROR_ASSIGNMENT: Assignment error.
 * @PINOS_DAEMON_CONFIG_ERROR_COMMAND: Command error.
 *
 * Error codes for Pinos daemon config.
 */
typedef enum
{
  PINOS_DAEMON_CONFIG_ERROR_GENERIC,
  PINOS_DAEMON_CONFIG_ERROR_ASSIGNMENT,
  PINOS_DAEMON_CONFIG_ERROR_COMMAND,
} PinosDaemonConfigError;

GType               pinos_daemon_config_get_type      (void);

PinosDaemonConfig * pinos_daemon_config_new           (void);
void                pinos_daemon_config_free          (PinosDaemonConfig  *config);
gboolean            pinos_daemon_config_load_file     (PinosDaemonConfig  *config,
                                                       const gchar        *filename,
                                                       GError            **err);
gboolean            pinos_daemon_config_load          (PinosDaemonConfig  *config,
                                                       GError            **err);
gboolean            pinos_daemon_config_run_commands  (PinosDaemonConfig  *config,
                                                       PinosDaemon        *daemon);

G_END_DECLS

#endif /* __PINOS_DAEMON_CONFIG_H__ */
