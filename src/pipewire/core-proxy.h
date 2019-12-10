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

#ifndef PIPEWIRE_REMOTE_H
#define PIPEWIRE_REMOTE_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/hook.h>

#include <pipewire/context.h>
#include <pipewire/properties.h>
#include <pipewire/node.h>
#include <pipewire/proxy.h>

/** Connect to a PipeWire instance \memberof pw_core_proxy
 * \return a pw_core_proxy on success or NULL with errno set on error */
struct pw_core_proxy *
pw_context_connect(struct pw_context *context,		/**< a \ref pw_context */
	      struct pw_properties *properties,	/**< optional properties, ownership of
						  *  the properties is taken.*/
	      size_t user_data_size		/**< extra user data size */);

/** Connect to a PipeWire instance on the given socket \memberof pw_core_proxy
 * \param fd the connected socket to use, the socket will be closed
 *	automatically on disconnect or error.
 * \return a pw_core_proxy on success or NULL with errno set on error */
struct pw_core_proxy *
pw_context_connect_fd(struct pw_context *context,	/**< a \ref pw_context */
	      int fd,				/**< an fd */
	      struct pw_properties *properties,	/**< optional properties, ownership of
						  *  the properties is taken.*/
	      size_t user_data_size		/**< extra user data size */);

/** Connect to a given PipeWire instance \memberof pw_core_proxy
 * \return a pw_core_proxy on success or NULL with errno set on error */
struct pw_core_proxy *
pw_context_connect_self(struct pw_context *context,	/**< a \ref pw_context to connect to */
	      struct pw_properties *properties,	/**< optional properties, ownership of
						  *  the properties is taken.*/
	      size_t user_data_size		/**< extra user data size */);

/** Steal the fd of the core_proxy connection or < 0 on error. The core_proxy
  * will be disconnected after this call. */
int pw_core_proxy_steal_fd(struct pw_core_proxy *core_proxy);

/** Get the core proxy, can only be called when connected */
int pw_core_proxy_disconnect(struct pw_core_proxy *proxy);

/** Get the user_data. It is of the size specified when this object was
 * constructed */
void *pw_core_proxy_get_user_data(struct pw_core_proxy *core_proxy);

/** Get the client proxy */
struct pw_client_proxy * pw_core_proxy_get_client_proxy(struct pw_core_proxy *proxy);

/** Get the context object used to created this core_proxy */
struct pw_context * pw_core_proxy_get_context(struct pw_core_proxy *proxy);

/** Get properties from the core_proxy */
const struct pw_properties *pw_core_proxy_get_properties(struct pw_core_proxy *proxy);

/** Update the core_proxy properties. This updates the properties
 * of the associated client.
 * \return the number of properties that were updated */
int pw_core_proxy_update_properties(struct pw_core_proxy *core_proxy, const struct spa_dict *dict);

/** Get the core_proxy mempool object */
struct pw_mempool * pw_core_proxy_get_mempool(struct pw_core_proxy *proxy);

/** Get the proxy with the given id */
struct pw_proxy *pw_core_proxy_find_proxy(struct pw_core_proxy *proxy, uint32_t id);

/** Export an object into the PipeWire instance associated with core_proxy */
struct pw_proxy *pw_core_proxy_export(struct pw_core_proxy *proxy,	/**< the proxy */
				  uint32_t type,			/**< the type of object */
				  struct pw_properties *properties,	/**< extra properties */
				  void *object,				/**< object to export */
				  size_t user_data_size			/**< extra user data */);


#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_REMOTE_H */
