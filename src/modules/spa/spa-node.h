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

#include <spa/node/node.h>

#include <pipewire/core.h>
#include <pipewire/node.h>

#ifdef __cplusplus
extern "C" {
#endif

enum pw_spa_node_flags {
	PW_SPA_NODE_FLAG_ASYNC		= (1 << 0),
	PW_SPA_NODE_FLAG_DISABLE	= (1 << 1),
	PW_SPA_NODE_FLAG_ACTIVATE	= (1 << 2),
	PW_SPA_NODE_FLAG_NO_REGISTER	= (1 << 3),
};

struct pw_node *
pw_spa_node_new(struct pw_core *core,
		struct pw_client *owner,	/**< optional owner */
		struct pw_global *parent,	/**< optional parent */
		const char *name,
		enum pw_spa_node_flags flags,
		struct spa_node *node,
		struct spa_handle *handle,
		struct pw_properties *properties,
		size_t user_data_size);

struct pw_node *
pw_spa_node_load(struct pw_core *core,
		 struct pw_client *owner,	/**< optional owner */
		 struct pw_global *parent,	/**< optional parent */
		 const char *lib,
		 const char *factory_name,
		 const char *name,
		 enum pw_spa_node_flags flags,
		 struct pw_properties *properties,
		 size_t user_data_size);

void *pw_spa_node_get_user_data(struct pw_node *node);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_SPA_NODE_H__ */
