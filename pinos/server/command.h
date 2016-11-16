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

#ifdef __cplusplus
extern "C" {
#endif

#include <pinos/server/core.h>

typedef struct _PinosCommand PinosCommand;


struct _PinosCommand {
  SpaList link;

  const char *name;
};

void               pinos_command_free                 (PinosCommand *command);

PinosCommand *     pinos_command_parse                (const char    *line,
                                                       char         **err);
bool               pinos_command_run                  (PinosCommand  *command,
                                                       PinosCore     *core,
                                                       char         **err);
#ifdef __cplusplus
}
#endif

#endif /* __PINOS_COMMAND_H__ */
