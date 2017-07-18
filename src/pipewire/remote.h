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

#ifndef __PIPEWIRE_REMOTE_H__
#define __PIPEWIRE_REMOTE_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <pipewire/map.h>
#include <pipewire/loop.h>
#include <pipewire/properties.h>
#include <pipewire/protocol.h>
#include <pipewire/proxy.h>
#include <pipewire/type.h>
#include <pipewire/core.h>

/** \page page_remote_api Remote API
 *
 * \section sec_remote_api_overview Overview
 *
 * The remote API allows you to connect to a remote PipeWire and
 * perform actions on the PipeWire graph. This includes
 *
 * \li introspecting the objects on the server
 * \li Creating nodes
 * \li Linking nodes on their ports
 * \li providing media to the server for playback or consumption
 * \li retrieving media from the server
 *
 * \section sec_remote_api_loop Event Loop Abstraction
 *
 * Most API is asynchronous and based around an event loop. Methods will
 * start an operation which will cause a state change of the \ref pw_context
 * object. Connect to the state_changed signal to be notified of these
 * state changes.
 *
 * The most convenient way to deal with the asynchronous calls is probably
 * with the thread loop (See \subpage page_thread_loop for more details).
 *
 * \subsection ssec_remote_api_proxy Proxy
 *
 * Proxies are local representations of remote resources. They
 * allow communication between local and remote objects.
 *
 * The \ref pw_remote maintains a list of all proxies, including a core
 * proxy that is used to get access to other proxy objects.
 *
 * See also \subpage page_proxy
 *
 * \section sec_remote_api_remote Remote
 *
 * \subsection ssec_remote_create Create
 *
 * To create a new remote use pw_remote_new(). You will
 * need to pass a \ref pw_core implementation for event and data loop.
 *
 * A typical loop would be created with pw_thread_loop_new() but
 * other implementation are possible.
 *
 * You will also need to pass properties for the remote. Use
 * pw_fill_remote_properties() to get a default set of properties.
 *
 * After creating the remote, you can track the state of the remote
 * by listening for the state_changed signal.
 *
 * \subsection ssec_remote_api_remote_connect Connecting
 *
 * A remote must be connected to a server before any operation can be
 * issued.  Calling pw_remote_connect() will initiate the connection
 * procedure.
 *
 * When connecting, the remote will automatically create a core
 * proxy to get access to the registry and types.
 *
 * \subsection ssec_remote_api_remote_disconnect Disconnect
 *
 * Use pw_remote_disconnect() to disconnect from the remote.
 */

/** \enum pw_remote_state The state of a \ref pw_remote \memberof pw_remote */
enum pw_remote_state {
	PW_REMOTE_STATE_ERROR = -1,		/**< remote is in error */
	PW_REMOTE_STATE_UNCONNECTED = 0,	/**< not connected */
	PW_REMOTE_STATE_CONNECTING = 1,		/**< connecting to remote PipeWire */
	PW_REMOTE_STATE_CONNECTED = 2,		/**< remote is connected and ready */
};

/** Convert a \ref pw_remote_state to a readable string \memberof pw_remote */
const char *pw_remote_state_as_string(enum pw_remote_state state);

/** \class pw_remote
 *
 * \brief Represents a connection with the PipeWire server
 *
 * a \ref pw_remote is created and used to connect to the server.
 * A \ref pw_proxy for the core object will automatically be created
 * when connecting.
 *
 * See also \ref page_client_api
 */
struct pw_remote {
	struct pw_core *core;			/**< core */
	struct spa_list link;			/**< link in core remote_list */
	struct pw_properties *properties;	/**< extra properties */

	struct pw_core_proxy *core_proxy;	/**< proxy for the core object */
	struct pw_map objects;			/**< map of client side proxy objects
						 *   indexed with the client id */
        struct pw_core_info *info;		/**< info about the remote core */
	/** Signal emited when the core info changed */
	PW_SIGNAL(info_changed, (struct pw_listener *listener, struct pw_remote *remote));

	/** Signal emited when a reply to a sync was received */
	PW_SIGNAL(sync_reply, (struct pw_listener *listener, struct pw_remote *remote, uint32_t seq));

	uint32_t n_types;			/**< number of client types */
	struct pw_map types;			/**< client types */

	struct spa_list proxy_list;		/**< list of \ref pw_proxy objects */
	struct spa_list stream_list;		/**< list of \ref pw_stream objects */

	struct pw_protocol_connection *conn;	/**< the protocol connection */

	enum pw_remote_state state;
	char *error;
	/** Signal emited when the state changes */
	PW_SIGNAL(state_changed, (struct pw_listener *listener, struct pw_remote *remote));

	/** Signal emited when the remote is destroyed */
	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_remote *remote));
};

/** Create a new unconnected remote \memberof pw_remote
 * \return a new unconnected remote */
struct pw_remote *
pw_remote_new(struct pw_core *core,		/**< a \ref pw_core */
	      struct pw_properties *properties	/**< optional properties, ownership of
						  *  the properties is taken.*/ );

/** Destroy a remote \memberof pw_remote */
void pw_remote_destroy(struct pw_remote *remote);

/** Connect to a remote PipeWire \memberof pw_remote
 * \return true on success. */
int pw_remote_connect(struct pw_remote *remote);

/** Connect to a remote PipeWire on the given socket \memberof pw_remote
 * \param fd the connected socket to use
 * \return true on success. */
int pw_remote_connect_fd(struct pw_remote *remote, int fd);

/** Disconnect from the remote PipeWire. \memberof pw_remote */
void pw_remote_disconnect(struct pw_remote *remote);

/** Update the state of the remote, mostly used by protocols */
void pw_remote_update_state(struct pw_remote *remote, enum pw_remote_state state, const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_REMOTE_H__ */
