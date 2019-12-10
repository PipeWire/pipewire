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

#ifndef PIPEWIRE_COMMAND_H
#define PIPEWIRE_COMMAND_H

#ifdef __cplusplus
extern "C" {
#endif

#include <pipewire/context.h>

struct pw_command;

typedef int (*pw_command_func_t) (struct pw_command *command, struct pw_context *context, char **err);

/** \class pw_command
 *
 * A configuration command
 */
struct pw_command {
	struct spa_list link;	/**< link in list of commands */
	pw_command_func_t func;
	char **args;
	uint32_t id;		/**< id of command */
	int n_args;
};

struct pw_command *
pw_command_parse(struct pw_properties *properties, const char *line, char **err);

void
pw_command_free(struct pw_command *command);

int pw_command_run(struct pw_command *command, struct pw_context *context, char **err);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_COMMAND_H */
