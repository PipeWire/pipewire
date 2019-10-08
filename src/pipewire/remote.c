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

#define NAME "remote"

/** \cond */

struct remote {
	struct pw_remote this;
	struct spa_hook core_listener;
	struct spa_hook core_proxy_listener;
};

/** \endcond */

SPA_EXPORT
const char *pw_remote_state_as_string(enum pw_remote_state state)
{
	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		return "error";
	case PW_REMOTE_STATE_UNCONNECTED:
		return "unconnected";
	case PW_REMOTE_STATE_CONNECTING:
		return "connecting";
	case PW_REMOTE_STATE_CONNECTED:
		return "connected";
	}
	return "invalid-state";
}

static int
pw_remote_update_state(struct pw_remote *remote, enum pw_remote_state state, const char *fmt, ...)
{
	enum pw_remote_state old = remote->state;

	if (old != state) {
		free(remote->error);

		if (fmt) {
			va_list varargs;

			va_start(varargs, fmt);
			if (vasprintf(&remote->error, fmt, varargs) < 0) {
				pw_log_debug(NAME" %p: error formating message: %m", remote);
				remote->error = NULL;
			}
			va_end(varargs);
		} else {
			remote->error = NULL;
		}
		if (state == PW_REMOTE_STATE_ERROR) {
			pw_log_error(NAME" %p: update state from %s -> %s (%s)", remote,
				     pw_remote_state_as_string(old),
				     pw_remote_state_as_string(state), remote->error);
		} else {
			pw_log_debug(NAME" %p: update state from %s -> %s", remote,
				     pw_remote_state_as_string(old),
				     pw_remote_state_as_string(state));
		}

		remote->state = state;
		pw_remote_emit_state_changed(remote, old, state, remote->error);
	}
	return 0;
}

static void core_event_ping(void *data, uint32_t id, int seq)
{
	struct pw_remote *this = data;
	pw_log_debug(NAME" %p: object %u ping %u", this, id, seq);
	pw_core_proxy_pong(this->core_proxy, id, seq);
}

static void core_event_done(void *data, uint32_t id, int seq)
{
	struct pw_remote *this = data;
	struct pw_proxy *proxy;

	pw_log_debug(NAME" %p: object %u done %d", this, id, seq);

	proxy = pw_map_lookup(&this->objects, id);
	if (proxy)
		pw_proxy_emit_done(proxy, seq);
}

static void core_event_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct pw_remote *this = data;
	struct pw_proxy *proxy;

	pw_log_error(NAME" %p: object error %u: seq:%d %d (%s): %s", this, id, seq,
			res, spa_strerror(res), message);

	proxy = pw_map_lookup(&this->objects, id);
	if (proxy)
		pw_proxy_emit_error(proxy, seq, res, message);
}

static void core_event_remove_id(void *data, uint32_t id)
{
	struct pw_remote *this = data;
	struct pw_proxy *proxy;

	pw_log_debug(NAME" %p: object remove %u", this, id);
	if ((proxy = pw_map_lookup(&this->objects, id)) != NULL) {
		proxy->removed = true;
		pw_proxy_destroy(proxy);
	}
}

static void core_event_add_mem(void *data, uint32_t id, uint32_t type, int fd, uint32_t flags)
{
	struct pw_remote *this = data;
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
	struct pw_remote *this = data;
	pw_log_debug(NAME" %p: remove mem %u", this, id);
	pw_mempool_unref_id(this->pool, id);
}

static const struct pw_core_proxy_events core_events = {
	PW_VERSION_CORE_PROXY_EVENTS,
	.error = core_event_error,
	.ping = core_event_ping,
	.done = core_event_done,
	.remove_id = core_event_remove_id,
	.add_mem = core_event_add_mem,
	.remove_mem = core_event_remove_mem,
};

