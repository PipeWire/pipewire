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

#include "modules/module-protocol-native/connection.h"

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX   108
#endif

#define LOCK_SUFFIX     ".lock"
#define LOCK_SUFFIXLEN  5

struct pw_protocol *pw_protocol_native_init(void);

typedef bool(*demarshal_func_t) (void *object, void *data, size_t size);

struct connection {
	struct pw_protocol_connection this;

	int fd;

	struct spa_source *source;
        struct pw_connection *connection;

        bool disconnecting;
        struct pw_listener need_flush;
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
};

struct impl {
	struct pw_core *core;
	struct spa_list link;

	struct pw_protocol *protocol;
	struct pw_properties *properties;

	struct spa_list client_list;

	struct spa_loop_control_hooks hooks;
};

struct native_client {
	struct impl *impl;
	struct spa_list link;
	struct pw_client *client;
	int fd;
	struct spa_source *source;
	struct pw_connection *connection;
	struct pw_listener busy_changed;
};

static void client_destroy(void *data)
{
	struct pw_client *client = data;
	struct native_client *this = client->user_data;

	pw_loop_destroy_source(this->impl->core->main_loop, this->source);
	spa_list_remove(&this->link);

	pw_connection_destroy(this->connection);
	close(this->fd);
}

static void
process_messages(struct native_client *client)
{
	struct pw_connection *conn = client->connection;
	uint8_t opcode;
	uint32_t id;
	uint32_t size;
	struct pw_client *c = client->client;
	void *message;

	while (pw_connection_get_next(conn, &opcode, &id, &message, &size)) {
		struct pw_resource *resource;
		const demarshal_func_t *demarshal;

		pw_log_trace("protocol-native %p: got message %d from %u", client->impl,
			     opcode, id);

		resource = pw_map_lookup(&c->objects, id);
		if (resource == NULL) {
			pw_log_error("protocol-native %p: unknown resource %u",
				     client->impl, id);
			continue;
		}
		if (opcode >= resource->iface->n_methods) {
			pw_log_error("protocol-native %p: invalid method %u %u", client->impl,
				     id,  opcode);
			pw_client_destroy(c);
			break;
		}
		demarshal = resource->iface->methods;
		if (!demarshal[opcode] || !demarshal[opcode] (resource, message, size)) {
			pw_log_error("protocol-native %p: invalid message received %u %u",
				     client->impl, id, opcode);
			pw_client_destroy(c);
			break;
		}
		if (c->busy) {
			break;
		}
	}
}

static void
on_busy_changed(struct pw_listener *listener,
		struct pw_client *client)
{
	struct native_client *c = SPA_CONTAINER_OF(listener, struct native_client, busy_changed);
	enum spa_io mask = SPA_IO_ERR | SPA_IO_HUP;

	if (!client->busy)
		mask |= SPA_IO_IN;

	pw_loop_update_io(c->impl->core->main_loop, c->source, mask);

	if (!client->busy)
		process_messages(c);

}

static void on_before_hook(const struct spa_loop_control_hooks *hooks)
{
	struct impl *this = SPA_CONTAINER_OF(hooks, struct impl, hooks);
	struct native_client *client, *tmp;

	spa_list_for_each_safe(client, tmp, &this->client_list, link)
		pw_connection_flush(client->connection);
}

static void
connection_data(struct spa_loop_utils *utils,
		struct spa_source *source, int fd, enum spa_io mask, void *data)
{
	struct native_client *client = data;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_error("protocol-native %p: got connection error", client->impl);
		pw_client_destroy(client->client);
		return;
	}

	if (mask & SPA_IO_IN)
		process_messages(client);
}

static struct native_client *client_new(struct impl *impl, int fd)
{
	struct native_client *this;
	struct pw_client *client;
	socklen_t len;
	struct ucred ucred, *ucredp;

