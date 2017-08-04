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

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>

#include "config.h"

#include "pipewire/pipewire.h"
#include "pipewire/private.h"
#include "pipewire/log.h"
#include "pipewire/interfaces.h"
#include "pipewire/core.h"
#include "pipewire/node.h"
#include "pipewire/module.h"
#include "pipewire/client.h"
#include "pipewire/resource.h"
#include "pipewire/link.h"
#include "pipewire/node-factory.h"
#include "pipewire/data-loop.h"
#include "pipewire/main-loop.h"

#include "extensions/protocol-native.h"
#include "modules/module-protocol-native/connection.h"

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX   108
#endif

#define LOCK_SUFFIX     ".lock"
#define LOCK_SUFFIXLEN  5

void pw_protocol_native_init(struct pw_protocol *protocol);

struct protocol_data {
	struct pw_module *module;
};

struct connection {
	struct pw_protocol_connection this;

	int fd;

	struct spa_source *source;

        struct pw_protocol_native_connection *connection;
        struct pw_listener conn_listener;

        bool disconnecting;
	bool flush_signaled;
        struct spa_source *flush_event;
};

struct listener {
	struct pw_protocol_listener this;

	int fd;
	int fd_lock;
	struct sockaddr_un addr;
	char lock_addr[UNIX_PATH_MAX + LOCK_SUFFIXLEN];

	struct pw_loop *loop;
	struct spa_source *source;
	struct spa_loop_control_hooks hooks;
};

struct client_data {
	struct pw_client *client;
	struct pw_listener client_listener;
	int fd;
	struct spa_source *source;
	struct pw_protocol_native_connection *connection;
};

static void
process_messages(struct pw_client *client)
{
	struct client_data *data = client->user_data;
	struct pw_protocol_native_connection *conn = data->connection;
	uint8_t opcode;
	uint32_t id;
	uint32_t size;
	void *message;

	while (pw_protocol_native_connection_get_next(conn, &opcode, &id, &message, &size)) {
		struct pw_resource *resource;
		const struct pw_protocol_native_demarshal *demarshal;

		pw_log_trace("protocol-native %p: got message %d from %u", client->protocol,
			     opcode, id);

		resource = pw_map_lookup(&client->objects, id);
		if (resource == NULL) {
			pw_log_error("protocol-native %p: unknown resource %u",
				     client->protocol, id);
			continue;
		}
		if ((resource->permissions & PW_PERM_X) == 0) {
			pw_log_error("protocol-native %p: execute not allowed on resource %u",
				     client->protocol, id);
			continue;
		}

		if (opcode >= resource->marshal->n_methods)
			goto invalid_method;

		demarshal = resource->marshal->method_demarshal;
		if (!demarshal[opcode].func)
			goto invalid_message;

		if ((demarshal[opcode].flags & PW_PROTOCOL_NATIVE_PERM_W) &&
		    ((resource->permissions & PW_PERM_X) == 0)) {
			pw_log_error("protocol-native %p: method %u requires write access on %u",
				     client->protocol, opcode, id);
			continue;
		}

		if (demarshal[opcode].flags & PW_PROTOCOL_NATIVE_REMAP)
			if (!pw_pod_remap_data(SPA_POD_TYPE_STRUCT, message, size, &client->types))
				goto invalid_message;

		if (!demarshal[opcode].func (resource, message, size))
			goto invalid_message;

		/* when the client is busy processing an async action, stop processing messages
		 * for the client until it finishes the action */
		if (client->busy)
			break;
	}
	return;

      invalid_method:
	pw_log_error("protocol-native %p: invalid method %u on resource %u",
		     client->protocol, opcode, id);
	pw_client_destroy(client);
	return;
      invalid_message:
	pw_log_error("protocol-native %p: invalid message received %u %u",
		     client->protocol, id, opcode);
	pw_client_destroy(client);
	return;
}

static void
client_busy_changed(void *data, bool busy)
{
	struct pw_client *client = data;
	struct client_data *c = client->user_data;
	enum spa_io mask = SPA_IO_ERR | SPA_IO_HUP;

	if (!busy)
		mask |= SPA_IO_IN;

	pw_loop_update_io(client->core->main_loop, c->source, mask);

	if (busy)
		process_messages(client);

}

static void on_before_hook(const struct spa_loop_control_hooks *hooks)
{
	struct listener *listener = SPA_CONTAINER_OF(hooks, struct listener, hooks);
	struct pw_protocol_listener *this = &listener->this;
	struct pw_client *client, *tmp;
	struct client_data *data;

	spa_list_for_each_safe(client, tmp, &this->client_list, protocol_link) {
		data = client->user_data;
		pw_protocol_native_connection_flush(data->connection);
	}
}

