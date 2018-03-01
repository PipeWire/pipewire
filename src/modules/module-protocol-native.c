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

#include <spa/pod/iter.h>
#include <spa/lib/debug.h>

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
#include "pipewire/factory.h"
#include "pipewire/data-loop.h"
#include "pipewire/main-loop.h"

#include "extensions/protocol-native.h"
#include "modules/module-protocol-native/connection.h"
#include "modules/module-protocol-native/defs.h"

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX   108
#endif

static bool debug_messages = 0;

#define LOCK_SUFFIX     ".lock"
#define LOCK_SUFFIXLEN  5

void pw_protocol_native_init(struct pw_protocol *protocol);

struct protocol_data {
	struct pw_module *module;
	struct spa_hook module_listener;
	struct pw_protocol *protocol;
	struct pw_properties *properties;
};

struct client {
	struct pw_protocol_client this;

	struct pw_properties *properties;
	struct spa_source *source;

        struct pw_protocol_native_connection *connection;
        struct spa_hook conn_listener;

        bool disconnecting;
	bool flush_signaled;
        struct spa_source *flush_event;
};

struct server {
	struct pw_protocol_server this;

	int fd_lock;
	struct sockaddr_un addr;
	char lock_addr[UNIX_PATH_MAX + LOCK_SUFFIXLEN];

	struct pw_loop *loop;
	struct spa_source *source;
	struct spa_hook hook;
};

struct client_data {
	struct pw_client *client;
	struct spa_hook client_listener;
	struct spa_source *source;
	struct pw_protocol_native_connection *connection;
	bool busy;
};

static bool pod_remap_data(uint32_t type, void *body, uint32_t size, struct pw_map *types)
{
	void *t;

	switch (type) {
	case SPA_POD_TYPE_ID:
		if ((t = pw_map_lookup(types, *(int32_t *) body)) == NULL)
			return false;
		*(int32_t *) body = PW_MAP_PTR_TO_ID(t);
		break;

	case SPA_POD_TYPE_PROP:
	{
		struct spa_pod_prop_body *b = body;

		if ((t = pw_map_lookup(types, b->key)) == NULL)
			return false;
		b->key = PW_MAP_PTR_TO_ID(t);

		if (b->value.type == SPA_POD_TYPE_ID) {
			void *alt;
			if (!pod_remap_data
			    (b->value.type, SPA_POD_BODY(&b->value), b->value.size, types))
				return false;

			SPA_POD_PROP_ALTERNATIVE_FOREACH(b, size, alt)
				if (!pod_remap_data(b->value.type, alt, b->value.size, types))
					return false;
		}
		break;
	}
	case SPA_POD_TYPE_OBJECT:
	{
		struct spa_pod_object_body *b = body;
		struct spa_pod *p;

		if ((t = pw_map_lookup(types, b->id)) != NULL)
			b->id = PW_MAP_PTR_TO_ID(t);
		else
			b->id = SPA_ID_INVALID;

		if ((t = pw_map_lookup(types, b->type)) == NULL)
			return false;
		b->type = PW_MAP_PTR_TO_ID(t);

		SPA_POD_OBJECT_BODY_FOREACH(b, size, p)
			if (!pod_remap_data(p->type, SPA_POD_BODY(p), p->size, types))
				return false;
		break;
	}
	case SPA_POD_TYPE_STRUCT:
	{
		struct spa_pod *b = body, *p;

		SPA_POD_FOREACH(b, size, p)
			if (!pod_remap_data(p->type, SPA_POD_BODY(p), p->size, types))
				return false;
		break;
	}
	default:
		break;
	}
	return true;
}

