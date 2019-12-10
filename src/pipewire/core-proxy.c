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

#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <sys/mman.h>

#include <spa/pod/parser.h>
#include <spa/debug/types.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"

#include "extensions/protocol-native.h"

#define NAME "core-proxy"

/** \cond */

/** \endcond */
static void core_event_ping(void *data, uint32_t id, int seq)
{
	struct pw_core_proxy *this = data;
	pw_log_debug(NAME" %p: object %u ping %u", this, id, seq);
	pw_core_proxy_pong(this->core_proxy, id, seq);
}

static void core_event_done(void *data, uint32_t id, int seq)
{
	struct pw_core_proxy *this = data;
	struct pw_proxy *proxy;

	pw_log_trace(NAME" %p: object %u done %d", this, id, seq);

	proxy = pw_map_lookup(&this->objects, id);
	if (proxy)
		pw_proxy_emit_done(proxy, seq);
}

static void core_event_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct pw_core_proxy *this = data;
	struct pw_proxy *proxy;

	pw_log_error(NAME" %p: object error %u: seq:%d %d (%s): %s", this, id, seq,
			res, spa_strerror(res), message);

	proxy = pw_map_lookup(&this->objects, id);
	if (proxy)
		pw_proxy_emit_error(proxy, seq, res, message);
}

static void core_event_remove_id(void *data, uint32_t id)
{
	struct pw_core_proxy *this = data;
	struct pw_proxy *proxy;

	pw_log_debug(NAME" %p: object remove %u", this, id);
	if ((proxy = pw_map_lookup(&this->objects, id)) != NULL)
		pw_proxy_remove(proxy);
}

static void core_event_bound_id(void *data, uint32_t id, uint32_t global_id)
{
	struct pw_core_proxy *this = data;
	struct pw_proxy *proxy;

	pw_log_debug(NAME" %p: proxy %u bound %u", this, id, global_id);
	if ((proxy = pw_map_lookup(&this->objects, id)) != NULL) {
		pw_proxy_set_bound_id(proxy, global_id);
	}
}

static void core_event_add_mem(void *data, uint32_t id, uint32_t type, int fd, uint32_t flags)
{
	struct pw_core_proxy *this = data;
	struct pw_memblock *m;

	pw_log_debug(NAME" %p: add mem %u type:%u fd:%d flags:%u", this, id, type, fd, flags);

	m = pw_mempool_import(this->pool, flags, type, fd);
	if (m->id != id) {
		pw_log_error(NAME" %p: invalid mem id %u, expected %u",
				this, id, m->id);
		pw_memblock_unref(m);
	}
}

static void core_event_remove_mem(void *data, uint32_t id)
{
	struct pw_core_proxy *this = data;
	pw_log_debug(NAME" %p: remove mem %u", this, id);
	pw_mempool_unref_id(this->pool, id);
}

static const struct pw_core_proxy_events core_events = {
	PW_VERSION_CORE_PROXY_EVENTS,
	.error = core_event_error,
	.ping = core_event_ping,
	.done = core_event_done,
	.remove_id = core_event_remove_id,
	.bound_id = core_event_bound_id,
	.add_mem = core_event_add_mem,
	.remove_mem = core_event_remove_mem,
};

SPA_EXPORT
struct pw_core *pw_core_proxy_get_core(struct pw_core_proxy *core_proxy)
{
	return core_proxy->core;
}

SPA_EXPORT
const struct pw_properties *pw_core_proxy_get_properties(struct pw_core_proxy *core_proxy)
{
	return core_proxy->properties;
}

SPA_EXPORT
int pw_core_proxy_update_properties(struct pw_core_proxy *core_proxy, const struct spa_dict *dict)
{
	int changed;

	changed = pw_properties_update(core_proxy->properties, dict);

	pw_log_debug(NAME" %p: updated %d properties", core_proxy, changed);

	if (!changed)
		return 0;

	if (core_proxy->client_proxy)
		pw_client_proxy_update_properties(core_proxy->client_proxy, &core_proxy->properties->dict);

	return changed;
}

SPA_EXPORT
void *pw_core_proxy_get_user_data(struct pw_core_proxy *core_proxy)
{
	return core_proxy->user_data;
}

static int destroy_proxy(void *object, void *data)
{
	struct pw_core_proxy *core_proxy = data;
	struct pw_proxy *p = object;

	if (object == NULL)
		return 0;

	p->core_proxy = NULL;
	if (object != core_proxy)
		pw_proxy_remove(p);

	return 0;
}

