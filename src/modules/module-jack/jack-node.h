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

#ifndef __PIPEWIRE_JACK_NODE_H__
#define __PIPEWIRE_JACK_NODE_H__

#include <pipewire/node.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pw_jack_node {
	struct pw_core *core;
	struct pw_node *node;

	struct spa_hook_list listener_list;

	struct jack_server *server;
	struct jack_client *client;
	int ref_num;

	struct pw_port *otherport;

	struct spa_list graph_link;
};

struct pw_jack_node_events {
#define PW_VERSION_JACK_NODE_EVENTS 0
        uint32_t version;

        void (*destroy) (void *data);

        void (*pull) (void *data);

        void (*push) (void *data);
};


struct pw_jack_node *
pw_jack_node_new(struct pw_core *core,
		 struct pw_global *parent,
		 struct jack_server *server,
		 int ref_num,
		 struct pw_properties *properties);

void
pw_jack_node_destroy(struct pw_jack_node *node);

struct pw_node *pw_jack_node_get_node(struct pw_jack_node *node);

void pw_jack_node_add_listener(struct pw_jack_node *node,
			       struct spa_hook *listener,
			       const struct pw_jack_node_events *events,
			       void *data);

struct pw_port *pw_jack_node_add_port(struct pw_jack_node *node,
				      enum pw_direction direction,
				      jack_port_id_t port_id);

struct pw_port *pw_jack_node_find_port(struct pw_jack_node *node,
				       enum pw_direction direction, jack_port_id_t port_id);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_JACK_NODE_H__ */