	len = sizeof(ucred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) < 0) {
		pw_log_error("no peercred: %m");
		ucredp = NULL;
	} else {
		ucredp = &ucred;
	}

	client = pw_client_new(impl->core, ucredp, NULL, sizeof(struct native_client));
	if (client == NULL)
		goto no_client;

	client->destroy = client_destroy;

	this = client->user_data;
	this->impl = impl;
	this->fd = fd;
	this->source = pw_loop_add_io(impl->core->main_loop,
				      this->fd,
				      SPA_IO_ERR | SPA_IO_HUP, false, connection_data, this);
	if (this->source == NULL)
		goto no_source;

	this->connection = pw_connection_new(fd);
	if (this->connection == NULL)
		goto no_connection;

	client->protocol = impl->protocol;
	client->protocol_private = this->connection;

	this->client = client;

	spa_list_insert(impl->client_list.prev, &this->link);

	pw_signal_add(&client->busy_changed, &this->busy_changed, on_busy_changed);

	pw_global_bind(impl->core->global, client, 0, 0);

	return this;

      no_connection:
	pw_loop_destroy_source(impl->core->main_loop, this->source);
      no_source:
	free(this);
      no_client:
	return NULL;
}

static void destroy_listener(struct listener *l)
{
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
	struct impl *impl = data;
	struct native_client *client;
	struct sockaddr_un name;
	socklen_t length;
	int client_fd;

	length = sizeof(name);
	client_fd = accept4(fd, (struct sockaddr *) &name, &length, SOCK_CLOEXEC);
	if (client_fd < 0) {
		pw_log_error("failed to accept: %m");
		return;
	}

	client = client_new(impl, client_fd);
	if (client == NULL) {
		pw_log_error("failed to create client");
		close(client_fd);
		return;
	}

	pw_loop_update_io(impl->core->main_loop,
			  client->source, SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);
}

static bool add_socket(struct impl *impl, struct listener *l)
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

	l->loop = impl->core->main_loop;
	l->source = pw_loop_add_io(l->loop, l->fd, SPA_IO_IN, false, socket_data, impl);
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

        if (name == NULL)
                name = getenv("PIPEWIRE_CORE");
        if (name == NULL)
                name = "pipewire-0";

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
        struct pw_connection *conn = impl->connection;

        if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
                return;
        }

        if (mask & SPA_IO_IN) {
                uint8_t opcode;
                uint32_t id;
                uint32_t size;
                void *message;

                while (!impl->disconnecting
                       && pw_connection_get_next(conn, &opcode, &id, &message, &size)) {
                        struct pw_proxy *proxy;
                        const demarshal_func_t *demarshal;

                        pw_log_trace("protocol-native %p: got message %d from %u", this, opcode, id);

                        proxy = pw_map_lookup(&this->objects, id);
                        if (proxy == NULL) {
                                pw_log_error("protocol-native %p: could not find proxy %u", this, id);
                                continue;
                        }
                        if (opcode >= proxy->iface->n_events) {
                                pw_log_error("protocol-native %p: invalid method %u for %u", this, opcode,
                                             id);
                                continue;
                        }

                        demarshal = proxy->iface->events;
                        if (demarshal[opcode]) {
                                if (!demarshal[opcode] (proxy, message, size))
                                        pw_log_error
                                            ("protocol-native %p: invalid message received %u for %u", this,
                                             opcode, id);
                        } else
                                pw_log_error("protocol-native %p: function %d not implemented on %u", this,
                                             opcode, id);

                }
        }
}


static void do_flush_event(struct spa_loop_utils *utils, struct spa_source *source, void *data)
{
        struct connection *impl = data;
        if (impl->connection)
                if (!pw_connection_flush(impl->connection))
                        impl->this.disconnect(&impl->this);
}

static void on_need_flush(struct pw_listener *listener, struct pw_connection *connection)
{
        struct connection *impl = SPA_CONTAINER_OF(listener, struct connection, need_flush);
        struct pw_remote *remote = impl->this.remote;
        pw_loop_signal_event(remote->core->main_loop, impl->flush_event);
}