static void core_proxy_destroy(void *data)
{
	struct pw_core_proxy *core_proxy = data;
	struct pw_stream *stream, *s2;
	struct pw_filter *filter, *f2;

	if (core_proxy->destroyed)
		return;

	core_proxy->destroyed = true;

	pw_log_debug(NAME" %p: core proxy destroy", core_proxy);
	spa_list_remove(&core_proxy->link);

	spa_list_for_each_safe(stream, s2, &core_proxy->stream_list, link)
		pw_stream_disconnect(stream);
	spa_list_for_each_safe(filter, f2, &core_proxy->filter_list, link)
		pw_filter_disconnect(filter);

	pw_protocol_client_disconnect(core_proxy->conn);
	core_proxy->client_proxy = NULL;

	pw_map_for_each(&core_proxy->objects, destroy_proxy, core_proxy);
	pw_map_reset(&core_proxy->objects);

	spa_list_consume(stream, &core_proxy->stream_list, link)
		pw_stream_destroy(stream);
	spa_list_consume(filter, &core_proxy->filter_list, link)
		pw_filter_destroy(filter);

	pw_mempool_destroy(core_proxy->pool);

	pw_protocol_client_destroy(core_proxy->conn);

	pw_map_clear(&core_proxy->objects);

	pw_log_debug(NAME" %p: free", core_proxy);
	pw_properties_free(core_proxy->properties);
}

static const struct pw_proxy_events core_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = core_proxy_destroy,
};

SPA_EXPORT
struct pw_client_proxy * pw_core_proxy_get_client_proxy(struct pw_core_proxy *core_proxy)
{
	return core_proxy->client_proxy;
}

SPA_EXPORT
struct pw_proxy *pw_core_proxy_find_proxy(struct pw_core_proxy *core_proxy, uint32_t id)
{
	return pw_map_lookup(&core_proxy->objects, id);
}

SPA_EXPORT
struct pw_proxy *pw_core_proxy_export(struct pw_core_proxy *core_proxy,
		uint32_t type, struct pw_properties *props, void *object,
		size_t user_data_size)
{
	struct pw_proxy *proxy;
	const struct pw_export_type *t;
	int res;

	t = pw_core_find_export_type(core_proxy->core, type);
	if (t == NULL) {
		res = -EPROTO;
		goto error_export_type;
	}

	proxy = t->func(core_proxy, type, props, object, user_data_size);
        if (proxy == NULL) {
		res = -errno;
		goto error_proxy_failed;
	}
	return proxy;

error_export_type:
	pw_log_error(NAME" %p: can't export type %d: %s", core_proxy, type, spa_strerror(res));
	goto exit_free;
error_proxy_failed:
	pw_log_error(NAME" %p: failed to create proxy: %s", core_proxy, spa_strerror(res));
	goto exit;
exit_free:
	if (props)
		pw_properties_free(props);
exit:
	errno = -res;
	return NULL;
}

static struct pw_core_proxy *core_proxy_new(struct pw_core *core,
		struct pw_properties *properties, size_t user_data_size)
{
	struct pw_core_proxy *p;
	struct pw_protocol *protocol = NULL;
	const char *protocol_name;
	int res;

	p = calloc(1, sizeof(struct pw_core_proxy) + user_data_size);
	if (p == NULL) {
		res = -errno;
		goto exit_cleanup;
	}
	pw_log_debug(NAME" %p: new", p);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto error_properties;

	pw_fill_connect_properties(core, properties);

	p->proxy.core_proxy = p;
	p->core = core;
	p->properties = properties;
	p->pool = pw_mempool_new(NULL);
	p->core_proxy = p;
	if (user_data_size > 0)
		p->user_data = SPA_MEMBER(p, sizeof(struct pw_core_proxy), void);
	p->proxy.user_data = p->user_data;

	pw_map_init(&p->objects, 64, 32);
	spa_list_init(&p->stream_list);
	spa_list_init(&p->filter_list);

	if ((protocol_name = pw_properties_get(properties, PW_KEY_PROTOCOL)) == NULL) {
		if ((protocol_name = pw_properties_get(core->properties, PW_KEY_PROTOCOL)) == NULL) {
			protocol_name = PW_TYPE_INFO_PROTOCOL_Native;
			if ((protocol = pw_core_find_protocol(core, protocol_name)) == NULL) {
				res = -ENOTSUP;
				goto error_protocol;
			}
		}
	}

