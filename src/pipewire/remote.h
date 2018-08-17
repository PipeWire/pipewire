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

#include <spa/utils/hook.h>

/** \page page_remote_api Remote API
 *
 * \section sec_remote_api_overview Overview
 *
 * The remote API allows you to connect to a remote PipeWire instance
 * and perform actions on the PipeWire graph. This includes
 *
 * \li introspecting the objects on the instance
 * \li Creating nodes
 * \li Linking nodes on their ports
 * \li providing media to the server for playback or consumption
 * \li retrieving media from the remote instance
 *
 * \section sec_remote_api_loop Event Loop Abstraction
 *
 * Most API is asynchronous and based around an event loop. Methods will
 * start an operation which will cause a state change of the \ref pw_remote
 * object. Connect to the state_changed event to be notified of these
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
 * need to pass a local \ref pw_core implementation for event and
 * data loop.
 *
 * A typical loop would be created with pw_thread_loop_new() but
 * other implementation are possible.
 *
 * You will also need to pass properties for the remote. Use
 * pw_fill_remote_properties() to get a default set of properties.
 *
 * After creating the remote, you can track the state of the remote
 * by listening for the state_changed event.
 *
 * \subsection ssec_remote_api_remote_connect Connecting
 *
 * A remote must be connected before any operation can be issued.
 * Calling pw_remote_connect() will initiate the connection procedure.
 *
 * When connecting, the remote will automatically create a core
 * proxy to get access to the registry proxy and types.
 *
 * \subsection ssec_remote_api_remote_registry Registry
 *
 * \subpage page_registry
 *
 *
 * \subsection ssec_remote_api_remote_disconnect Disconnect
 *
 * Use pw_remote_disconnect() to disconnect from the remote.
 */
/** \class pw_remote
 *
 * \brief Represents a connection with a remote PipeWire instance
 *
 * a \ref pw_remote is created and used to connect to a remote PipeWire
 * instance.
 * A \ref pw_proxy for the core object will automatically be created
 * when connecting.
 *
 * See also \ref page_core_api
 */
struct pw_remote;

#include <pipewire/core.h>
#include <pipewire/properties.h>
#include <pipewire/protocol.h>
#include <pipewire/proxy.h>

/** \enum pw_remote_state The state of a \ref pw_remote \memberof pw_remote */
enum pw_remote_state {
	PW_REMOTE_STATE_ERROR = -1,		/**< remote is in error */
	PW_REMOTE_STATE_UNCONNECTED = 0,	/**< not connected */
	PW_REMOTE_STATE_CONNECTING = 1,		/**< connecting to remote PipeWire */
	PW_REMOTE_STATE_CONNECTED = 2,		/**< remote is connected and ready */
};

/** Convert a \ref pw_remote_state to a readable string \memberof pw_remote */
const char *pw_remote_state_as_string(enum pw_remote_state state);

/** Events for the remote. use \ref pw_remote_add_listener */
struct pw_remote_events {
#define PW_VERSION_REMOTE_EVENTS	0
	uint32_t version;

	/** The remote is destroyed */
	void (*destroy)	(void *data);
        /** emited when the remote core info changed */
	void (*info_changed) (void *data, const struct pw_core_info *info);
        /** emited when a reply to a sync was received */
	void (*sync_reply) (void *data, uint32_t seq);
	/** emited when the state changes */
	void (*state_changed) (void *data, enum pw_remote_state old,
			       enum pw_remote_state state, const char *error);
};

/** Specify the name of the protocol to use, default is using the native protocol */
#define PW_REMOTE_PROP_PROTOCOL		"pipewire.protocol"
/** The name of the remote to connect to, default env(PIPEWIRE_REMOTE) or pipewire-0 */
#define PW_REMOTE_PROP_REMOTE_NAME	"pipewire.remote.name"

/** Create a new unconnected remote \memberof pw_remote
 * \return a new unconnected remote */
struct pw_remote *
pw_remote_new(struct pw_core *core,		/**< a \ref pw_core */
	      struct pw_properties *properties,	/**< optional properties, ownership of
						  *  the properties is taken.*/
	      size_t user_data_size		/**< extra user data size */);

/** Destroy a remote \memberof pw_remote */
void pw_remote_destroy(struct pw_remote *remote);

/** Get the core used to construct this remote */
struct pw_core *pw_remote_get_core(struct pw_remote *remote);

/** Get the remote properties */
const struct pw_properties *pw_remote_get_properties(struct pw_remote *remote);

/** Get the user_data. The size was given in \ref pw_remote_new */
void *pw_remote_get_user_data(struct pw_remote *remote);

/** Get the current state, \a error is set when state is \ref PW_REMOTE_STATE_ERROR */
enum pw_remote_state pw_remote_get_state(struct pw_remote *remote, const char **error);

/** Add listener for events */
void pw_remote_add_listener(struct pw_remote *remote,
			    struct spa_hook *listener,
			    const struct pw_remote_events *events,
			    void *data);

/** Connect to a remote PipeWire \memberof pw_remote
 * \return 0 on success, < 0 on error */
int pw_remote_connect(struct pw_remote *remote);

/** Connect to a remote PipeWire on the given socket \memberof pw_remote
 * \param fd the connected socket to use
 * \return 0 on success, < 0 on error */
int pw_remote_connect_fd(struct pw_remote *remote, int fd);

/** Steal the fd of the remote connection or < 0 on error. The remote
  * will be in the unconnected state after this call. */
int pw_remote_steal_fd(struct pw_remote *remote);

/** Get the core proxy, can only be called when connected */
struct pw_core_proxy * pw_remote_get_core_proxy(struct pw_remote *remote);

/** Get the remote core info, can only be called when connected */
const struct pw_core_info *pw_remote_get_core_info(struct pw_remote *remote);

/** Get the proxy with the given id */
struct pw_proxy *pw_remote_find_proxy(struct pw_remote *remote, uint32_t id);

/** Disconnect from the remote PipeWire. \memberof pw_remote */
int pw_remote_disconnect(struct pw_remote *remote);

/** run a local node in a remote graph */
struct pw_proxy *pw_remote_export(struct pw_remote *remote, struct pw_node *node);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_REMOTE_H__ */
