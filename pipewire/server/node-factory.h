/* PipeWire
 * Copyright (C) 2016 Axis Communications AB
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

#ifndef __PIPEWIRE_NODE_FACTORY_H__
#define __PIPEWIRE_NODE_FACTORY_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PIPEWIRE_TYPE__NodeFactory                            "PipeWire:Object:NodeFactory"
#define PIPEWIRE_TYPE_NODE_FACTORY_BASE                       PIPEWIRE_TYPE__NodeFactory ":"

#include <pipewire/server/core.h>
#include <pipewire/server/client.h>

/**
 * pw_node_factory:
 *
 * PipeWire node factory interface.
 */
struct pw_node_factory {
	struct pw_core *core;
	struct spa_list link;
	struct pw_global *global;

	const char *name;

	struct pw_node *(*create_node) (struct pw_node_factory *factory,
					struct pw_client *client,
					const char *name, struct pw_properties *properties);
};

#define pw_node_factory_create_node(f,...)	(f)->create_node((f),__VA_ARGS__)

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_NODE_FACTORY_H__ */