static void
process_messages(struct client_data *data)
{
	struct pw_protocol_native_connection *conn = data->connection;
	struct pw_client *client = data->client;
	struct pw_core *core = client->core;
	uint8_t opcode;
	uint32_t id, size;
	void *message;

	core->current_client = client;

	/* when the client is busy processing an async action, stop processing messages
	 * for the client until it finishes the action */
	while (!data->busy) {
		struct pw_resource *resource;
		const struct pw_protocol_native_demarshal *demarshal;
	        const struct pw_protocol_marshal *marshal;
		uint32_t permissions;

		if (!pw_protocol_native_connection_get_next(conn, &opcode, &id, &message, &size))
			break;

		pw_log_trace("protocol-native %p: got message %d from %u", client->protocol,
			     opcode, id);

		resource = pw_client_find_resource(client, id);
		if (resource == NULL) {
			pw_log_error("protocol-native %p: unknown resource %u",
				     client->protocol, id);
			continue;
		}
		permissions = pw_resource_get_permissions(resource);
		if ((permissions & PW_PERM_X) == 0) {
			pw_log_error("protocol-native %p: execute not allowed on resource %u",
				     client->protocol, id);
			continue;
		}

		marshal = pw_resource_get_marshal(resource);
		if (marshal == NULL || opcode >= marshal->n_methods)
			goto invalid_method;

		demarshal = marshal->method_demarshal;
		if (!demarshal[opcode].func)
			goto invalid_message;

		if ((demarshal[opcode].flags & PW_PROTOCOL_NATIVE_PERM_W) &&
		    ((permissions & PW_PERM_X) == 0)) {
			pw_log_error("protocol-native %p: method %u requires write access on %u",
				     client->protocol, opcode, id);
			continue;
		}

		if (demarshal[opcode].flags & PW_PROTOCOL_NATIVE_REMAP)
			if (!pod_remap_data(SPA_POD_TYPE_STRUCT, message, size, &client->types))
				goto invalid_message;

		if (debug_messages) {
			printf("<<<<<<<<< in: %d %d %d\n", id, opcode, size);
		        spa_debug_pod((struct spa_pod *)message, 0);
		}
		if (demarshal[opcode].func(resource, message, size) < 0)
			goto invalid_message;
	}
      done:
	core->current_client = NULL;
	return;

      invalid_method:
	pw_log_error("protocol-native %p: invalid method %u on resource %u",
		     client->protocol, opcode, id);
	pw_client_destroy(client);
	goto done;
      invalid_message:
	pw_log_error("protocol-native %p: invalid message received %u %u",
		     client->protocol, id, opcode);
	pw_client_destroy(client);
	goto done;
}

static void
client_busy_changed(void *data, bool busy)
{
	struct client_data *c = data;
	struct pw_client *client = c->client;
	enum spa_io mask = SPA_IO_ERR | SPA_IO_HUP;

	c->busy = busy;

	if (!busy)
		mask |= SPA_IO_IN;

	pw_log_debug("protocol-native %p: busy changed %d", client->protocol, busy);
	pw_loop_update_io(client->core->main_loop, c->source, mask);

	if (!busy)
		process_messages(c);

}

static void
connection_data(void *data, int fd, enum spa_io mask)
{
	struct client_data *this = data;
	struct pw_client *client = this->client;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_error("protocol-native %p: got connection error", client->protocol);
		pw_client_destroy(client);
		return;
	}

	if (mask & SPA_IO_IN)
		process_messages(this);
}

static void client_free(void *data)
{
	struct client_data *this = data;
	struct pw_client *client = this->client;

	pw_loop_destroy_source(client->protocol->core->main_loop, this->source);
	spa_list_remove(&client->protocol_link);

	pw_protocol_native_connection_destroy(this->connection);
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.free = client_free,
	.busy_changed = client_busy_changed,
};

static struct pw_client *client_new(struct server *s, int fd)
{
	struct client_data *this;
	struct pw_client *client;
	struct pw_protocol *protocol = s->this.protocol;
	struct protocol_data *pd = protocol->user_data;
	socklen_t len;
	struct ucred ucred, *ucredp;
	struct pw_core *core = protocol->core;
	struct pw_properties *props;

