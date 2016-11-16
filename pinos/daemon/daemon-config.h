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

#ifdef __cplusplus
extern "C" {
#endif

#include <pinos/server/core.h>

typedef struct _PinosDaemonConfig PinosDaemonConfig;

struct _PinosDaemonConfig {
  SpaList commands;
};

PinosDaemonConfig * pinos_daemon_config_new           (void);
void                pinos_daemon_config_free          (PinosDaemonConfig  *config);
bool                pinos_daemon_config_load_file     (PinosDaemonConfig  *config,
                                                       const char         *filename,
                                                       char              **err);
bool                pinos_daemon_config_load          (PinosDaemonConfig  *config,
                                                       char              **err);
bool                pinos_daemon_config_run_commands  (PinosDaemonConfig  *config,
                                                       PinosCore          *core);
#ifdef __cplusplus
}
#endif


#endif /* __PINOS_DAEMON_CONFIG_H__ */
