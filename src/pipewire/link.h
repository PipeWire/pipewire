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

/** \class pw_link
 *
 * PipeWire link object.
 */
struct pw_link;

#include <pipewire/core.h>
#include <pipewire/introspect.h>
#include <pipewire/port.h>

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

/** link events added with \ref pw_link_add_listener */
struct pw_link_events {
#define PW_VERSION_LINK_EVENTS	0
	uint32_t version;

	/** A link is destroyed */
	void (*destroy) (void *data);

	/** A link is freed */
	void (*free) (void *data);

	/** The info changed on a link */
	void (*info_changed) (void *data, const struct pw_link_info *info);

	/** The link state changed, \a error is only valid when the state is
	  * in error. */
	void (*state_changed) (void *data, enum pw_link_state old,
					   enum pw_link_state state, const char *error);

	/** A port is unlinked */
	void (*port_unlinked) (void *data, struct pw_port *port);
};

/** Indicate that a link is passive, it does not cause the nodes to activate,
  * set to "1" or "0" */
#define PW_LINK_PROP_PASSIVE	"pipewire.link.passive"

/** Make a new link between two ports \memberof pw_link
 * \return a newly allocated link */
struct pw_link *
pw_link_new(struct pw_core *core,		/**< the core object */
	    struct pw_port *output,		/**< an output port */
	    struct pw_port *input,		/**< an input port */
	    struct spa_pod *format_filter,	/**< an optional format filter */
	    struct pw_properties *properties	/**< extra properties */,
	    char **error,			/**< error string when result is NULL */
	    size_t user_data_size		/**< extra user data size */);

/** Destroy a link \memberof pw_link */
void pw_link_destroy(struct pw_link *link);

/** Add an event listener to \a link */
void pw_link_add_listener(struct pw_link *link,
			  struct spa_hook *listener,
			  const struct pw_link_events *events,
			  void *data);

/** Finish link configuration and register */
int pw_link_register(struct pw_link *link,		/**< the link to register */
		     struct pw_client *owner,		/**< optional link owner */
		     struct pw_global *parent,		/**< parent global */
		     struct pw_properties *properties	/**< extra properties */);

/** Get the core of a link */
struct pw_core *pw_link_get_core(struct pw_link *link);

/** Get the user_data of a link, the size of the memory is given when
  * constructing the link */
void *pw_link_get_user_data(struct pw_link *link);

/** Get the link info */
const struct pw_link_info *pw_link_get_info(struct pw_link *link);

/** Get the global of the link */
struct pw_global *pw_link_get_global(struct pw_link *link);

/** Get the output port of the link */
struct pw_port *pw_link_get_output(struct pw_link *link);

/** Get the input port of the link */
struct pw_port *pw_link_get_input(struct pw_link *link);

/** Find the link between 2 ports \memberof pw_link */
struct pw_link *pw_link_find(struct pw_port *output, struct pw_port *input);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_LINK_H__ */
