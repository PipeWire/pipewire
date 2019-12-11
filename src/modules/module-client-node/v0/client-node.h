/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef PIPEWIRE_CLIENT_NODE0_H
#define PIPEWIRE_CLIENT_NODE0_H

#include <pipewire/impl.h>

#include "ext-client-node.h"

#ifdef __cplusplus
extern "C" {
#endif

/** \class pw_client_node0
 *
 * PipeWire client node interface
 */
struct pw_client_node0 {
	struct pw_impl_node *node;

	struct pw_resource *resource;
};

struct pw_client_node0 *
pw_client_node0_new(struct pw_resource *resource,
		   struct pw_properties *properties);

void
pw_client_node0_destroy(struct pw_client_node0 *node);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_CLIENT_NODE0_H */
