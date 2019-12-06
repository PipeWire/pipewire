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

#include <pipewire/log.h>
#include <pipewire/proxy.h>
#include <pipewire/core.h>
#include <pipewire/private.h>
#include <pipewire/type.h>
#include <pipewire/interfaces.h>

#include <spa/debug/types.h>

#define NAME "proxy"

/** \cond */
struct proxy {
	struct pw_proxy this;
};
/** \endcond */

int pw_proxy_init(struct pw_proxy *proxy, uint32_t type, uint32_t version)
{
	int res;

	proxy->refcount = 1;
	proxy->type = type;
	proxy->version = version;

	proxy->id = pw_map_insert_new(&proxy->core_proxy->objects, proxy);
	if (proxy->id == SPA_ID_INVALID) {
		res = -errno;
		pw_log_error(NAME" %p: can't allocate new id: %m", proxy);
		goto error;
	}

	spa_hook_list_init(&proxy->listener_list);
	spa_hook_list_init(&proxy->object_listener_list);

	if ((res = pw_proxy_install_marshal(proxy, false)) < 0) {
		pw_log_error(NAME" %p: no marshal for type %s/%d", proxy,
				spa_debug_type_find_name(pw_type_info(), type),
				version);
		goto error_clean;
	}
	return 0;

error_clean:
	pw_map_remove(&proxy->core_proxy->objects, proxy->id);
error:
	return res;
}

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
 * \sa pw_core_proxy
 *
 * \memberof pw_proxy
 */
SPA_EXPORT
struct pw_proxy *pw_proxy_new(struct pw_proxy *factory,
			      uint32_t type, uint32_t version,
			      size_t user_data_size)
{
	struct proxy *impl;
	struct pw_proxy *this;
	int res;

	impl = calloc(1, sizeof(struct proxy) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	this->core_proxy = factory->core_proxy;

	if ((res = pw_proxy_init(this, type, version)) < 0)
		goto error_init;

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct proxy), void);

	pw_log_debug(NAME" %p: new %u type %s/%d core-proxy:%p, marshal:%p",
			this, this->id,
			spa_debug_type_find_name(pw_type_info(), type), version,
			this->core_proxy, this->marshal);
	return this;

error_init:
	free(impl);
	errno = -res;
	return NULL;
}

SPA_EXPORT
int pw_proxy_install_marshal(struct pw_proxy *this, bool implementor)
{
	struct pw_core_proxy *core_proxy = this->core_proxy;
	const struct pw_protocol_marshal *marshal;

	marshal = pw_protocol_get_marshal(core_proxy->conn->protocol,
			this->type, this->version,
			implementor ? PW_PROTOCOL_MARSHAL_FLAG_IMPL : 0);
	if (marshal == NULL)
		return -EPROTO;

	this->marshal = marshal;
	this->impl = SPA_INTERFACE_INIT(
			this->type,
			this->marshal->version,
			this->marshal->client_marshal, this);
	return 0;
}

SPA_EXPORT
void *pw_proxy_get_user_data(struct pw_proxy *proxy)
{
	return proxy->user_data;
}

SPA_EXPORT
uint32_t pw_proxy_get_id(struct pw_proxy *proxy)
{
	return proxy->id;
}

SPA_EXPORT
uint32_t pw_proxy_get_type(struct pw_proxy *proxy, uint32_t *version)
{
	if (version)
		*version = proxy->version;
	return proxy->type;
}

SPA_EXPORT
struct pw_core_proxy *pw_proxy_get_core_proxy(struct pw_proxy *proxy)
{
	return proxy->core_proxy;
}

SPA_EXPORT
struct pw_protocol *pw_proxy_get_protocol(struct pw_proxy *proxy)
{
	return proxy->core_proxy->conn->protocol;
}

SPA_EXPORT
void pw_proxy_add_listener(struct pw_proxy *proxy,
			   struct spa_hook *listener,
			   const struct pw_proxy_events *events,
			   void *data)
{
	spa_hook_list_append(&proxy->listener_list, listener, events, data);
}

SPA_EXPORT
void pw_proxy_add_object_listener(struct pw_proxy *proxy,
				 struct spa_hook *listener,
				 const void *funcs,
				 void *data)
{
	spa_hook_list_append(&proxy->object_listener_list, listener, funcs, data);
}

/** Destroy a proxy object
 *
 * \param proxy Proxy object to destroy
 *
 * \note This is normally called by \ref pw_core_proxy when the server
 *       decides to destroy the server side object
 * \memberof pw_proxy
 */
SPA_EXPORT
void pw_proxy_destroy(struct pw_proxy *proxy)
{
	if (!proxy->zombie) {
		pw_log_debug(NAME" %p: destroy %u", proxy, proxy->id);
		pw_proxy_emit_destroy(proxy);
	}
	if (!proxy->removed) {
		/* if the server did not remove this proxy, remove ourselves
		 * from the proxy objects and schedule a destroy. */
		if (proxy->core_proxy) {
			proxy->zombie = true;
			pw_core_proxy_destroy(proxy->core_proxy, proxy);
		} else {
			proxy->removed = true;
		}
	}
	if (proxy->removed) {
		if (proxy->core_proxy)
			pw_map_remove(&proxy->core_proxy->objects, proxy->id);

		pw_proxy_unref(proxy);
	}
}

void pw_proxy_remove(struct pw_proxy *proxy)
{
	proxy->removed = true;
	pw_proxy_destroy(proxy);
}

SPA_EXPORT
void pw_proxy_unref(struct pw_proxy *proxy)
{
	if (--proxy->refcount > 0)
		return;

	pw_log_debug(NAME" %p: free %u", proxy, proxy->id);
	free(proxy);
}

SPA_EXPORT
int pw_proxy_sync(struct pw_proxy *proxy, int seq)
{
	int res = -EIO;
	struct pw_core_proxy *core_proxy = proxy->core_proxy;

	if (core_proxy != NULL) {
		res = pw_core_proxy_sync(core_proxy, proxy->id, seq);
		pw_log_debug(NAME" %p: %u seq:%d sync %u", proxy, proxy->id, seq, res);
	}
	return res;
}

SPA_EXPORT
int pw_proxy_errorf(struct pw_proxy *proxy, int res, const char *error, ...)
{
	va_list ap;
	int r = -EIO;
	struct pw_core_proxy *core_proxy = proxy->core_proxy;

	va_start(ap, error);
	if (core_proxy != NULL)
		r = pw_core_proxy_errorv(core_proxy, proxy->id,
				core_proxy->recv_seq, res, error, ap);
	va_end(ap);
	return r;
}

SPA_EXPORT
int pw_proxy_error(struct pw_proxy *proxy, int res, const char *error)
{
	int r = -EIO;
	struct pw_core_proxy *core_proxy = proxy->core_proxy;
	if (core_proxy != NULL)
		r = pw_core_proxy_error(core_proxy, proxy->id,
				core_proxy->recv_seq, res, error);
	return r;
}

SPA_EXPORT
struct spa_hook_list *pw_proxy_get_object_listeners(struct pw_proxy *proxy)
{
	return &proxy->object_listener_list;
}

SPA_EXPORT
const struct pw_protocol_marshal *pw_proxy_get_marshal(struct pw_proxy *proxy)
{
	return proxy->marshal;
}
