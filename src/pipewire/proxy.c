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

#include <pipewire/log.h>
#include <pipewire/proxy.h>
#include <pipewire/core.h>
#include <pipewire/remote.h>
#include <pipewire/private.h>

/** \cond */
struct proxy {
	struct pw_proxy this;
};
/** \endcond */

/** Create a proxy object with a given id and type
 *
 * \param factory another proxy object that serves as a factory
 * \param type Type of the proxy object
 * \param user_data_size size of user_data
 * \return A newly allocated proxy object or NULL on failure
 *
 * This function creates a new proxy object with the supplied id and type. The
 * proxy object will have an id assigned from the client id space.
 *
 * \sa pw_remote
 *
 * \memberof pw_proxy
 */
struct pw_proxy *pw_proxy_new(struct pw_proxy *factory,
			      uint32_t type,
			      size_t user_data_size)
{
	struct proxy *impl;
	struct pw_proxy *this;
	struct pw_remote *remote = factory->remote;

	impl = calloc(1, sizeof(struct proxy) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->remote = remote;

	spa_hook_list_init(&this->listener_list);
	spa_hook_list_init(&this->proxy_listener_list);

	this->id = pw_map_insert_new(&remote->objects, this);

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct proxy), void);

	this->marshal = pw_protocol_get_marshal(remote->conn->protocol, type);

	spa_list_append(&this->remote->proxy_list, &this->link);

	pw_log_debug("proxy %p: new %u, remote %p, marshal %p", this, this->id, remote, this->marshal);

	return this;
}

void *pw_proxy_get_user_data(struct pw_proxy *proxy)
{
	return proxy->user_data;
}

uint32_t pw_proxy_get_id(struct pw_proxy *proxy)
{
	return proxy->id;
}

struct pw_protocol *pw_proxy_get_protocol(struct pw_proxy *proxy)
{
	return proxy->remote->conn->protocol;
}

void pw_proxy_add_listener(struct pw_proxy *proxy,
			   struct spa_hook *listener,
			   const struct pw_proxy_events *events,
			   void *data)
{
	spa_hook_list_append(&proxy->listener_list, listener, events, data);
}

void pw_proxy_add_proxy_listener(struct pw_proxy *proxy,
				 struct spa_hook *listener,
				 const void *events,
				 void *data)
{
	spa_hook_list_append(&proxy->proxy_listener_list, listener, events, data);
}

/** Destroy a proxy object
 *
 * \param proxy Proxy object to destroy
 *
 * \note This is normally called by \ref pw_remote when the server
 *       decides to destroy the server side object
 * \memberof pw_proxy
 */
void pw_proxy_destroy(struct pw_proxy *proxy)
{
	struct proxy *impl = SPA_CONTAINER_OF(proxy, struct proxy, this);

	pw_log_debug("proxy %p: destroy %u", proxy, proxy->id);
	spa_hook_list_call(&proxy->listener_list, struct pw_proxy_events, destroy);

	pw_map_insert_at(&proxy->remote->objects, proxy->id, NULL);
	spa_list_remove(&proxy->link);

	free(impl);
}

struct spa_hook_list *pw_proxy_get_proxy_listeners(struct pw_proxy *proxy)
{
	return &proxy->proxy_listener_list;
}

const struct pw_protocol_marshal *pw_proxy_get_marshal(struct pw_proxy *proxy)
{
	return proxy->marshal;
}
