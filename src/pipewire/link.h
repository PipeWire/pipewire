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

#ifndef __PIPEWIRE_LINK_H__
#define __PIPEWIRE_LINK_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/ringbuffer.h>

#include <pipewire/mem.h>
#include <pipewire/introspect.h>

#include <pipewire/type.h>
#include <pipewire/core.h>
#include <pipewire/port.h>
#include <pipewire/main-loop.h>

#define PW_TYPE__Link             PW_TYPE_OBJECT_BASE "Link"
#define PW_TYPE_LINK_BASE         PW_TYPE__Link ":"

/** \page page_link Link
 *
 * \section page_link_overview Overview
 *
 * A link is the connection between 2 nodes (\ref page_node). Nodes are
 * linked together on ports.
 *
 * The link is responsible for negotiating the format and buffers for
 * the nodes.
 */

/** \class pw_link
 *
 * PipeWire link interface.
 */
struct pw_link {
	struct pw_core *core;		/**< core object */
	struct spa_list link;		/**< link in core link_list */
	struct pw_global *global;	/**< global for this link */

	struct pw_properties *properties;	/**< extra link properties */

        struct pw_link_info info;		/**< introspectable link info */

	enum pw_link_state state;	/**< link state */
	char *error;			/**< error message when state error */
	/** Emited when the link state changed */
	PW_SIGNAL(state_changed, (struct pw_listener *listener,
				  struct pw_link *link,
				  enum pw_link_state old, enum pw_link_state state));

	/** Emited when the link is destroyed */
	PW_SIGNAL(destroy_signal, (struct pw_listener *, struct pw_link *));

	struct spa_list resource_list;	/**< list of bound resources */

	struct spa_port_io io;		/**< link io area */

	struct pw_port *output;		/**< output port */
	struct spa_list output_link;	/**< link in output port links */
	struct pw_port *input;		/**< input port */
	struct spa_list input_link;	/**< link in input port links */

	/** Emited when the port is unlinked */
	PW_SIGNAL(port_unlinked, (struct pw_listener *listener,
				  struct pw_link *link, struct pw_port *port));

	struct {
		struct spa_graph_port out_port;
		struct spa_graph_port in_port;
	} rt;
};


/** Make a new link between two ports \memberof pw_link
 * \return a newly allocated link */
struct pw_link *
pw_link_new(struct pw_core *core,		/**< the core object */
	    struct pw_port *output,		/**< an output port */
	    struct pw_port *input,		/**< an input port */
	    struct spa_format *format_filter,	/**< an optional format filter */
	    struct pw_properties *properties	/**< extra properties */,
	    char **error			/**< error string */);

/** Destroy a link \memberof pw_link */
void pw_link_destroy(struct pw_link *link);

/** Find the link between 2 ports \memberof pw_link */
struct pw_link * pw_link_find(struct pw_port *output, struct pw_port *input);

/** Activate a link \memberof pw_link
 * Starts the negotiation of formats and buffers on \a link and then
 * starts data streaming */
bool pw_link_activate(struct pw_link *link);

/** Deactivate a link \memberof pw_link */
bool pw_link_deactivate(struct pw_link *link);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_LINK_H__ */