static int impl_connect_fd(struct pw_protocol_connection *conn, int fd)
{
        struct connection *impl = SPA_CONTAINER_OF(conn, struct connection, this);
	struct pw_remote *remote = impl->this.remote;

        impl->connection = pw_connection_new(fd);
        if (impl->connection == NULL)
                goto error_close;

        conn->remote->protocol_private = impl->connection;

        pw_signal_add(&impl->connection->need_flush, &impl->need_flush, on_need_flush);

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

static int impl_disconnect(struct pw_protocol_connection *conn)
{
        struct connection *impl = SPA_CONTAINER_OF(conn, struct connection, this);
	struct pw_remote *remote = impl->this.remote;

        impl->disconnecting = true;

        if (impl->source)
                pw_loop_destroy_source(remote->core->main_loop, impl->source);
        impl->source = NULL;

        if (impl->connection)
                pw_connection_destroy(impl->connection);
        impl->connection = NULL;

        if (impl->fd != -1)
                close(impl->fd);
        impl->fd = -1;

	return 0;
}

static int impl_destroy(struct pw_protocol_connection *conn)
{
        struct connection *impl = SPA_CONTAINER_OF(conn, struct connection, this);
	struct pw_remote *remote = conn->remote;

        pw_loop_destroy_source(remote->core->main_loop, impl->flush_event);

	spa_list_remove(&conn->link);
	free(impl);

	return 0;
}

static struct pw_protocol_connection *
impl_new_connection(struct pw_protocol *protocol,
		    struct pw_remote *remote,
		    struct pw_properties *properties)
{
	struct impl *impl = protocol->protocol_private;
	struct connection *c;
	struct pw_protocol_connection *this;

	if ((c = calloc(1, sizeof(struct connection))) == NULL)
		return NULL;

	this = &c->this;
	this->remote = remote;

	this->connect = impl_connect;
	this->connect_fd = impl_connect_fd;
	this->disconnect = impl_disconnect;
	this->destroy = impl_destroy;

        c->flush_event = pw_loop_add_event(remote->core->main_loop, do_flush_event, c);

	spa_list_insert(impl->protocol->connection_list.prev, &c->this.link);

	return this;
}

static struct pw_protocol_listener *
impl_add_listener(struct pw_protocol *protocol,
                  struct pw_core *core,
                  struct pw_properties *properties)
{
	struct impl *impl = protocol->protocol_private;
	struct listener *l;
	const char *name;

	if ((l = calloc(1, sizeof(struct listener))) == NULL)
		return NULL;

	l->fd = -1;
	l->fd_lock = -1;

	name = get_name(properties);

	if (!init_socket_name(l, name))
		goto error;

	if (!lock_socket(l))
		goto error;

	if (!add_socket(impl, l))
		goto error;

	spa_list_insert(impl->protocol->listener_list.prev, &l->this.link);

	impl->hooks.before = on_before_hook;
	pw_loop_add_hooks(impl->core->main_loop, &impl->hooks);

	pw_log_info("protocol-native %p: Added listener", protocol);

	return &l->this;

      error:
	destroy_listener(l);
	return NULL;
}


static struct impl *pw_protocol_native_new(struct pw_core *core, struct pw_properties *properties)
{
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));

	impl->core = core;
	impl->properties = properties;
	impl->protocol = pw_protocol_native_init();
	impl->protocol->new_connection = impl_new_connection;
	impl->protocol->add_listener = impl_add_listener;
	impl->protocol->protocol_private = impl;
	pw_log_debug("protocol-native %p: new %p", impl, impl->protocol);

	spa_list_init(&impl->client_list);

	impl_add_listener(impl->protocol, core, properties);

	return impl;
}

#if 0
static void pw_protocol_native_destroy(struct impl *impl)
{
	struct impl *object, *tmp;

	pw_log_debug("protocol-native %p: destroy", impl);

	pw_signal_remove(&impl->before_iterate);

	pw_global_destroy(impl->global);

	spa_list_for_each_safe(object, tmp, &impl->object_list, link)
	    object_destroy(object);

	free(impl);
}
#endif

bool pipewire__module_init(struct pw_module *module, const char *args)
{
	pw_protocol_native_new(module->core, NULL);
	return true;
}