static void
connection_data(struct spa_loop_utils *utils,
		struct spa_source *source, int fd, enum spa_io mask, void *data)
{
	struct pw_client *client = data;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_error("protocol-native %p: got connection error", client->protocol);
		pw_client_destroy(client);
		return;
	}

	if (mask & SPA_IO_IN)
		process_messages(client);
}

static void client_free(void *data)
{
	struct pw_client *client = data;
	struct client_data *this = client->user_data;

	pw_loop_destroy_source(client->protocol->core->main_loop, this->source);
	spa_list_remove(&client->protocol_link);

	pw_protocol_native_connection_destroy(this->connection);
	close(this->fd);
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.free = client_free,
	.busy_changed = client_busy_changed,
};

static struct pw_client *client_new(struct listener *l, int fd)
{
	struct client_data *this;
	struct pw_client *client;
	struct pw_protocol *protocol = l->this.protocol;
	struct protocol_data *pd = protocol->user_data;
	socklen_t len;
	struct ucred ucred, *ucredp;

	len = sizeof(ucred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) < 0) {
		pw_log_error("no peercred: %m");
		ucredp = NULL;
	} else {
		ucredp = &ucred;
	}

	client = pw_client_new(protocol->core, pd->module->global, ucredp, NULL, sizeof(struct client_data));
	if (client == NULL)
		goto no_client;

	this = pw_client_get_user_data(client);
	this->client = client;
	this->fd = fd;
	this->source = pw_loop_add_io(protocol->core->main_loop,
				      this->fd,
				      SPA_IO_ERR | SPA_IO_HUP, false, connection_data, client);
	if (this->source == NULL)
		goto no_source;

	this->connection = pw_protocol_native_connection_new(fd);
	if (this->connection == NULL)
		goto no_connection;

	client->protocol = protocol;
	spa_list_insert(l->this.client_list.prev, &client->protocol_link);

	pw_client_add_listener(client, &this->client_listener, &client_events, client);

	pw_global_bind(protocol->core->global, client, PW_PERM_RWX, PW_VERSION_CORE, 0);

	return client;

      no_connection:
	pw_loop_destroy_source(protocol->core->main_loop, this->source);
      no_source:
	pw_client_destroy(client);
      no_client:
	return NULL;
}

static bool init_socket_name(struct listener *l, const char *name)
{
	int name_size;
	const char *runtime_dir;

	if ((runtime_dir = getenv("XDG_RUNTIME_DIR")) == NULL) {
		pw_log_error("XDG_RUNTIME_DIR not set in the environment");
		return false;
	}

	l->addr.sun_family = AF_LOCAL;
	name_size = snprintf(l->addr.sun_path, sizeof(l->addr.sun_path),
			     "%s/%s", runtime_dir, name) + 1;

	if (name_size > (int) sizeof(l->addr.sun_path)) {
		pw_log_error("socket path \"%s/%s\" plus null terminator exceeds 108 bytes",
			     runtime_dir, name);
		*l->addr.sun_path = 0;
		return false;
	}
	return true;
}

static bool lock_socket(struct listener *l)
{
	struct stat socket_stat;

	snprintf(l->lock_addr, sizeof(l->lock_addr), "%s%s", l->addr.sun_path, LOCK_SUFFIX);

	l->fd_lock = open(l->lock_addr, O_CREAT | O_CLOEXEC,
			  (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP));

	if (l->fd_lock < 0) {
		pw_log_error("unable to open lockfile %s check permissions", l->lock_addr);
		goto err;
	}

	if (flock(l->fd_lock, LOCK_EX | LOCK_NB) < 0) {
		pw_log_error("unable to lock lockfile %s, maybe another daemon is running",
			     l->lock_addr);
		goto err_fd;
	}

	if (stat(l->addr.sun_path, &socket_stat) < 0) {
		if (errno != ENOENT) {
			pw_log_error("did not manage to stat file %s\n", l->addr.sun_path);
			goto err_fd;
		}
	} else if (socket_stat.st_mode & S_IWUSR || socket_stat.st_mode & S_IWGRP) {
		unlink(l->addr.sun_path);
	}
	return true;

      err_fd:
	close(l->fd_lock);
	l->fd_lock = -1;
      err:
	*l->lock_addr = 0;
	*l->addr.sun_path = 0;
	return false;
}

