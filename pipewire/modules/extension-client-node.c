/* PipeWire
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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
#include <stdio.h>
#include <errno.h>
#include <dlfcn.h>

#include "config.h"

#include "pipewire/client/interfaces.h"
#include "pipewire/client/context.h"
#include "pipewire/client/extension.h"

struct pw_protocol *pw_protocol_native_ext_client_node_init(void);

struct impl {
	struct pw_context *context;
	struct pw_properties *properties;
};

static struct impl *extension_new(struct pw_context *context, struct pw_properties *properties)
{
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	pw_log_debug("extension %p: new", impl);

	impl->context = context;
	impl->properties = properties;

	pw_protocol_native_ext_client_node_init();

	return impl;
}

#if 0
static void extension_destroy(struct impl *impl)
{
	pw_log_debug("extension %p: destroy", impl);

	free(impl);
}
#endif

bool pipewire__extension_init(struct pw_extension *extension, const char *args)
{
	extension_new(extension->context, NULL);
	return true;
}
