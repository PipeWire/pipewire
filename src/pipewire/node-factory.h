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

#define PW_TYPE_INTERFACE__NodeFactory                  PW_TYPE_INTERFACE_BASE "NodeFactory"
#define PW_TYPE_NODE_FACTORY_BASE                       PW_TYPE_INTERFACE__NodeFactory ":"

/** \class pw_node_factory
 *
 * \brief PipeWire node factory interface.
 *
 * The factory object is used to make nodes on demand.
 */
struct pw_node_factory;

#include <pipewire/core.h>
#include <pipewire/client.h>
#include <pipewire/global.h>
#include <pipewire/properties.h>
#include <pipewire/resource.h>

struct pw_node_factory_implementation {
#define PW_VERSION_NODE_FACRORY_IMPLEMENTATION	0
	uint32_t version;

	/** The function to create a node from this factory */
	struct pw_node *(*create_node) (void *data,
					struct pw_resource *resource,
					const char *name,
					struct pw_properties *properties);
};

struct pw_node_factory *pw_node_factory_new(struct pw_core *core,
					    const char *name,
					    size_t user_data_size);

void pw_node_factory_export(struct pw_node_factory *factory,
			    struct pw_client *owner,
			    struct pw_global *parent);

void *pw_node_factory_get_user_data(struct pw_node_factory *factory);

void pw_node_factory_set_implementation(struct pw_node_factory *factory,
					const struct pw_node_factory_implementation *implementation,
					void *data);

struct pw_node *pw_node_factory_create_node(struct pw_node_factory *factory,
					    struct pw_resource *resource,
					    const char *name,
					    struct pw_properties *properties);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_NODE_FACTORY_H__ */