	if (protocol == NULL)
		protocol = pw_core_find_protocol(core, protocol_name);
	if (protocol == NULL) {
		res = -ENOTSUP;
		goto error_protocol;
	}

	p->conn = pw_protocol_new_client(protocol, properties);
	if (p->conn == NULL)
		goto error_connection;

	p->conn->core_proxy = p;

	if ((res = pw_proxy_init(&p->proxy, PW_TYPE_INTERFACE_Core, PW_VERSION_CORE_PROXY)) < 0)
		goto error_proxy;

	p->client_proxy = (struct pw_client_proxy*)pw_proxy_new(&p->proxy,
			PW_TYPE_INTERFACE_Client, PW_VERSION_CLIENT_PROXY, 0);
	if (p->client_proxy == NULL) {
		res = -errno;
		goto error_proxy;
	}

	pw_core_proxy_add_listener(p, &p->core_listener, &core_events, p);
	pw_proxy_add_listener(&p->proxy, &p->core_proxy_listener, &core_proxy_events, p);

	pw_core_proxy_hello(p, PW_VERSION_CORE_PROXY);
	pw_client_proxy_update_properties(p->client_proxy, &p->properties->dict);

	spa_list_append(&core->core_proxy_list, &p->link);

	return p;

error_properties:
	res = -errno;
	pw_log_error(NAME" %p: can't create properties: %m", p);
	goto exit_free;
error_protocol:
	pw_log_error(NAME" %p: can't find native protocol: %s", p, spa_strerror(res));
	goto exit_free;
error_connection:
	res = -errno;
	pw_log_error(NAME" %p: can't create new native protocol connection: %m", p);
	goto exit_free;
error_proxy:
	pw_log_error(NAME" %p: can't initialize proxy: %s", p, spa_strerror(res));
	goto exit_free;

exit_free:
	free(p);
exit_cleanup:
	if (properties)
		pw_properties_free(properties);
	errno = -res;
	return NULL;
}

SPA_EXPORT
struct pw_core_proxy *
pw_core_connect(struct pw_core *core, struct pw_properties *properties,
	      size_t user_data_size)
{
	struct pw_core_proxy *core_proxy;
	int res;

	core_proxy = core_proxy_new(core, properties, user_data_size);
	if (core_proxy == NULL)
		return NULL;

	if ((res = pw_protocol_client_connect(core_proxy->conn,
					&core_proxy->properties->dict,
					NULL, NULL)) < 0)
		goto error_free;

	return core_proxy;

error_free:
	pw_core_proxy_disconnect(core_proxy);
	errno = -res;
	return NULL;
}

SPA_EXPORT
struct pw_core_proxy *
pw_core_connect_fd(struct pw_core *core, int fd, struct pw_properties *properties,
	      size_t user_data_size)
{
	struct pw_core_proxy *core_proxy;
	int res;

	core_proxy = core_proxy_new(core, properties, user_data_size);
	if (core_proxy == NULL)
		return NULL;

	if ((res = pw_protocol_client_connect_fd(core_proxy->conn, fd, true)) < 0)
		goto error_free;

	return core_proxy;

error_free:
	pw_core_proxy_disconnect(core_proxy);
	errno = -res;
	return NULL;
}

SPA_EXPORT
struct pw_core_proxy *
pw_core_connect_self(struct pw_core *core, struct pw_properties *properties,
	      size_t user_data_size)
{
	const struct pw_core_info *info;

	if (properties == NULL)
                properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		return NULL;

	info = pw_core_get_info(core);
	pw_properties_set(properties, PW_KEY_REMOTE_NAME, info->name);

	return pw_core_connect(core, properties, user_data_size);
}

SPA_EXPORT
int pw_core_proxy_steal_fd(struct pw_core_proxy *proxy)
{
	return pw_protocol_client_steal_fd(proxy->conn);
}

SPA_EXPORT
struct pw_mempool * pw_core_proxy_get_mempool(struct pw_core_proxy *proxy)
{
	return proxy->pool;
}

SPA_EXPORT
int pw_core_proxy_disconnect(struct pw_core_proxy *proxy)
{
	pw_proxy_destroy(&proxy->proxy);
	return 0;
}