	len = sizeof(ucred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) < 0) {
		pw_log_error("no peercred: %m");
		ucredp = NULL;
	} else {
		ucredp = &ucred;
	}

	props = pw_properties_new(PW_CLIENT_PROP_PROTOCOL, "protocol-native", NULL);
	if (props == NULL)
		goto no_props;

	client = pw_client_new(protocol->core,
			       ucredp,
			       props,
			       sizeof(struct client_data));
	if (client == NULL)
		goto no_client;

	this = pw_client_get_user_data(client);
	this->client = client;
	this->source = pw_loop_add_io(pw_core_get_main_loop(core),
				      fd, SPA_IO_ERR | SPA_IO_HUP, true, connection_data, this);
	if (this->source == NULL)
		goto no_source;

	this->connection = pw_protocol_native_connection_new(fd);
	if (this->connection == NULL)
		goto no_connection;

	client->protocol = protocol;
	spa_list_append(&s->this.client_list, &client->protocol_link);

	pw_client_add_listener(client, &this->client_listener, &client_events, this);
	pw_client_register(client, client, pw_module_get_global(pd->module), NULL);

	pw_global_bind(pw_core_get_global(core), client, PW_PERM_RWX, PW_VERSION_CORE, 0);

	return client;

      no_connection:
	pw_loop_destroy_source(pw_core_get_main_loop(core), this->source);
      no_source:
	pw_client_destroy(client);
      no_client:
      no_props:
	return NULL;
}

static bool init_socket_name(struct server *s, const char *name)
{
	int name_size;
	const char *runtime_dir;

	if ((runtime_dir = getenv("XDG_RUNTIME_DIR")) == NULL) {
		pw_log_error("XDG_RUNTIME_DIR not set in the environment");
		return false;
	}

	s->addr.sun_family = AF_LOCAL;
	name_size = snprintf(s->addr.sun_path, sizeof(s->addr.sun_path),
			     "%s/%s", runtime_dir, name) + 1;

	if (name_size > (int) sizeof(s->addr.sun_path)) {
		pw_log_error("socket path \"%s/%s\" plus null terminator exceeds 108 bytes",
			     runtime_dir, name);
		*s->addr.sun_path = 0;
		return false;
	}
	return true;
}

static bool lock_socket(struct server *s)
{
	struct stat socket_stat;

	snprintf(s->lock_addr, sizeof(s->lock_addr), "%s%s", s->addr.sun_path, LOCK_SUFFIX);

	s->fd_lock = open(s->lock_addr, O_CREAT | O_CLOEXEC,
			  (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP));

	if (s->fd_lock < 0) {
		pw_log_error("unable to open lockfile %s check permissions", s->lock_addr);
		goto err;
	}

	if (flock(s->fd_lock, LOCK_EX | LOCK_NB) < 0) {
		pw_log_error("unable to lock lockfile %s, maybe another daemon is running",
			     s->lock_addr);
		goto err_fd;
	}

	if (stat(s->addr.sun_path, &socket_stat) < 0) {
		if (errno != ENOENT) {
			pw_log_error("did not manage to stat file %s\n", s->addr.sun_path);
			goto err_fd;
		}
	} else if (socket_stat.st_mode & S_IWUSR || socket_stat.st_mode & S_IWGRP) {
		unlink(s->addr.sun_path);
	}
	return true;

      err_fd:
	close(s->fd_lock);
	s->fd_lock = -1;
      err:
	*s->lock_addr = 0;
	*s->addr.sun_path = 0;
	return false;
}

static void
socket_data(void *data, int fd, enum spa_io mask)
{
	struct server *s = data;
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

	client = client_new(s, client_fd);
	if (client == NULL) {
		pw_log_error("failed to create client");
		close(client_fd);
		return;
	}
	c = client->user_data;

	pw_loop_update_io(client->protocol->core->main_loop,
			  c->source, SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);
}

