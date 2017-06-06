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

#include <pipewire/client/log.h>
#include <pipewire/client/proxy.h>
#include <pipewire/client/protocol-native.h>

/** \cond */
struct proxy {
	struct pw_proxy this;
};
/** \endcond */

/** Create a proxy object with a given id and type
 *
 * \param context Context object
 * \param id Id of the new object, SPA_ID_INVALID will choose a new id
 * \param type Type of the proxy object
 * \return A newly allocated proxy object or NULL on failure
 *
 * This function creates a new proxy object with the supplied id and type. The
 * proxy object will have an id assigned from the client id space.
 *
 * \sa pw_context
 *
 * \memberof pw_proxy
 */
struct pw_proxy *pw_proxy_new(struct pw_context *context, uint32_t id, uint32_t type)
{
	struct proxy *impl;
	struct pw_proxy *this;

	impl = calloc(1, sizeof(struct proxy));
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->context = context;
	this->type = type;

	pw_signal_init(&this->destroy_signal);

	if (id == SPA_ID_INVALID) {
		id = pw_map_insert_new(&context->objects, this);
	} else if (!pw_map_insert_at(&context->objects, id, this))
		goto in_use;

	this->id = id;

	pw_protocol_native_client_setup(this);

	spa_list_insert(&this->context->proxy_list, &this->link);

	pw_log_debug("proxy %p: new %u", this, this->id);

	return this;

      in_use:
	pw_log_error("proxy %p: id %u in use for context %p", this, id, context);
	free(impl);
	return NULL;
}

/** Destroy a proxy object
 *
 * \param proxy Proxy object to destroy
 *
 * \note This is normally called by \ref pw_context when the server
 *       decides to destroy the server side object
 * \memberof pw_proxy
 */
void pw_proxy_destroy(struct pw_proxy *proxy)
{
	struct proxy *impl = SPA_CONTAINER_OF(proxy, struct proxy, this);

	pw_log_debug("proxy %p: destroy %u", proxy, proxy->id);
	pw_signal_emit(&proxy->destroy_signal, proxy);

	pw_map_remove(&proxy->context->objects, proxy->id);
	spa_list_remove(&proxy->link);

	if (proxy->destroy)
		proxy->destroy(proxy);

	free(impl);
}