static void
socket_data(struct spa_loop_utils *utils,
	    struct spa_source *source, int fd, enum spa_io mask, void *data)
{
	struct listener *l = data;
	struct pw_client *client;
	struct client_data *c;
	struct sockaddr_un name;
	socklen_t length;
	int client_fd;

	length = sizeof(name);
	client_fd = accept4(fd, (struct sockaddr *) &name, &length, SOCK_CLOEXEC);
	if (client_fd < 0) {
		pw_log_error("failed to accept: %m");
		return;
	}

	client = client_new(l, client_fd);
	if (client == NULL) {
		pw_log_error("failed to create client");
		close(client_fd);
		return;
	}
	c = client->user_data;

	pw_loop_update_io(client->protocol->core->main_loop,
			  c->source, SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);
}

static bool add_socket(struct pw_protocol *protocol, struct listener *l)
{
	socklen_t size;

	if ((l->fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0)
		return false;

	size = offsetof(struct sockaddr_un, sun_path) + strlen(l->addr.sun_path);
	if (bind(l->fd, (struct sockaddr *) &l->addr, size) < 0) {
		pw_log_error("bind() failed with error: %m");
		return false;
	}

	if (listen(l->fd, 128) < 0) {
		pw_log_error("listen() failed with error: %m");
		return false;
	}

	l->loop = protocol->core->main_loop;
	l->source = pw_loop_add_io(l->loop, l->fd, SPA_IO_IN, false, socket_data, l);
	if (l->source == NULL)
		return false;

	return true;
}

static const char *
get_name(struct pw_properties *properties)
{
	const char *name = NULL;

	if (properties)
		name = pw_properties_get(properties, "pipewire.core.name");
	if (name == NULL)
		name = getenv("PIPEWIRE_CORE");
	if (name == NULL)
		name = "pipewire-0";
	return name;
}

static int impl_connect(struct pw_protocol_connection *conn)
{
        struct sockaddr_un addr;
        socklen_t size;
        const char *runtime_dir, *name = NULL;
        int name_size, fd;

        if ((runtime_dir = getenv("XDG_RUNTIME_DIR")) == NULL) {
		pw_log_error("connect failed: XDG_RUNTIME_DIR not set in the environment");
                return -1;
        }

	name = get_name(NULL);

        if ((fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0)
                return -1;

        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_LOCAL;
        name_size = snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/%s", runtime_dir, name) + 1;

        if (name_size > (int) sizeof addr.sun_path) {
                pw_log_error("socket path \"%s/%s\" plus null terminator exceeds 108 bytes",
                             runtime_dir, name);
                goto error_close;
        };

        size = offsetof(struct sockaddr_un, sun_path) + name_size;

        if (connect(fd, (struct sockaddr *) &addr, size) < 0) {
                goto error_close;
        }

        return conn->connect_fd(conn, fd);

      error_close:
        close(fd);
        return -1;
}


static void
on_remote_data(struct spa_loop_utils *utils,
	       struct spa_source *source, int fd, enum spa_io mask, void *data)
{
        struct connection *impl = data;
        struct pw_remote *this = impl->this.remote;
        struct pw_protocol_native_connection *conn = impl->connection;

        if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_error("protocol-native %p: got connection error", impl);
		pw_loop_destroy_source(this->core->main_loop, impl->source);
		impl->source = NULL;
		pw_remote_update_state(this, PW_REMOTE_STATE_ERROR, "connection error");
		return;
        }

        if (mask & SPA_IO_IN) {
                uint8_t opcode;
                uint32_t id;
                uint32_t size;
                void *message;

                while (!impl->disconnecting
                       && pw_protocol_native_connection_get_next(conn, &opcode, &id, &message, &size)) {
                        struct pw_proxy *proxy;
                        const struct pw_protocol_native_demarshal *demarshal;

                        pw_log_trace("protocol-native %p: got message %d from %u", this, opcode, id);

                        proxy = pw_map_lookup(&this->objects, id);
                        if (proxy == NULL || proxy->marshal == NULL) {
                                pw_log_error("protocol-native %p: could not find proxy %u", this, id);
                                continue;
                        }
                        if (opcode >= proxy->marshal->n_events) {
                                pw_log_error("protocol-native %p: invalid method %u for %u", this, opcode,
                                             id);
                                continue;
                        }

                        demarshal = proxy->marshal->event_demarshal;
			if (!demarshal[opcode].func) {
                                pw_log_error("protocol-native %p: function %d not implemented on %u", this,
                                             opcode, id);
				continue;
			}

			if (demarshal[opcode].flags & PW_PROTOCOL_NATIVE_REMAP) {
				if (!pw_pod_remap_data(SPA_POD_TYPE_STRUCT, message, size, &this->types)) {
                                        pw_log_error
                                            ("protocol-native %p: invalid message received %u for %u", this,
                                             opcode, id);
					continue;
				}
			}
			if (!demarshal[opcode].func(proxy, message, size)) {
				pw_log_error ("protocol-native %p: invalid message received %u for %u", this,
					opcode, id);
				continue;
			}
                }
        }
}


static void do_flush_event(struct spa_loop_utils *utils, struct spa_source *source,
			   uint64_t count, void *data)
{
        struct connection *impl = data;
	impl->flush_signaled = false;
        if (impl->connection)
                if (!pw_protocol_native_connection_flush(impl->connection))
                        impl->this.disconnect(&impl->this);
}

static void on_need_flush(void *data)
{
        struct connection *impl = data;
        struct pw_remote *remote = impl->this.remote;

	if (!impl->flush_signaled) {
		impl->flush_signaled = true;
		pw_loop_signal_event(remote->core->main_loop, impl->flush_event);
	}
}

static const struct pw_protocol_native_connection_events conn_events = {
	PW_VERSION_PROTOCOL_NATIVE_CONNECTION_EVENTS,
	.need_flush = on_need_flush,
};

static int impl_connect_fd(struct pw_protocol_connection *conn, int fd)
{
        struct connection *impl = SPA_CONTAINER_OF(conn, struct connection, this);
	struct pw_remote *remote = conn->remote;

        impl->connection = pw_protocol_native_connection_new(fd);
        if (impl->connection == NULL)
                goto error_close;

	pw_protocol_native_connection_add_listener(impl->connection,
						   &impl->conn_listener,
						   &conn_events,
						   impl);

        impl->fd = fd;
        impl->source = pw_loop_add_io(remote->core->main_loop,
                                      fd,
                                      SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR,
                                      false, on_remote_data, impl);

	return 0;

      error_close:
        close(fd);
        return -1;
}

static void impl_disconnect(struct pw_protocol_connection *conn)
{
        struct connection *impl = SPA_CONTAINER_OF(conn, struct connection, this);
	struct pw_remote *remote = conn->remote;

        impl->disconnecting = true;

        if (impl->source)
                pw_loop_destroy_source(remote->core->main_loop, impl->source);
        impl->source = NULL;

        if (impl->connection)
                pw_protocol_native_connection_destroy(impl->connection);
        impl->connection = NULL;

        if (impl->fd != -1)
                close(impl->fd);
        impl->fd = -1;
}

static void impl_destroy(struct pw_protocol_connection *conn)
{
        struct connection *impl = SPA_CONTAINER_OF(conn, struct connection, this);
	struct pw_remote *remote = conn->remote;

        pw_loop_destroy_source(remote->core->main_loop, impl->flush_event);

	spa_list_remove(&conn->link);
	free(impl);
}

static struct pw_protocol_connection *
impl_new_connection(struct pw_protocol *protocol,
		    struct pw_remote *remote,
		    struct pw_properties *properties)
{
	struct connection *impl;
	struct pw_protocol_connection *this;

	if ((impl = calloc(1, sizeof(struct connection))) == NULL)
		return NULL;

	this = &impl->this;
	this->protocol = protocol;
	this->remote = remote;

	this->connect = impl_connect;
	this->connect_fd = impl_connect_fd;
	this->disconnect = impl_disconnect;
	this->destroy = impl_destroy;

        impl->flush_event = pw_loop_add_event(remote->core->main_loop, do_flush_event, impl);

	spa_list_insert(protocol->connection_list.prev, &this->link);

	return this;
}

static void destroy_listener(struct pw_protocol_listener *listener)
{
	struct listener *l = SPA_CONTAINER_OF(listener, struct listener, this);

	if (l->source)
		pw_loop_destroy_source(l->loop, l->source);
	if (l->addr.sun_path[0])
		unlink(l->addr.sun_path);
	if (l->fd >= 0)
		close(l->fd);
	if (l->lock_addr[0])
		unlink(l->lock_addr);
	if (l->fd_lock >= 0)
		close(l->fd_lock);
	free(l);
}

static struct pw_protocol_listener *
impl_add_listener(struct pw_protocol *protocol,
                  struct pw_core *core,
                  struct pw_properties *properties)
{
	struct pw_protocol_listener *this;
	struct listener *l;
	const char *name;

	if ((l = calloc(1, sizeof(struct listener))) == NULL)
		return NULL;

	l->fd = -1;
	l->fd_lock = -1;

	this = &l->this;
	this->protocol = protocol;
	spa_list_init(&this->client_list);
	this->destroy = destroy_listener;

	name = get_name(core->properties);

	if (!init_socket_name(l, name))
		goto error;

	if (!lock_socket(l))
		goto error;

	if (!add_socket(protocol, l))
		goto error;

	spa_list_insert(protocol->listener_list.prev, &this->link);

	l->hooks.before = on_before_hook;
	pw_loop_add_hooks(protocol->core->main_loop, &l->hooks);

	pw_log_info("protocol-native %p: Added listener %p", protocol, this);

	return this;

      error:
	destroy_listener(this);
	return NULL;
}

const static struct pw_protocol_implementaton protocol_impl = {
	PW_VERSION_PROTOCOL_IMPLEMENTATION,
	impl_new_connection,
	impl_add_listener,
};

static struct spa_pod_builder *
impl_ext_begin_proxy(struct pw_proxy *proxy, uint8_t opcode)
{
	struct connection *impl = SPA_CONTAINER_OF(proxy->remote->conn, struct connection, this);
	return pw_protocol_native_connection_begin_proxy(impl->connection, proxy, opcode);
}

static uint32_t impl_ext_add_proxy_fd(struct pw_proxy *proxy, int fd)
{
	struct connection *impl = SPA_CONTAINER_OF(proxy->remote->conn, struct connection, this);
	return pw_protocol_native_connection_add_fd(impl->connection, fd);
}

static int impl_ext_get_proxy_fd(struct pw_proxy *proxy, uint32_t index)
{
	struct connection *impl = SPA_CONTAINER_OF(proxy->remote->conn, struct connection, this);
	return pw_protocol_native_connection_get_fd(impl->connection, index);
}

static void impl_ext_end_proxy(struct pw_proxy *proxy,
			       struct spa_pod_builder *builder)
{
	struct connection *impl = SPA_CONTAINER_OF(proxy->remote->conn, struct connection, this);
	pw_protocol_native_connection_end(impl->connection, builder);
}

static struct spa_pod_builder *
impl_ext_begin_resource(struct pw_resource *resource, uint8_t opcode)
{
	struct client_data *data = resource->client->user_data;
	return pw_protocol_native_connection_begin_resource(data->connection, resource, opcode);
}

static uint32_t impl_ext_add_resource_fd(struct pw_resource *resource, int fd)
{
	struct client_data *data = resource->client->user_data;
	return pw_protocol_native_connection_add_fd(data->connection, fd);
}
static int impl_ext_get_resource_fd(struct pw_resource *resource, uint32_t index)
{
	struct client_data *data = resource->client->user_data;
	return pw_protocol_native_connection_get_fd(data->connection, index);
}

static void impl_ext_end_resource(struct pw_resource *resource,
				  struct spa_pod_builder *builder)
{
	struct client_data *data = resource->client->user_data;
	pw_protocol_native_connection_end(data->connection, builder);
}

const static struct pw_protocol_native_ext protocol_ext_impl = {
	PW_VERSION_PROTOCOL_NATIVE_EXT,
	impl_ext_begin_proxy,
	impl_ext_add_proxy_fd,
	impl_ext_get_proxy_fd,
	impl_ext_end_proxy,
	impl_ext_begin_resource,
	impl_ext_add_resource_fd,
	impl_ext_get_resource_fd,
	impl_ext_end_resource,
};

static bool module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = module->core;
	struct pw_protocol *this;
	const char *val;
	struct protocol_data *d;

	if (pw_core_find_protocol(core, PW_TYPE_PROTOCOL__Native) != NULL)
		return true;

	this = pw_protocol_new(core, PW_TYPE_PROTOCOL__Native, sizeof(struct protocol_data));
	if (this == NULL)
		return false;

	this->implementation = &protocol_impl;
	this->extension = &protocol_ext_impl;

	pw_protocol_native_init(this);

	pw_log_debug("protocol-native %p: new", this);

	d = pw_protocol_get_user_data(this);
	d->module = module;

	if ((val = pw_properties_get(core->properties, "pipewire.daemon"))) {
		if (atoi(val) == 1)
			impl_add_listener(this, core, properties);
	}
	return true;
}

#if 0
static void pw_protocol_native_destroy(struct impl *impl)
{
	struct impl *object, *tmp;

	pw_log_debug("protocol-native %p: destroy", impl);

	spa_list_for_each_safe(object, tmp, &impl->object_list, link)
	    object_destroy(object);

	free(impl);
}
#endif

bool pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
