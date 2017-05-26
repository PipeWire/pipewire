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

#ifndef __PIPEWIRE_NODE_H__
#define __PIPEWIRE_NODE_H__

#ifdef __cplusplus
extern "C" {
#endif

#define PIPEWIRE_TYPE__Node                          "PipeWire:Object:Node"
#define PIPEWIRE_TYPE_NODE_BASE                      PIPEWIRE_TYPE__Node ":"

#include <spa/clock.h>
#include <spa/node.h>

#include <pipewire/client/mem.h>
#include <pipewire/client/transport.h>

#include <pipewire/server/core.h>
#include <pipewire/server/port.h>
#include <pipewire/server/link.h>
#include <pipewire/server/client.h>
#include <pipewire/server/data-loop.h>

/**
 * pw_node:
 *
 * PipeWire node class.
 */
struct pw_node {
	struct pw_core *core;
	struct spa_list link;
	struct pw_global *global;

	struct pw_client *owner;
	char *name;
	struct pw_properties *properties;
	enum pw_node_state state;
	char *error;
	PW_SIGNAL(state_request, (struct pw_listener * listener,
				  struct pw_node * object, enum pw_node_state state));
	PW_SIGNAL(state_changed, (struct pw_listener * listener,
				  struct pw_node * object,
				  enum pw_node_state old, enum pw_node_state state));

	struct spa_handle *handle;
	struct spa_node *node;
	bool live;
	struct spa_clock *clock;

	struct spa_list resource_list;

	PW_SIGNAL(initialized, (struct pw_listener * listener, struct pw_node * object));

	uint32_t max_input_ports;
	uint32_t n_input_ports;
	struct spa_list input_ports;
	struct pw_port **input_port_map;
	uint32_t n_used_input_links;

	uint32_t max_output_ports;
	uint32_t n_output_ports;
	struct spa_list output_ports;
	struct pw_port **output_port_map;
	uint32_t n_used_output_links;

	PW_SIGNAL(port_added, (struct pw_listener * listener,
			       struct pw_node * node, struct pw_port * port));
	PW_SIGNAL(port_removed, (struct pw_listener * listener,
				 struct pw_node * node, struct pw_port * port));

	PW_SIGNAL(destroy_signal, (struct pw_listener * listener, struct pw_node * object));
	PW_SIGNAL(free_signal, (struct pw_listener * listener, struct pw_node * object));

	PW_SIGNAL(async_complete, (struct pw_listener * listener,
				   struct pw_node * node, uint32_t seq, int res));

	struct pw_data_loop *data_loop;
	PW_SIGNAL(loop_changed, (struct pw_listener * listener, struct pw_node * object));
};

struct pw_node *
pw_node_new(struct pw_core *core,
	    struct pw_client *owner,
	    const char *name,
	    bool async,
	    struct spa_node *node,
	    struct spa_clock *clock,
	    struct pw_properties *properties);

void
pw_node_destroy(struct pw_node *node);


void
pw_node_set_data_loop(struct pw_node *node, struct pw_data_loop *loop);

struct pw_port *
pw_node_get_free_port(struct pw_node *node, enum pw_direction direction);

int
pw_node_set_state(struct pw_node *node, enum pw_node_state state);

void
pw_node_update_state(struct pw_node *node, enum pw_node_state state, char *error);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_NODE_H__ */