static bool add_socket(struct pw_protocol *protocol, struct server *s)
{
	socklen_t size;
	int fd;

	if ((fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0)
		goto error;

	size = offsetof(struct sockaddr_un, sun_path) + strlen(s->addr.sun_path);
	if (bind(fd, (struct sockaddr *) &s->addr, size) < 0) {
		pw_log_error("bind() failed with error: %m");
		goto error_close;
	}

	if (listen(fd, 128) < 0) {
		pw_log_error("listen() failed with error: %m");
		goto error_close;
	}

	s->loop = pw_core_get_main_loop(protocol->core);
	s->source = pw_loop_add_io(s->loop, fd, SPA_IO_IN, true, socket_data, s);
	if (s->source == NULL)
		goto error_close;

	return true;

      error_close:
	close(fd);
      error:
	return false;

}

static int impl_steal_fd(struct pw_protocol_client *client)
{
	struct client *impl = SPA_CONTAINER_OF(client, struct client, this);
	int fd;

	if (impl->source == NULL)
		return -EIO;

	fd = dup(impl->source->fd);

	pw_protocol_client_disconnect(client);

	return fd;
}

static void
on_remote_data(void *data, int fd, enum spa_io mask)
{
	struct client *impl = data;
	struct pw_remote *this = impl->this.remote;
	struct pw_protocol_native_connection *conn = impl->connection;
	struct pw_core *core = pw_remote_get_core(this);

        if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_error("protocol-native %p: got connection error", impl);
		pw_loop_destroy_source(pw_core_get_main_loop(core), impl->source);
		impl->source = NULL;
		pw_remote_disconnect(this);
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
			const struct pw_protocol_marshal *marshal;

                        pw_log_trace("protocol-native %p: got message %d from %u", this, opcode, id);

                        proxy = pw_remote_find_proxy(this, id);

                        if (proxy == NULL) {
                                pw_log_error("protocol-native %p: could not find proxy %u", this, id);
                                continue;
                        }

			marshal = pw_proxy_get_marshal(proxy);
                        if (marshal == NULL || opcode >= marshal->n_events) {
                                pw_log_error("protocol-native %p: invalid method %u for %u", this, opcode,
                                             id);
                                continue;
                        }

                        demarshal = marshal->event_demarshal;
			if (!demarshal[opcode].func) {
                                pw_log_error("protocol-native %p: function %d not implemented on %u", this,
                                             opcode, id);
				continue;
			}

			if (demarshal[opcode].flags & PW_PROTOCOL_NATIVE_REMAP) {
				if (!pod_remap_data(SPA_POD_TYPE_STRUCT, message, size, &this->types)) {
                                        pw_log_error
                                            ("protocol-native %p: invalid message received %u for %u", this,
                                             opcode, id);
					continue;
				}
			}
			if (debug_messages) {
				printf("<<<<<<<<< in: %d %d %d\n", id, opcode, size);
			        spa_debug_pod((struct spa_pod *)message, 0);
			}
			if (demarshal[opcode].func(proxy, message, size) < 0) {
				pw_log_error ("protocol-native %p: invalid message received %u for %u", this,
					opcode, id);
				continue;
			}
                }
        }
}


static void do_flush_event(void *data, uint64_t count)
{
        struct client *impl = data;
	impl->flush_signaled = false;
        if (impl->connection)
                if (!pw_protocol_native_connection_flush(impl->connection))
                        impl->this.disconnect(&impl->this);
}

