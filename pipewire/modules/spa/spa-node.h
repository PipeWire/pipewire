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

#ifndef __PIPEWIRE_SPA_NODE_H__
#define __PIPEWIRE_SPA_NODE_H__

#include <pipewire/server/core.h>
#include <pipewire/server/node.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pw_spa_node {
	struct pw_node *node;

	char *lib;
	char *factory_name;
	struct spa_handle *handle;
	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_spa_node *node));
};

typedef int (*setup_node_t) (struct pw_core *core,
			     struct spa_node *spa_node,
			     struct pw_properties *pw_props);

struct pw_spa_node *
pw_spa_node_load(struct pw_core *core,
		 const char *dir,
		 const char *lib,
		 const char *factory_name,
		 const char *name,
		 struct pw_properties *properties,
		 setup_node_t setup_func);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_SPA_NODE_H__ */