SPA_EXPORT
struct pw_remote *pw_remote_new(struct pw_core *core,
				struct pw_properties *properties,
				size_t user_data_size)
{
	struct remote *impl;
	struct pw_remote *this;
	struct pw_protocol *protocol;
	const char *protocol_name;
	int res;

	impl = calloc(1, sizeof(struct remote) + user_data_size);
	if (impl == NULL) {
		res = -errno;
		goto exit_cleanup;
	}

	this = &impl->this;
	pw_log_debug(NAME" %p: new", impl);

	this->core = core;

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct remote), void);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto error_properties;

	pw_fill_remote_properties(core, properties);
	this->properties = properties;

	this->state = PW_REMOTE_STATE_UNCONNECTED;

	pw_map_init(&this->objects, 64, 32);

	spa_list_init(&this->stream_list);

	spa_hook_list_init(&this->listener_list);

	if ((protocol_name = pw_properties_get(properties, PW_KEY_PROTOCOL)) == NULL) {
		if (pw_module_load(core, "libpipewire-module-protocol-native",
					NULL, NULL) == NULL) {
			res = -errno;
			goto error_protocol;
		}

		protocol_name = PW_TYPE_INFO_PROTOCOL_Native;
	}

	protocol = pw_core_find_protocol(core, protocol_name);
	if (protocol == NULL) {
		res = -ENOTSUP;
		goto error_protocol;
	}

	this->conn = pw_protocol_new_client(protocol, this, properties);
	if (this->conn == NULL)
		goto error_connection;

	pw_module_load(core, "libpipewire-module-rtkit", NULL, NULL);
	pw_module_load(core, "libpipewire-module-client-node", NULL, NULL);
	pw_module_load(core, "libpipewire-module-adapter", NULL, NULL);

        spa_list_append(&core->remote_list, &this->link);

	return this;

error_properties:
	res = -errno;
	pw_log_error(NAME" %p: can't create properties: %m", this);
	goto exit_free;
error_protocol:
	pw_log_error(NAME" %p: can't load native protocol: %s", this, spa_strerror(res));
	goto exit_free;
error_connection:
	res = -errno;
	pw_log_error(NAME" %p: can't create new native protocol connection: %m", this);
	goto exit_free;

exit_free:
	free(impl);
exit_cleanup:
	if (properties)
		pw_properties_free(properties);
	errno = -res;
	return NULL;
}

SPA_EXPORT
void pw_remote_destroy(struct pw_remote *remote)
{
	struct remote *impl = SPA_CONTAINER_OF(remote, struct remote, this);
	struct pw_stream *stream;

	pw_log_debug(NAME" %p: destroy", remote);
	pw_remote_emit_destroy(remote);

	if (remote->state != PW_REMOTE_STATE_UNCONNECTED)
		pw_remote_disconnect(remote);

	spa_list_consume(stream, &remote->stream_list, link)
		pw_stream_destroy(stream);

	pw_protocol_client_destroy(remote->conn);

	spa_list_remove(&remote->link);

	pw_map_clear(&remote->objects);

	pw_log_debug(NAME" %p: free", remote);
	pw_properties_free(remote->properties);
	free(remote->error);
	free(impl);
}

SPA_EXPORT
struct pw_core *pw_remote_get_core(struct pw_remote *remote)
{
	return remote->core;
}

SPA_EXPORT
const struct pw_properties *pw_remote_get_properties(struct pw_remote *remote)
{
	return remote->properties;
}

SPA_EXPORT
int pw_remote_update_properties(struct pw_remote *remote, const struct spa_dict *dict)
{
	int changed;

	changed = pw_properties_update(remote->properties, dict);

	pw_log_debug(NAME" %p: updated %d properties", remote, changed);

	if (!changed)
		return 0;

	if (remote->client_proxy)
		pw_client_proxy_update_properties(remote->client_proxy, &remote->properties->dict);

	return changed;
}

SPA_EXPORT
void *pw_remote_get_user_data(struct pw_remote *remote)
{
	return remote->user_data;
}

