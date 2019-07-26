/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifndef PIPEWIRE_PORT_H
#define PIPEWIRE_PORT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/hook.h>

/** \page page_port Port
 *
 * \section page_node_overview Overview
 *
 * A port can be used to link two nodes.
 */
/** \class pw_port
 *
 * The port object
 */
struct pw_port;
struct pw_link;
struct pw_control;

#include <pipewire/core.h>
#include <pipewire/introspect.h>
#include <pipewire/node.h>

enum pw_port_state {
	PW_PORT_STATE_ERROR = -1,	/**< the port is in error */
	PW_PORT_STATE_INIT = 0,		/**< the port is being created */
	PW_PORT_STATE_CONFIGURE = 1,	/**< the port is ready for format negotiation */
	PW_PORT_STATE_READY = 2,	/**< the port is ready for buffer allocation */
	PW_PORT_STATE_PAUSED = 3,	/**< the port is paused */
};

/** Port events, use \ref pw_port_add_listener */
struct pw_port_events {
#define PW_VERSION_PORT_EVENTS 0
	uint32_t version;

	/** The port is destroyed */
	void (*destroy) (void *data);

	/** The port is freed */
	void (*free) (void *data);

	/** the port info changed */
	void (*info_changed) (void *data, const struct pw_port_info *info);

	/** a new link is added on this port */
	void (*link_added) (void *data, struct pw_link *link);

	/** a link is removed from this port */
	void (*link_removed) (void *data, struct pw_link *link);

	/** the state of the port changed */
	void (*state_changed) (void *data, enum pw_port_state old,
			enum pw_port_state state, const char *error);

	/** a control was added to the port */
	void (*control_added) (void *data, struct pw_control *control);

	/** a control was removed from the port */
	void (*control_removed) (void *data, struct pw_control *control);
};

/** Get the port direction */
enum pw_direction pw_port_get_direction(struct pw_port *port);

/** Get the port properties */
const struct pw_properties *pw_port_get_properties(struct pw_port *port);

/** Update the port properties */
int pw_port_update_properties(struct pw_port *port, const struct spa_dict *dict);

/** Get the port id */
uint32_t pw_port_get_id(struct pw_port *port);

/** Get the port parent node or NULL when not yet set */
struct pw_node *pw_port_get_node(struct pw_port *port);

/** Add an event listener on the port */
void pw_port_add_listener(struct pw_port *port,
			  struct spa_hook *listener,
			  const struct pw_port_events *events,
			  void *data);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_PORT_H */
