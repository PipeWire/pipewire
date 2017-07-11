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

#ifndef __PIPEWIRE_CLIENT_NODE_H__
#define __PIPEWIRE_CLIENT_NODE_H__

#include <pipewire/node.h>
#include <extensions/client-node.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \class pw_client_node
 *
 * PipeWire client node interface
 */
struct pw_client_node {
	struct pw_node *node;

	struct pw_resource *resource;

	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_client_node *node));
};

struct pw_client_node *
pw_client_node_new(struct pw_resource *resource,
		   const char *name,
		   struct pw_properties *properties);

void
pw_client_node_destroy(struct pw_client_node *node);

int
pw_client_node_get_fds(struct pw_client_node *node, int *readfd, int *writefd);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_CLIENT_NODE_H__ */
