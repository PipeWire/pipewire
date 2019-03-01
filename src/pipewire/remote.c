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

/** \cond */

struct remote {
	struct pw_remote this;
	struct spa_hook core_listener;
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
				pw_log_debug("remote %p: error formating message: %m", remote);
				remote->error = NULL;
			}
			va_end(varargs);
		} else {
			remote->error = NULL;
		}
		if (state == PW_REMOTE_STATE_ERROR) {
			pw_log_error("remote %p: update state from %s -> %s (%s)", remote,
				     pw_remote_state_as_string(old),
				     pw_remote_state_as_string(state), remote->error);
		} else {
			pw_log_debug("remote %p: update state from %s -> %s", remote,
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
	pw_log_debug("remote %p: object %u ping %u", this, id, seq);
	pw_core_proxy_pong(this->core_proxy, id, seq);
}

static void core_event_done(void *data, uint32_t id, int seq)
{
	struct pw_remote *this = data;
	struct pw_proxy *proxy;

	pw_log_debug("remote %p: object %u done %d", this, id, seq);

	proxy = pw_map_lookup(&this->objects, id);
	if (proxy)
		pw_proxy_emit_done(proxy, seq);
}

static void core_event_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct pw_remote *this = data;
	struct pw_proxy *proxy;

	pw_log_error("remote %p: object error %u: seq:%d %d (%s): %s", this, id, seq,
			res, spa_strerror(res), message);

	proxy = pw_map_lookup(&this->objects, id);
	if (proxy)
		pw_proxy_emit_error(proxy, seq, res, message);
}

static void core_event_remove_id(void *data, uint32_t id)
{
	struct pw_remote *this = data;
	struct pw_proxy *proxy;

	pw_log_debug("remote %p: object remove %u", this, id);
	proxy = pw_map_lookup(&this->objects, id);
	if (proxy)
		pw_proxy_destroy(proxy);

	pw_map_remove(&this->objects, id);
}

static const struct pw_core_proxy_events core_proxy_events = {
	PW_VERSION_CORE_PROXY_EVENTS,
	.error = core_event_error,
	.ping = core_event_ping,
	.done = core_event_done,
	.remove_id = core_event_remove_id,
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

	impl = calloc(1, sizeof(struct remote) + user_data_size);
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	pw_log_debug("remote %p: new", impl);

	this->core = core;

	if (user_data_size > 0)
		this->user_data = SPA_MEMBER(impl, sizeof(struct remote), void);

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto no_mem;

	pw_fill_remote_properties(core, properties);
	this->properties = properties;

	this->state = PW_REMOTE_STATE_UNCONNECTED;

	pw_map_init(&this->objects, 64, 32);

	spa_list_init(&this->proxy_list);
	spa_list_init(&this->stream_list);

	spa_hook_list_init(&this->listener_list);

	if ((protocol_name = pw_properties_get(properties, PW_REMOTE_PROP_PROTOCOL)) == NULL) {
		if (!pw_module_load(core, "libpipewire-module-protocol-native", NULL, NULL, NULL, NULL))
			goto no_protocol;

		protocol_name = PW_TYPE_INFO_PROTOCOL_Native;
	}

	protocol = pw_core_find_protocol(core, protocol_name);
	if (protocol == NULL)
		goto no_protocol;

	this->conn = pw_protocol_new_client(protocol, this, properties);
	if (this->conn == NULL)
		goto no_connection;

	pw_module_load(core, "libpipewire-module-rtkit", NULL, NULL, NULL, NULL);
	pw_module_load(core, "libpipewire-module-client-node", NULL, NULL, NULL, NULL);

        spa_list_append(&core->remote_list, &this->link);

	return this;

      no_mem:
	pw_log_error("no memory");
	goto exit;
      no_protocol:
	pw_log_error("can't load native protocol");
	goto exit_free_props;
      no_connection:
	pw_log_error("can't create new native protocol connection");
	goto exit_free_props;

      exit_free_props:
	pw_properties_free(properties);
      exit:
	free(impl);
	return NULL;
}

