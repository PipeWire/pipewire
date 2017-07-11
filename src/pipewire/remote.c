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

#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

#include "pipewire/pipewire.h"
#include "pipewire/introspect.h"
#include "pipewire/interfaces.h"
#include "pipewire/remote.h"
#include "pipewire/core.h"
#include "pipewire/module.h"

#include <spa/lib/debug.h>

/** \cond */
struct remote {
	struct pw_remote this;
};
/** \endcond */

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

static void
remote_update_state(struct pw_remote *remote, enum pw_remote_state state, const char *fmt, ...)
{
	if (remote->state != state) {
		if (remote->error)
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
		pw_log_debug("remote %p: update state from %s -> %s (%s)", remote,
			     pw_remote_state_as_string(remote->state),
			     pw_remote_state_as_string(state), remote->error);

		remote->state = state;
		pw_signal_emit(&remote->state_changed, remote);
	}
}

static void core_event_info(void *object, struct pw_core_info *info)
{
	struct pw_proxy *proxy = object;
	struct pw_remote *this = proxy->remote;

	pw_log_debug("got core info");
	this->info = pw_core_info_update(this->info, info);
	pw_signal_emit(&this->info_changed, this);
}

static void core_event_done(void *object, uint32_t seq)
{
	struct pw_proxy *proxy = object;
	struct pw_remote *this = proxy->remote;

	pw_log_debug("core event done %d", seq);
	if (seq == 0)
		remote_update_state(this, PW_REMOTE_STATE_CONNECTED, NULL);

	pw_signal_emit(&this->sync_reply, this, seq);
}

static void core_event_error(void *object, uint32_t id, int res, const char *error, ...)
{
	struct pw_proxy *proxy = object;
	struct pw_remote *this = proxy->remote;
	remote_update_state(this, PW_REMOTE_STATE_ERROR, error);
}

static void core_event_remove_id(void *object, uint32_t id)
{
	struct pw_proxy *core_proxy = object;
	struct pw_remote *this = core_proxy->remote;
	struct pw_proxy *proxy;

	proxy = pw_map_lookup(&this->objects, id);
	if (proxy) {
		pw_log_debug("remote %p: object remove %u", this, id);
		pw_proxy_destroy(proxy);
	}
}

static void
core_event_update_types(void *object, uint32_t first_id, uint32_t n_types, const char **types)
{
	struct pw_proxy *proxy = object;
	struct pw_remote *this = proxy->remote;
	int i;

	for (i = 0; i < n_types; i++, first_id++) {
		uint32_t this_id = spa_type_map_get_id(this->core->type.map, types[i]);
		if (!pw_map_insert_at(&this->types, first_id, PW_MAP_ID_TO_PTR(this_id)))
			pw_log_error("can't add type for client");
	}
}

static const struct pw_core_events core_events = {
	&core_event_update_types,
	&core_event_done,
	&core_event_error,
	&core_event_remove_id,
	&core_event_info,
};

struct pw_remote *pw_remote_new(struct pw_core *core,
				struct pw_properties *properties)
{
	struct remote *impl;
	struct pw_remote *this;

	impl = calloc(1, sizeof(struct remote));
	if (impl == NULL)
		return NULL;

	this = &impl->this;
	pw_log_debug("remote %p: new", impl);

	this->core = core;

	if (properties == NULL)
		properties = pw_properties_new(NULL, NULL);
	if (properties == NULL)
		goto no_mem;

	pw_fill_remote_properties(core, properties);
	this->properties = properties;

	this->state = PW_REMOTE_STATE_UNCONNECTED;

	pw_map_init(&this->objects, 64, 32);
	pw_map_init(&this->types, 64, 32);

	spa_list_init(&this->proxy_list);
	spa_list_init(&this->stream_list);

	pw_signal_init(&this->info_changed);
	pw_signal_init(&this->sync_reply);
	pw_signal_init(&this->state_changed);
	pw_signal_init(&this->destroy_signal);

	pw_module_load(core, "libpipewire-module-protocol-native", NULL, NULL);
	pw_module_load(core, "libpipewire-module-client-node", NULL, NULL);
	this->protocol = pw_protocol_get(PW_TYPE_PROTOCOL__Native);
	if (this->protocol == NULL || this->protocol->new_connection == NULL)
		goto no_protocol;

	this->conn = this->protocol->new_connection(this->protocol, this, properties);
	if (this->conn == NULL)
		goto no_connection;

        spa_list_insert(core->remote_list.prev, &this->link);

	return this;

      no_connection:
      no_protocol:
	pw_properties_free(properties);
      no_mem:
	free(impl);
	return NULL;
}

void pw_remote_destroy(struct pw_remote *remote)
{
	struct remote *impl = SPA_CONTAINER_OF(remote, struct remote, this);

	pw_log_debug("remote %p: destroy", remote);
	pw_signal_emit(&remote->destroy_signal, remote);

	if (remote->state != PW_REMOTE_STATE_UNCONNECTED)
		pw_remote_disconnect(remote);

	remote->conn->destroy(remote->conn);

	spa_list_remove(&remote->link);

	if (remote->properties)
		pw_properties_free(remote->properties);
	free(remote->error);
	free(impl);
}

static int do_connect(struct pw_remote *remote)
{
	remote->core_proxy = pw_proxy_new(remote, 0, remote->core->type.core, 0);
	if (remote->core_proxy == NULL)
		goto no_proxy;

	pw_proxy_set_implementation(remote->core_proxy, remote, PW_VERSION_CORE,
				    &core_events, NULL);

	pw_core_do_client_update(remote->core_proxy, &remote->properties->dict);
	pw_core_do_sync(remote->core_proxy, 0);

	return 0;

      no_proxy:
	remote->conn->disconnect(remote->conn);
	remote_update_state(remote, PW_REMOTE_STATE_ERROR, "can't connect: no memory");
	return -1;
}

int pw_remote_connect(struct pw_remote *remote)
{
	int res;

	remote_update_state(remote, PW_REMOTE_STATE_CONNECTING, NULL);

	if ((res = remote->conn->connect(remote->conn)) < 0) {
		remote_update_state(remote, PW_REMOTE_STATE_ERROR, "connect failed");
		return res;
	}

	return do_connect(remote);
}

int pw_remote_connect_fd(struct pw_remote *remote, int fd)
{
	int res;

	remote_update_state(remote, PW_REMOTE_STATE_CONNECTING, NULL);

	if ((res = remote->conn->connect_fd(remote->conn, fd)) < 0) {
		remote_update_state(remote, PW_REMOTE_STATE_ERROR, "connect_fd failed");
		return res;
	}

	return do_connect(remote);
}

void pw_remote_disconnect(struct pw_remote *remote)
{
	struct pw_proxy *proxy, *t2;
	struct pw_stream *stream, *s2;

	pw_log_debug("remote %p: disconnect", remote);
	remote->conn->disconnect(remote->conn);

	spa_list_for_each_safe(stream, s2, &remote->stream_list, link)
	    pw_stream_destroy(stream);

	spa_list_for_each_safe(proxy, t2, &remote->proxy_list, link)
	    pw_proxy_destroy(proxy);

	remote->core_proxy = NULL;

	pw_map_clear(&remote->objects);
	pw_map_clear(&remote->types);
	remote->n_types = 0;

	if (remote->info) {
		pw_core_info_free (remote->info);
		remote->info = NULL;
	}

        remote_update_state(remote, PW_REMOTE_STATE_UNCONNECTED, NULL);
}
