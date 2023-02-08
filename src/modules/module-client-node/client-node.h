/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_CLIENT_NODE_H
#define PIPEWIRE_CLIENT_NODE_H

#include <pipewire/impl.h>
#include <pipewire/extensions/client-node.h>

#ifdef __cplusplus
extern "C" {
#endif

/** \class pw_impl_client_node
 *
 * PipeWire client node interface
 */
struct pw_impl_client_node {
	struct pw_impl_node *node;

	struct pw_resource *resource;
	uint32_t flags;
};

struct pw_impl_client_node *
pw_impl_client_node_new(struct pw_resource *resource,
		   struct pw_properties *properties,
		   bool do_register);

void
pw_impl_client_node_destroy(struct pw_impl_client_node *node);

void pw_impl_client_node_registered(struct pw_impl_client_node *node, struct pw_global *global);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_CLIENT_NODE_H */
