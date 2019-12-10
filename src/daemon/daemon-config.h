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

#ifndef PIPEWIRE_DAEMON_CONFIG_H
#define PIPEWIRE_DAEMON_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

#include <pipewire/context.h>

struct pw_daemon_config {
	struct spa_list commands;
	struct pw_properties *properties;
};

struct pw_daemon_config * pw_daemon_config_new(struct pw_properties *properties);

void pw_daemon_config_free(struct pw_daemon_config *config);

int pw_daemon_config_load_file(struct pw_daemon_config *config, const char *filename, char **err);

int pw_daemon_config_load(struct pw_daemon_config *config, char **err);

int pw_daemon_config_run_commands(struct pw_daemon_config *config, struct pw_context *context);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_DAEMON_CONFIG_H */
