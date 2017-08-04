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

/** \class pw_link
 *
 * PipeWire link interface.
 */
struct pw_link;

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

struct pw_link_events {
#define PW_VERSION_LINK_EVENTS	0
	uint32_t version;

	void (*destroy) (void *data);

	void (*info_changed) (void *data, const struct pw_link_info *info);

	void (*state_changed) (void *data, enum pw_link_state old,
					   enum pw_link_state state, const char *error);

	void (*port_unlinked) (void *data, struct pw_port *port);
};

/** Make a new link between two ports \memberof pw_link
 * \return a newly allocated link */
struct pw_link *
pw_link_new(struct pw_core *core,		/**< the core object */
	    struct pw_global *parent,		/**< parent global */
	    struct pw_port *output,		/**< an output port */
	    struct pw_port *input,		/**< an input port */
	    struct spa_format *format_filter,	/**< an optional format filter */
	    struct pw_properties *properties	/**< extra properties */,
	    char **error			/**< error string when result is NULL */);

/** Destroy a link \memberof pw_link */
void pw_link_destroy(struct pw_link *link);

void pw_link_add_listener(struct pw_link *link,
			  struct pw_listener *listener,
			  const struct pw_link_events *events,
			  void *data);

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