static void on_need_flush(void *data)
{
        struct client *impl = data;
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

static int impl_connect_fd(struct pw_protocol_client *client, int fd)
{
	struct client *impl = SPA_CONTAINER_OF(client, struct client, this);
	struct pw_remote *remote = client->remote;

	impl->disconnecting = false;

	impl->connection = pw_protocol_native_connection_new(fd);
	if (impl->connection == NULL)
                goto error_close;

	pw_protocol_native_connection_add_listener(impl->connection,
						   &impl->conn_listener,
						   &conn_events,
						   impl);

        impl->source = pw_loop_add_io(remote->core->main_loop,
                                      fd,
                                      SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR,
                                      true, on_remote_data, impl);
	if (impl->source == NULL)
		goto error_close;

	return 0;

      error_close:
        close(fd);
        return -ENOMEM;
}

static void impl_disconnect(struct pw_protocol_client *client)
{
	struct client *impl = SPA_CONTAINER_OF(client, struct client, this);
	struct pw_remote *remote = client->remote;

	impl->disconnecting = true;

	if (impl->source)
                pw_loop_destroy_source(remote->core->main_loop, impl->source);
	impl->source = NULL;

	if (impl->connection)
                pw_protocol_native_connection_destroy(impl->connection);
	impl->connection = NULL;
}

static void impl_destroy(struct pw_protocol_client *client)
{
	struct client *impl = SPA_CONTAINER_OF(client, struct client, this);
	struct pw_remote *remote = client->remote;

	impl_disconnect(client);

	pw_loop_destroy_source(remote->core->main_loop, impl->flush_event);

	if (impl->properties)
		pw_properties_free(impl->properties);

	spa_list_remove(&client->link);
	free(impl);
}

static struct pw_protocol_client *
impl_new_client(struct pw_protocol *protocol,
		struct pw_remote *remote,
		struct pw_properties *properties)
{
	struct client *impl;
	struct pw_protocol_client *this;
	const char *str = NULL;

	if ((impl = calloc(1, sizeof(struct client))) == NULL)
		return NULL;

	this = &impl->this;
	this->protocol = protocol;
	this->remote = remote;

	impl->properties = properties ? pw_properties_copy(properties) : NULL;

	if (properties)
		str = pw_properties_get(properties, "remote.intention");
	if (str == NULL)
		str = "generic";

	if (!strcmp(str, "screencast"))
		this->connect = pw_protocol_native_connect_portal_screencast;
	else
		this->connect = pw_protocol_native_connect_local_socket;

	this->steal_fd = impl_steal_fd;
	this->connect_fd = impl_connect_fd;
	this->disconnect = impl_disconnect;
	this->destroy = impl_destroy;

	impl->flush_event = pw_loop_add_event(remote->core->main_loop, do_flush_event, impl);

	spa_list_append(&protocol->client_list, &this->link);

	return this;
}

static void destroy_server(struct pw_protocol_server *server)
{
	struct server *s = SPA_CONTAINER_OF(server, struct server, this);
	struct pw_client *client, *tmp;

	spa_list_remove(&server->link);

	spa_list_for_each_safe(client, tmp, &server->client_list, protocol_link)
		pw_client_destroy(client);

	if (s->source)
		pw_loop_destroy_source(s->loop, s->source);
	if (s->addr.sun_path[0])
		unlink(s->addr.sun_path);
	if (s->lock_addr[0])
		unlink(s->lock_addr);
	if (s->fd_lock != 1)
		close(s->fd_lock);
	free(s);
}

static void on_before_hook(void *_data)
{
	struct server *server = _data;
	struct pw_protocol_server *this = &server->this;
	struct pw_client *client, *tmp;
	struct client_data *data;

	spa_list_for_each_safe(client, tmp, &this->client_list, protocol_link) {
		data = client->user_data;
		pw_protocol_native_connection_flush(data->connection);
	}
}

static const struct spa_loop_control_hooks impl_hooks = {
	SPA_VERSION_LOOP_CONTROL_HOOKS,
	.before = on_before_hook,
};

static const char *
get_name(const struct pw_properties *properties)
{
	const char *name = NULL;

	if (properties)
		name = pw_properties_get(properties, PW_CORE_PROP_NAME);
	if (name == NULL)
		name = getenv("PIPEWIRE_CORE");
	if (name == NULL)
		name = "pipewire-0";
	return name;
}

static struct pw_protocol_server *
impl_add_server(struct pw_protocol *protocol,
                struct pw_core *core,
                struct pw_properties *properties)
{
	struct pw_protocol_server *this;
	struct server *s;
	const char *name;

	if ((s = calloc(1, sizeof(struct server))) == NULL)
		return NULL;

	s->fd_lock = -1;

	this = &s->this;
	this->protocol = protocol;
	spa_list_init(&this->client_list);
	this->destroy = destroy_server;

	spa_list_append(&protocol->server_list, &this->link);

	name = get_name(pw_core_get_properties(core));

	if (!init_socket_name(s, name))
		goto error;

	if (!lock_socket(s))
		goto error;

	if (!add_socket(protocol, s))
		goto error;

	pw_loop_add_hook(pw_core_get_main_loop(core), &s->hook, &impl_hooks, s);

	pw_log_info("protocol-native %p: Added server %p %s", protocol, this, name);

	return this;

      error:
	destroy_server(this);
	return NULL;
}

const static struct pw_protocol_implementaton protocol_impl = {
	PW_VERSION_PROTOCOL_IMPLEMENTATION,
	.new_client = impl_new_client,
	.add_server = impl_add_server,
};

static struct spa_pod_builder *
impl_ext_begin_proxy(struct pw_proxy *proxy, uint8_t opcode)
{
	struct client *impl = SPA_CONTAINER_OF(proxy->remote->conn, struct client, this);
	return pw_protocol_native_connection_begin_proxy(impl->connection, proxy, opcode);
}

static uint32_t impl_ext_add_proxy_fd(struct pw_proxy *proxy, int fd)
{
	struct client *impl = SPA_CONTAINER_OF(proxy->remote->conn, struct client, this);
	return pw_protocol_native_connection_add_fd(impl->connection, fd);
}

static int impl_ext_get_proxy_fd(struct pw_proxy *proxy, uint32_t index)
{
	struct client *impl = SPA_CONTAINER_OF(proxy->remote->conn, struct client, this);
	return pw_protocol_native_connection_get_fd(impl->connection, index);
}

static void impl_ext_end_proxy(struct pw_proxy *proxy,
			       struct spa_pod_builder *builder)
{
	struct client *impl = SPA_CONTAINER_OF(proxy->remote->conn, struct client, this);
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

static void module_destroy(void *data)
{
	struct protocol_data *d = data;

	spa_hook_remove(&d->module_listener);

	if (d->properties)
		pw_properties_free(d->properties);

	pw_protocol_destroy(d->protocol);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
};

static int module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct pw_protocol *this;
	const char *val;
	struct protocol_data *d;

	if (pw_core_find_protocol(core, PW_TYPE_PROTOCOL__Native) != NULL)
		return 0;

	this = pw_protocol_new(core, PW_TYPE_PROTOCOL__Native, sizeof(struct protocol_data));
	if (this == NULL)
		return -ENOMEM;

	debug_messages = pw_debug_is_category_enabled("connection");

	this->implementation = &protocol_impl;
	this->extension = &protocol_ext_impl;

	pw_protocol_native_init(this);

	pw_log_debug("protocol-native %p: new %d", this, debug_messages);

	d = pw_protocol_get_user_data(this);
	d->protocol = this;
	d->module = module;
	d->properties = properties;

	val = getenv("PIPEWIRE_DAEMON");
	if (val == NULL)
		val = pw_properties_get(pw_core_get_properties(core), PW_CORE_PROP_DAEMON);
	if (val && pw_properties_parse_bool(val)) {
		if (impl_add_server(this, core, properties) == NULL)
			return -ENOMEM;
	}

	pw_module_add_listener(module, &d->module_listener, &module_events, d);

	return 0;
}

int pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