SPA_EXPORT
void pw_remote_destroy(struct pw_remote *remote)
{
	struct remote *impl = SPA_CONTAINER_OF(remote, struct remote, this);
	struct pw_stream *stream;

	pw_log_debug("remote %p: destroy", remote);
	pw_remote_emit_destroy(remote);

	if (remote->state != PW_REMOTE_STATE_UNCONNECTED)
		pw_remote_disconnect(remote);

	spa_list_consume(stream, &remote->stream_list, link)
		pw_stream_destroy(stream);

	pw_protocol_client_destroy (remote->conn);

	spa_list_remove(&remote->link);

	pw_map_clear(&remote->objects);

	pw_log_debug("remote %p: free", remote);
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

	pw_log_debug("remote %p: updated %d properties", remote, changed);

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

static int do_connect(struct pw_remote *remote)
{
	struct remote *impl = SPA_CONTAINER_OF(remote, struct remote, this);
	struct pw_proxy dummy;

	dummy.remote = remote;

	remote->core_proxy = (struct pw_core_proxy*)pw_proxy_new(&dummy,
			PW_TYPE_INTERFACE_Core, 0);
	if (remote->core_proxy == NULL)
		goto no_proxy;

	remote->client_proxy = (struct pw_client_proxy*)pw_proxy_new(&dummy,
			PW_TYPE_INTERFACE_Client, 0);
	if (remote->client_proxy == NULL)
		goto clean_core_proxy;

	pw_core_proxy_add_listener(remote->core_proxy, &impl->core_listener, &core_proxy_events, remote);

	pw_client_proxy_update_properties(remote->client_proxy, &remote->properties->dict);
	pw_core_proxy_hello(remote->core_proxy, PW_VERSION_CORE);
	pw_remote_update_state(remote, PW_REMOTE_STATE_CONNECTED, NULL);

	return 0;

      clean_core_proxy:
	pw_proxy_destroy((struct pw_proxy*)remote->core_proxy);
      no_proxy:
	pw_protocol_client_disconnect(remote->conn);
	pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR, "can't connect: no memory");
	return -ENOMEM;
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

	do_connect(remote);
}

SPA_EXPORT
int pw_remote_connect(struct pw_remote *remote)
{
	int res;

	pw_remote_update_state(remote, PW_REMOTE_STATE_CONNECTING, NULL);

	if ((res = pw_protocol_client_connect (remote->conn, done_connect, remote)) < 0) {
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

	if ((res = pw_protocol_client_connect_fd (remote->conn, fd)) < 0) {
		pw_remote_update_state(remote, PW_REMOTE_STATE_ERROR,
				"connect_fd failed %s", spa_strerror(res));
		return res;
	}

	return do_connect(remote);
}

SPA_EXPORT
int pw_remote_steal_fd(struct pw_remote *remote)
{
	int fd;

	fd = pw_protocol_client_steal_fd(remote->conn);
	pw_remote_update_state(remote, PW_REMOTE_STATE_UNCONNECTED, NULL);

	return fd;
}

SPA_EXPORT
int pw_remote_disconnect(struct pw_remote *remote)
{
	struct pw_proxy *proxy;
	struct pw_stream *stream, *s2;

	pw_log_debug("remote %p: disconnect", remote);
	spa_list_for_each_safe(stream, s2, &remote->stream_list, link)
		pw_stream_disconnect(stream);

	pw_protocol_client_disconnect (remote->conn);

	remote->core_proxy = NULL;
	remote->client_proxy = NULL;

        pw_remote_update_state(remote, PW_REMOTE_STATE_UNCONNECTED, NULL);

	spa_list_consume(proxy, &remote->proxy_list, link)
		pw_proxy_destroy(proxy);

	pw_map_reset(&remote->objects);

	return 0;
}

SPA_EXPORT
struct pw_proxy *pw_remote_export(struct pw_remote *remote,
		uint32_t type, struct pw_properties *props, void *object)
{
	struct pw_proxy *proxy;
	const struct pw_export_type *t;

	if (remote->core_proxy == NULL)
		goto no_core_proxy;

	t = pw_core_find_export_type(remote->core, type);
	if (t == NULL)
		goto no_export_type;

	proxy = t->func(remote, type, props, object);
        if (proxy == NULL)
		goto proxy_failed;

	return proxy;

    no_core_proxy:
	errno = ENETDOWN;
	pw_log_error("no core proxy: %m");
	return NULL;
    no_export_type:
	pw_log_error("can't export type %d: %m", type);
	return NULL;
    proxy_failed:
	pw_log_error("failed to create proxy: %m");
	return NULL;
}

SPA_EXPORT
int pw_core_register_export_type(struct pw_core *core, struct pw_export_type *type)
{
	pw_log_debug("Add export type %d/%s to core", type->type,
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
	errno = EPROTO;
	return NULL;
}