SPA_EXPORT
enum pw_remote_state pw_remote_get_state(struct pw_remote *remote, const char **error)
{
	if (error)
		*error = remote->error;
	return remote->state;
}

SPA_EXPORT
void pw_remote_add_listener(struct pw_remote *remote,
			    struct spa_hook *listener,
			    const struct pw_remote_events *events,
			    void *data)
{
	spa_hook_list_append(&remote->listener_list, listener, events, data);
}

static void core_proxy_destroy(void *data)
{
	struct pw_remote *remote = data;
	struct remote *impl = SPA_CONTAINER_OF(remote, struct remote, this);

	pw_log_debug(NAME" %p: core proxy destroy", remote);
	if (remote->core_proxy) {
		spa_hook_remove(&impl->core_proxy_listener);
		spa_hook_remove(&impl->core_listener);
		remote->core_proxy = NULL;
		pw_mempool_destroy(remote->pool);
		remote->pool = NULL;
	}
}


static const struct pw_proxy_events core_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = core_proxy_destroy,
};

static int
do_connect(struct spa_loop *loop,
		bool async, uint32_t seq, const void *data, size_t size, void *user_data)
{
	struct pw_remote *remote = user_data;
	struct remote *impl = SPA_CONTAINER_OF(remote, struct remote, this);
	struct pw_proxy dummy, *core_proxy;
	int res;

	spa_zero(dummy);
	dummy.remote = remote;

	core_proxy = pw_proxy_new(&dummy, PW_TYPE_INTERFACE_Core, PW_VERSION_CORE_PROXY, 0);
	if (core_proxy == NULL) {
		res = -errno;
		goto error_disconnect;
	}
	remote->core_proxy = (struct pw_core_proxy*)core_proxy;

	remote->client_proxy = (struct pw_client_proxy*)pw_proxy_new(&dummy,
			PW_TYPE_INTERFACE_Client, PW_VERSION_CLIENT_PROXY, 0);
	if (remote->client_proxy == NULL) {
		res = -errno;
		goto error_clean_core_proxy;
	}
	remote->pool = pw_mempool_new(NULL);

	pw_core_proxy_add_listener(remote->core_proxy, &impl->core_listener, &core_events, remote);
	pw_proxy_add_listener(core_proxy, &impl->core_proxy_listener, &core_proxy_events, remote);

	pw_core_proxy_hello(remote->core_proxy, PW_VERSION_CORE_PROXY);
	pw_client_proxy_update_properties(remote->client_proxy, &remote->properties->dict);
	pw_remote_update_state(remote, PW_REMOTE_STATE_CONNECTED, NULL);

	return 0;

error_clean_core_proxy:
	((struct pw_proxy*)remote->core_proxy)->removed = true;
	pw_proxy_destroy((struct pw_proxy*)remote->core_proxy);
error_disconnect:
	pw_protocol_client_disconnect(remote->conn);
	pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR, "can't connect: %s", spa_strerror(res));
	return res;
}

SPA_EXPORT
struct pw_core_proxy * pw_remote_get_core_proxy(struct pw_remote *remote)
{
	return remote->core_proxy;
}

SPA_EXPORT
struct pw_client_proxy * pw_remote_get_client_proxy(struct pw_remote *remote)
{
	return remote->client_proxy;
}

SPA_EXPORT
struct pw_proxy *pw_remote_find_proxy(struct pw_remote *remote, uint32_t id)
{
	return pw_map_lookup(&remote->objects, id);
}

static void done_connect(void *data, int result)
{
	struct pw_remote *remote = data;
	if (result < 0) {
		pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR, "can't connect: %s",
				spa_strerror(result));
		return;
	}
	pw_loop_invoke(remote->core->main_loop,
			do_connect, 0, NULL, 0, false, remote);
}

