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

#include <spa/buffer.h>

#include <pipewire/node.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pw_jack_port;

struct pw_jack_node {
	struct pw_node *node;

	struct pw_core *core;
	struct jack_server *server;

        struct jack_client_control *control;

	struct pw_jack_port *driverport;

	struct spa_list graph_link;

	void *user_data;
};

struct pw_jack_node_events {
#define PW_VERSION_JACK_NODE_EVENTS 0
        uint32_t version;

        void (*destroy) (void *data);

        void (*free) (void *data);

	/** the state of the node changed */
	void (*state_changed) (void *data, enum pw_node_state old,
			       enum pw_node_state state, const char *error);

        void (*pull) (void *data);

        void (*push) (void *data);
};

struct pw_jack_port {
	struct pw_jack_node *node;

	enum pw_direction direction;
	struct pw_port *port;

	jack_port_id_t port_id;
	struct jack_port *jack_port;
	float *ptr;

	void *user_data;
};

struct pw_jack_port_events {
#define PW_VERSION_JACK_PORT_EVENTS 0
        uint32_t version;

        void (*destroy) (void *data);

        void (*free) (void *data);
};

struct pw_jack_node *
pw_jack_node_new(struct pw_core *core,
		 struct pw_global *parent,
		 struct jack_server *server,
		 const char *name,
		 int pid,
		 struct pw_properties *properties,
		 size_t user_data_size);

struct pw_jack_node *
pw_jack_driver_new(struct pw_core *core,
		   struct pw_global *parent,
		   struct jack_server *server,
		   const char *name,
		   int n_capture_channels,
		   int n_playback_channels,
		   struct pw_properties *properties,
		   size_t user_data_size);

void
pw_jack_node_destroy(struct pw_jack_node *node);

void pw_jack_node_add_listener(struct pw_jack_node *node,
			       struct spa_hook *listener,
			       const struct pw_jack_node_events *events,
			       void *data);

struct pw_jack_port *
pw_jack_node_add_port(struct pw_jack_node *node,
		      const char *name,
		      const char *type,
                      unsigned int flags,
                      size_t user_data_size);

void pw_jack_port_add_listener(struct pw_jack_port *port,
			       struct spa_hook *listener,
			       const struct pw_jack_port_events *events,
			       void *data);

struct pw_jack_port *
pw_jack_node_find_port(struct pw_jack_node *node,
		       enum pw_direction direction,
		       jack_port_id_t port_id);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_JACK_NODE_H__ */
