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

#ifndef __PIPEWIRE_COMMAND_H__
#define __PIPEWIRE_COMMAND_H__

#ifdef __cplusplus
extern "C" {
#endif

/** \class pw_command
 *
 * A configuration command
 */
struct pw_command;

#include <pipewire/core.h>


struct pw_command *
pw_command_parse(const char *line, char **err);

void
pw_command_free(struct pw_command *command);

int pw_command_run(struct pw_command *command, struct pw_core *core, char **err);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_COMMAND_H__ */