SPA_EXPORT
int pw_remote_connect(struct pw_remote *remote)
{
	int res;

	pw_remote_update_state(remote, PW_REMOTE_STATE_CONNECTING, NULL);

	if ((res = pw_protocol_client_connect(remote->conn, done_connect, remote)) < 0) {
		pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR,
				"connect failed %s", spa_strerror(res));
		return res;
	}
	return remote->state == PW_REMOTE_STATE_ERROR ? -EIO : 0;
}

SPA_EXPORT
int pw_remote_connect_fd(struct pw_remote *remote, int fd)
{
	int res;

	pw_remote_update_state(remote, PW_REMOTE_STATE_CONNECTING, NULL);

	if ((res = pw_protocol_client_connect_fd(remote->conn, fd, true)) < 0) {
		pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR,
				"connect_fd failed %s", spa_strerror(res));
		return res;
	}
	pw_loop_invoke(remote->core->main_loop,
			do_connect, 0, NULL, 0, false, remote);

	return remote->state == PW_REMOTE_STATE_ERROR ? -EIO : 0;
}

SPA_EXPORT
int pw_remote_steal_fd(struct pw_remote *remote)
{
	int fd;

	fd = pw_protocol_client_steal_fd(remote->conn);
	if (fd >= 0)
		pw_remote_update_state(remote, PW_REMOTE_STATE_UNCONNECTED, NULL);
	return fd;
}

static int destroy_proxy(void *object, void *data)
{
	if (object)
		pw_proxy_destroy(object);
	return 0;
}

SPA_EXPORT
int pw_remote_disconnect(struct pw_remote *remote)
{
	struct pw_stream *stream, *s2;

	pw_log_debug(NAME" %p: disconnect", remote);

	spa_list_for_each_safe(stream, s2, &remote->stream_list, link)
		pw_stream_disconnect(stream);

	pw_protocol_client_disconnect(remote->conn);

	remote->client_proxy = NULL;

	pw_remote_update_state(remote, PW_REMOTE_STATE_UNCONNECTED, NULL);

	pw_map_for_each(&remote->objects, destroy_proxy, remote);

	pw_map_reset(&remote->objects);

	return 0;
}

SPA_EXPORT
struct pw_proxy *pw_remote_export(struct pw_remote *remote,
		uint32_t type, struct pw_properties *props, void *object,
		size_t user_data_size)
{
	struct pw_proxy *proxy;
	const struct pw_export_type *t;
	int res;

	if (remote->core_proxy == NULL) {
		res = -ENETDOWN;
		goto error_core_proxy;
	}

	t = pw_core_find_export_type(remote->core, type);
	if (t == NULL) {
		res = -EPROTO;
		goto error_export_type;
	}

	proxy = t->func(remote, type, props, object, user_data_size);
        if (proxy == NULL) {
		res = -errno;
		goto error_proxy_failed;
	}
	return proxy;

error_core_proxy:
	pw_log_error(NAME" %p: no core proxy: %s", remote, spa_strerror(res));
	goto exit_free;
error_export_type:
	pw_log_error(NAME" %p: can't export type %d: %s", remote, type, spa_strerror(res));
	goto exit_free;
error_proxy_failed:
	pw_log_error(NAME" %p: failed to create proxy: %s", remote, spa_strerror(res));
	goto exit;
exit_free:
	if (props)
		pw_properties_free(props);
exit:
	errno = -res;
	return NULL;
}

SPA_EXPORT
int pw_core_register_export_type(struct pw_core *core, struct pw_export_type *type)
{
	pw_log_debug("core %p: Add export type %d/%s to core", core, type->type,
			spa_debug_type_find_name(pw_type_info(), type->type));
	spa_list_append(&core->export_list, &type->link);
	return 0;
}

const struct pw_export_type *pw_core_find_export_type(struct pw_core *core, uint32_t type)
{
	const struct pw_export_type *t;
	spa_list_for_each(t, &core->export_list, link) {
		if (t->type == type)
			return t;
	}
	return NULL;
}
