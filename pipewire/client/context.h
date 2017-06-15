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

#ifndef __PIPEWIRE_CONTEXT_H__
#define __PIPEWIRE_CONTEXT_H__

#ifdef __cplusplus
extern "C" {
#endif

/** \page page_client_api Client API
 *
 * \section sec_client_api_overview Overview
 *
 * The client side API allows you to connect to the PipeWire server and
 * perform actions on the PipeWire graph. This includes
 *
 * \li introspecting the objects on the server
 * \li Creating nodes
 * \li Linking nodes on their ports
 * \li providing media to the server for playback or consumption
 * \li retrieving media from the server
 *
 * \section sec_client_api_loop Event Loop Abstraction
 *
 * Most API is asynchronous and based around an event loop. Methods will
 * start an operation which will cause a state change of the \ref pw_context
 * object. Connect to the state_changed signal to be notified of these
 * state changes.
 *
 * The most convenient way to deal with the asynchronous calls is probably
 * with the thread loop (See \subpage page_thread_loop for more details).
 *
 * \subsection ssec_client_api_context_proxy Proxy
 *
 * Proxies are client side representations of server side resources. They
 * allow communication between client and server objects.
 *
 * The context maintains a list of all proxies, including a core proxy
 * object and a registry object.
 *
 * See also \subpage page_proxy
 *
 * \section sec_client_api_context Context
 *
 * \subsection ssec_context_create Create
 *
 * To create a new context use pw_context_new(). You will
 * need to pass a \ref pw_loop implementation to use as the event loop.
 *
 * A typical loop would be created with pw_thread_loop_new() but
 * other implementation are possible.
 *
 * You will also need to pass properties for the context. Use
 * pw_fill_context_properties() to get a default set of properties.
 *
 * After creating the context, you can track the state of the context
 * by listening for the state_changed signal.
 *
 * \subsection ssec_client_api_context_connect Connecting
 *
 * A context must be connected to a server before any operation can be
 * issued.  Calling pw_context_connect() will initiate the connection
 * procedure.
 *
 * When connecting, the context will automatically create a registry
 * proxy to get notified of server objects. This behaviour can be disabled
 * by passing the \ref PW_CONTEXT_FLAG_NO_REGISTRY. You can create your
 * own registry later from the core_proxy member of the context.
 *
 * The context will automatically create proxies for all remote objects
 * and will bind to them. Use the subscription signal to reveive
 * notifications about objects. You can also disable this behaviour
 * with the \ref PW_CONTEXT_FLAG_NO_PROXY flag and manually bind to
 * the objects you are interested in.
 *
 * \subsection ssec_client_api_context_functions Streams
 *
 * Data exchange with the PipeWire server is done with the \ref pw_stream
 * object. \subpage page_streams
 *
 * \subsection ssec_client_api_context_disconnect Disconnect
 *
 * Use pw_context_disconnect() to disconnect from the server.
 */

#include <pipewire/client/map.h>
#include <pipewire/client/loop.h>
#include <pipewire/client/properties.h>
#include <pipewire/client/protocol.h>
#include <pipewire/client/subscribe.h>
#include <pipewire/client/proxy.h>
#include <pipewire/client/type.h>

/** \enum pw_context_state The state of a \ref pw_context \memberof pw_context */
enum pw_context_state {
	PW_CONTEXT_STATE_ERROR = -1,		/**< context is in error */
	PW_CONTEXT_STATE_UNCONNECTED = 0,	/**< not connected */
	PW_CONTEXT_STATE_CONNECTING = 1,	/**< connecting to PipeWire daemon */
	PW_CONTEXT_STATE_CONNECTED = 2,		/**< context is connected and ready */
};

/** Convert a \ref pw_context_state to a readable string \memberof pw_context */
const char *pw_context_state_as_string(enum pw_context_state state);

/** \enum pw_context_flags Extra flags passed to pw_context_connect() \memberof pw_context */
enum pw_context_flags {
	PW_CONTEXT_FLAG_NONE = 0,		/**< no flags */
	PW_CONTEXT_FLAG_NO_REGISTRY = (1 << 0),	/**< don't create the registry object */
	PW_CONTEXT_FLAG_NO_PROXY = (1 << 1),	/**< don't automatically create proxies for
						 *   server side objects */
};

/** \class pw_context
 *
 * \brief Represents a connection with the PipeWire server
 *
 * a \ref pw_context is created and used to connect to the server.
 * A \ref pw_proxy for the core object will automatically be created
 * when connecting.
 *
 * See also \ref page_client_api
 */
struct pw_context {
	char *name;				/**< the application name */
	struct pw_properties *properties;	/**< extra properties */

	struct pw_type type;			/**< the type map */

	struct pw_loop *loop;			/**< the loop */

	struct spa_support *support;		/**< support for spa plugins */
	uint32_t n_support;			/**< number of support items */

	struct pw_proxy *core_proxy;		/**< proxy for the core object */
	struct pw_proxy *registry_proxy;	/**< proxy for the registry object. Can
						 *   be NULL when \ref PW_CONTEXT_FLAG_NO_PROXY
						 *   was specified */
	struct pw_map objects;			/**< map of client side proxy objects
						 *   indexed with the client id */
	uint32_t n_types;			/**< number of client types */
	struct pw_map types;			/**< client types */

	struct spa_list stream_list;		/**< list of \ref pw_stream objects */
	struct spa_list proxy_list;		/**< list of \ref pw_proxy objects */

	struct pw_protocol *protocol;		/**< the protocol in use */
	void *protocol_private;			/**< private data for the protocol */

	enum pw_context_state state;		/**< context state */
	char *error;				/**< error string */
	/** Signal emited when the state changes */
	PW_SIGNAL(state_changed, (struct pw_listener *listener, struct pw_context *context));

	/** Signal emited when a global is added/changed/removed */
	PW_SIGNAL(subscription, (struct pw_listener *listener,
				 struct pw_context *context,
				 enum pw_subscription_event event, uint32_t type, uint32_t id));

	/** Signal emited when the context is destroyed */
	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_context *context));
};

struct pw_context *
pw_context_new(struct pw_loop *loop,
	       const char *name, struct pw_properties *properties);

void pw_context_destroy(struct pw_context *context);

bool pw_context_connect(struct pw_context *context, enum pw_context_flags flags);

bool pw_context_connect_fd(struct pw_context *context, enum pw_context_flags flags, int fd);

bool pw_context_disconnect(struct pw_context *context);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_CONTEXT_H__ */
