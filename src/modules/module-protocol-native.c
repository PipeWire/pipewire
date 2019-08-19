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
#include <spa/debug/pod.h>
#include <spa/debug/types.h>

#include "config.h"

#ifdef HAVE_SYSTEMD_DAEMON
#include <systemd/sd-daemon.h>
#endif

#include <pipewire/pipewire.h>
#include <extensions/protocol-native.h>
#include "pipewire/private.h"

#include "modules/module-protocol-native/connection.h"
#include "modules/module-protocol-native/defs.h"

#define NAME "protocol-native"

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX   108
#endif

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Native protocol using unix sockets" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static bool debug_messages = 0;

#define LOCK_SUFFIX     ".lock"
#define LOCK_SUFFIXLEN  5

void pw_protocol_native_init(struct pw_protocol *protocol);

struct protocol_data {
	struct pw_module *module;
	struct spa_hook module_listener;
	struct pw_protocol *protocol;
};

struct client {
	struct pw_protocol_client this;

	struct spa_source *source;

	struct pw_protocol_native_connection *connection;
	struct spa_hook conn_listener;

	struct spa_source *flush_event;
	unsigned int disconnecting:1;
	unsigned int flush_signaled:1;
};

struct server {
	struct pw_protocol_server this;

	int fd_lock;
	struct sockaddr_un addr;
	char lock_addr[UNIX_PATH_MAX + LOCK_SUFFIXLEN];

	struct pw_loop *loop;
	struct spa_source *source;
	struct spa_hook hook;
	unsigned int activated:1;
};

struct client_data {
	struct pw_client *client;
	struct spa_hook client_listener;
	struct spa_source *source;
	struct pw_protocol_native_connection *connection;
	unsigned int busy:1;
};

static void
process_messages(struct client_data *data)
{
	struct pw_protocol_native_connection *conn = data->connection;
	struct pw_client *client = data->client;
	struct pw_core *core = client->core;
	const struct pw_protocol_native_message *msg;
	struct pw_resource *resource;
	int res;

	core->current_client = client;

	/* when the client is busy processing an async action, stop processing messages
	 * for the client until it finishes the action */
	while (!data->busy) {
		const struct pw_protocol_native_demarshal *demarshal;
	        const struct pw_protocol_marshal *marshal;
		uint32_t permissions, required;

		if (pw_protocol_native_connection_get_next(conn, &msg) != 1)
			break;

		client->recv_seq = msg->seq;

		pw_log_trace(NAME" %p: got message %d from %u", client->protocol,
			     msg->opcode, msg->id);

		if (debug_messages) {
			fprintf(stderr, "<<<<<<<<< in: id:%d op:%d size:%d seq:%d\n",
					msg->id, msg->opcode, msg->size, msg->seq);
		        spa_debug_pod(0, NULL, (struct spa_pod *)msg->data);
		}

		resource = pw_client_find_resource(client, msg->id);
		if (resource == NULL) {
			pw_log_error(NAME" %p: unknown resource %u op:%u",
				     client->protocol, msg->id, msg->opcode);
			pw_resource_error(client->core_resource,
					-EINVAL, "unknown resource %u op:%u", msg->id, msg->opcode);
			continue;
		}

		marshal = pw_resource_get_marshal(resource);
		if (marshal == NULL || msg->opcode >= marshal->n_methods)
			goto invalid_method;

		demarshal = marshal->method_demarshal;
		if (!demarshal[msg->opcode].func) {
			res = -ENOENT;
			goto invalid_message;
		}

		permissions = pw_resource_get_permissions(resource);
		required = demarshal[msg->opcode].permissions | PW_PERM_X;

		if ((required & permissions) != required) {
			pw_log_error(NAME" %p: method %u on %u requires %08x, have %08x",
				     client->protocol, msg->opcode, msg->id, required, permissions);
			pw_resource_error(resource,
				-EACCES, "no permission to call method %u ", msg->opcode, msg->id);
			continue;
		}

		if ((res = demarshal[msg->opcode].func(resource, msg)) < 0)
			goto invalid_message;
	}
done:
	core->current_client = NULL;
	return;

invalid_method:
	pw_log_error(NAME" %p: invalid method id:%u op:%u",
		     client->protocol, msg->id, msg->opcode);
	pw_resource_error(resource, -EINVAL, "invalid method id:%u op:%u",
			msg->id, msg->opcode);
	pw_client_destroy(client);
	goto done;
invalid_message:
	pw_log_error(NAME" %p: invalid message received id:%u op:%u (%s)",
		     client->protocol, msg->id, msg->opcode, spa_strerror(res));
	pw_resource_error(resource, res, "invalid message received id:%u op:%u (%s)",
			msg->id, msg->opcode, spa_strerror(res));
	spa_debug_pod(0, NULL, (struct spa_pod *)msg->data);
	pw_client_destroy(client);
	goto done;
}

static void
client_busy_changed(void *data, bool busy)
{
	struct client_data *c = data;
	struct pw_client *client = c->client;
	uint32_t mask = SPA_IO_ERR | SPA_IO_HUP;

	c->busy = busy;

	if (!busy)
		mask |= SPA_IO_IN;

	pw_log_debug(NAME" %p: busy changed %d", client->protocol, busy);
	pw_loop_update_io(client->core->main_loop, c->source, mask);

	if (!busy)
		process_messages(c);

}

static void
connection_data(void *data, int fd, uint32_t mask)
{
	struct client_data *this = data;
	struct pw_client *client = this->client;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		pw_log_info(NAME" %p: client %p disconnected", client->protocol, client);
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

	spa_list_remove(&client->protocol_link);

	if (this->source)
		pw_loop_destroy_source(client->protocol->core->main_loop, this->source);
	if (this->connection)
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
	socklen_t len;
	struct ucred ucred;
	struct pw_core *core = protocol->core;
	struct pw_properties *props;
	char buffer[1024];
	struct protocol_data *d = pw_protocol_get_user_data(protocol);

	props = pw_properties_new(PW_KEY_PROTOCOL, "protocol-native", NULL);
	if (props == NULL)
		goto exit;

	len = sizeof(ucred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) < 0) {
		pw_log_error("server %p: no peercred: %m", s);
	} else {
		pw_properties_setf(props, PW_KEY_SEC_PID, "%d", ucred.pid);
		pw_properties_setf(props, PW_KEY_SEC_UID, "%d", ucred.uid);
		pw_properties_setf(props, PW_KEY_SEC_GID, "%d", ucred.gid);
	}

	len = sizeof(buffer);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERSEC, buffer, &len) < 0) {
		pw_log_warn("server %p: no peersec: %m", s);
	} else {
		pw_properties_setf(props, PW_KEY_SEC_LABEL, "%s", buffer);
	}

	pw_properties_setf(props, PW_KEY_MODULE_ID, "%d", d->module->global->id);

	client = pw_client_new(protocol->core,
			       props,
			       sizeof(struct client_data));
	if (client == NULL)
		goto exit;

	this = pw_client_get_user_data(client);
	client->protocol = protocol;
	spa_list_append(&s->this.client_list, &client->protocol_link);

	this->client = client;
	this->source = pw_loop_add_io(pw_core_get_main_loop(core),
				      fd, SPA_IO_ERR | SPA_IO_HUP, true, connection_data, this);
	if (this->source == NULL)
		goto cleanup_client;

	this->connection = pw_protocol_native_connection_new(protocol->core, fd);
	if (this->connection == NULL)
		goto cleanup_client;

	pw_client_add_listener(client, &this->client_listener, &client_events, this);

	if (pw_global_bind(pw_core_get_global(core), client,
			PW_PERM_RWX, PW_VERSION_CORE_PROXY, 0) < 0)
		goto cleanup_client;

	if (pw_client_register(client, NULL) < 0)
		goto cleanup_client;

	if (pw_global_bind(pw_client_get_global(client), client,
			PW_PERM_RWX, PW_VERSION_CLIENT_PROXY, 1) < 0)
		goto cleanup_client;

	return client;

cleanup_client:
	pw_client_destroy(client);
exit:
	return NULL;
}

static int init_socket_name(struct server *s, const char *name)
{
	int name_size;
	const char *runtime_dir;

	if ((runtime_dir = getenv("XDG_RUNTIME_DIR")) == NULL) {
		pw_log_error("server %p: XDG_RUNTIME_DIR not set in the environment", s);
		return -EIO;
	}

	s->addr.sun_family = AF_LOCAL;
	name_size = snprintf(s->addr.sun_path, sizeof(s->addr.sun_path),
			     "%s/%s", runtime_dir, name) + 1;

	if (name_size > (int) sizeof(s->addr.sun_path)) {
		pw_log_error("server %p: socket path \"%s/%s\" plus null terminator exceeds 108 bytes",
				s, runtime_dir, name);
		*s->addr.sun_path = 0;
		return -ENAMETOOLONG;
	}
	return 0;
}

static int lock_socket(struct server *s)
{
	int res;

	snprintf(s->lock_addr, sizeof(s->lock_addr), "%s%s", s->addr.sun_path, LOCK_SUFFIX);

	s->fd_lock = open(s->lock_addr, O_CREAT | O_CLOEXEC,
			  (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP));

	if (s->fd_lock < 0) {
		res = -errno;
		pw_log_error("server %p: unable to open lockfile '%s': %m", s, s->lock_addr);
		goto err;
	}

	if (flock(s->fd_lock, LOCK_EX | LOCK_NB) < 0) {
		res = -errno;
		pw_log_error("server %p: unable to lock lockfile '%s': %m"
				" (maybe another daemon is running)",
				s, s->lock_addr);
		goto err_fd;
	}
	return 0;

err_fd:
	close(s->fd_lock);
	s->fd_lock = -1;
err:
	*s->lock_addr = 0;
	*s->addr.sun_path = 0;
	return res;
}

static void
socket_data(void *data, int fd, uint32_t mask)
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
		pw_log_error("server %p: failed to accept: %m", s);
		return;
	}

	client = client_new(s, client_fd);
	if (client == NULL) {
		pw_log_error("server %p: failed to create client", s);
		close(client_fd);
		return;
	}
	c = client->user_data;

	if (!client->busy)
		pw_loop_update_io(client->protocol->core->main_loop,
			  c->source, SPA_IO_IN | SPA_IO_ERR | SPA_IO_HUP);
}

static int add_socket(struct pw_protocol *protocol, struct server *s)
{
	socklen_t size;
	int fd = -1, res;
	bool activated = false;

#ifdef HAVE_SYSTEMD_DAEMON
	{
		int i, n = sd_listen_fds(0);
		for (i = 0; i < n; ++i) {
			if (sd_is_socket_unix(SD_LISTEN_FDS_START + i, SOCK_STREAM,
						1, s->addr.sun_path, 0) > 0) {
				fd = SD_LISTEN_FDS_START + i;
				activated = true;
				pw_log_info("server %p: Found socket activation socket for '%s'",
						s, s->addr.sun_path);
				break;
			}
		}
	}
#endif

	if (fd < 0) {
		if ((fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
			res = -errno;
			goto error;
		}

		size = offsetof(struct sockaddr_un, sun_path) + strlen(s->addr.sun_path);
		if (bind(fd, (struct sockaddr *) &s->addr, size) < 0) {
			res = -errno;
			pw_log_error("server %p: bind() failed with error: %m", s);
			goto error_close;
		}

		if (listen(fd, 128) < 0) {
			res = -errno;
			pw_log_error("server %p: listen() failed with error: %m", s);
			goto error_close;
		}
	}

	s->activated = activated;
	s->loop = pw_core_get_main_loop(protocol->core);
	if (s->loop == NULL) {
		res = -errno;
		goto error_close;
	}
	s->source = pw_loop_add_io(s->loop, fd, SPA_IO_IN, true, socket_data, s);
	if (s->source == NULL) {
		res = -errno;
		goto error_close;
	}
	return 0;

error_close:
	close(fd);
error:
	return res;

}

static int impl_steal_fd(struct pw_protocol_client *client)
{
	struct client *impl = SPA_CONTAINER_OF(client, struct client, this);
	int fd;

	if (impl->source == NULL)
		return -EIO;

	fd = dup(impl->source->fd);
	if (fd < 0)
		return -errno;

	pw_protocol_client_disconnect(client);
	return fd;
}

static void
on_remote_data(void *data, int fd, uint32_t mask)
{
	struct client *impl = data;
	struct pw_remote *this = impl->this.remote;
	struct pw_protocol_native_connection *conn = impl->connection;
	struct pw_core *core = pw_remote_get_core(this);
	int res;

        if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		res = -EPIPE;
		goto error;
	}

        if (mask & SPA_IO_IN) {
		const struct pw_protocol_native_message *msg;

                while (!impl->disconnecting) {
                        struct pw_proxy *proxy;
                        const struct pw_protocol_native_demarshal *demarshal;
			const struct pw_protocol_marshal *marshal;

			res = pw_protocol_native_connection_get_next(conn, &msg);
			if (res < 0) {
				if (res == -EAGAIN)
					break;
				goto error;
			}
			if (res == 0)
				break;

                        pw_log_trace(NAME" %p: got message %d from %u seq:%d",
					this, msg->opcode, msg->id, msg->seq);

			this->recv_seq = msg->seq;

			if (debug_messages) {
				fprintf(stderr, "<<<<<<<<< in: id:%d op:%d size:%d seq:%d\n",
						msg->id, msg->opcode, msg->size, msg->seq);
			        spa_debug_pod(0, NULL, (struct spa_pod *)msg->data);
			}

			proxy = pw_remote_find_proxy(this, msg->id);
			if (proxy == NULL || proxy->zombie) {
				if (proxy == NULL)
					pw_log_error(NAME" %p: could not find proxy %u", this, msg->id);
				else
					pw_log_debug(NAME" %p: zombie proxy %u", this, msg->id);

				/* FIXME close fds */
				continue;
			}

			marshal = pw_proxy_get_marshal(proxy);
			if (marshal == NULL || msg->opcode >= marshal->n_events) {
				pw_log_error(NAME" %p: invalid method %u for %u (%d)",
						this, msg->opcode, msg->id,
						marshal ? marshal->n_events : (uint32_t)-1);
				continue;
                        }

                        demarshal = marshal->event_demarshal;
			if (!demarshal[msg->opcode].func) {
                                pw_log_error(NAME" %p: function %d not implemented on %u",
						this, msg->opcode, msg->id);
				continue;
			}
			proxy->refcount++;
			res = demarshal[msg->opcode].func(proxy, msg);
			pw_proxy_unref(proxy);

			if (res < 0) {
				pw_log_error (NAME" %p: invalid message received %u for %u",
						this, msg->opcode, msg->id);
				continue;
			}
                }
        }
	return;
error:
	pw_log_error(NAME" %p: got connection error %d (%s)", impl, res, spa_strerror(res));
	pw_loop_destroy_source(pw_core_get_main_loop(core), impl->source);
	impl->source = NULL;
	pw_remote_disconnect(this);
}


static void do_flush_event(void *data, uint64_t count)
{
	struct client *impl = data;
	impl->flush_signaled = false;
	if (impl->connection)
		if (pw_protocol_native_connection_flush(impl->connection) < 0)
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

static int impl_connect_fd(struct pw_protocol_client *client, int fd, bool do_close)
{
	struct client *impl = SPA_CONTAINER_OF(client, struct client, this);
	struct pw_remote *remote = client->remote;
	int res;

	impl->disconnecting = false;

	impl->connection = pw_protocol_native_connection_new(remote->core, fd);
	if (impl->connection == NULL) {
		res = -errno;
		goto error_cleanup;
	}

	impl->source = pw_loop_add_io(remote->core->main_loop,
					fd,
					SPA_IO_IN | SPA_IO_HUP | SPA_IO_ERR,
					do_close, on_remote_data, impl);
	if (impl->source == NULL) {
		res = -errno;
		goto error_cleanup;
	}

	pw_protocol_native_connection_add_listener(impl->connection,
						   &impl->conn_listener,
						   &conn_events,
						   impl);
	return 0;

error_cleanup:
	if (impl->connection) {
		pw_protocol_native_connection_destroy(impl->connection);
		impl->connection = NULL;
	}
	return res;
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
	int res;

	if ((impl = calloc(1, sizeof(struct client))) == NULL)
		return NULL;

	this = &impl->this;
	this->protocol = protocol;
	this->remote = remote;

	if (properties)
		str = pw_properties_get(properties, PW_KEY_REMOTE_INTENTION);
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
	if (impl->flush_event == NULL) {
		res = -errno;
		goto error_cleanup;
	}

	spa_list_append(&protocol->client_list, &this->link);

	return this;

error_cleanup:
	free(impl);
	errno = -res;
	return NULL;
}

static void destroy_server(struct pw_protocol_server *server)
{
	struct server *s = SPA_CONTAINER_OF(server, struct server, this);
	struct pw_client *client, *tmp;

	spa_list_remove(&server->link);
	spa_hook_remove(&s->hook);

	spa_list_for_each_safe(client, tmp, &server->client_list, protocol_link)
		pw_client_destroy(client);

	if (s->source) {
		spa_hook_remove(&s->hook);
		pw_loop_destroy_source(s->loop, s->source);
	}
	if (s->addr.sun_path[0] && !s->activated)
		unlink(s->addr.sun_path);
	if (s->lock_addr[0])
		unlink(s->lock_addr);
	if (s->fd_lock != -1)
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
		name = pw_properties_get(properties, PW_KEY_CORE_NAME);
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
	int res;

	if ((s = calloc(1, sizeof(struct server))) == NULL)
		return NULL;

	s->fd_lock = -1;

	this = &s->this;
	this->protocol = protocol;
	spa_list_init(&this->client_list);
	this->destroy = destroy_server;

	spa_list_append(&protocol->server_list, &this->link);

	name = get_name(pw_core_get_properties(core));

	pw_loop_add_hook(pw_core_get_main_loop(core), &s->hook, &impl_hooks, s);

	if ((res = init_socket_name(s, name)) < 0)
		goto error;

	if ((res = lock_socket(s)) < 0)
		goto error;

	if ((res = add_socket(protocol, s)) < 0)
		goto error;

	pw_log_info(NAME" %p: Added server %p %s", protocol, this, name);

	return this;

error:
	destroy_server(this);
	errno = -res;
	return NULL;
}

const static struct pw_protocol_implementaton protocol_impl = {
	PW_VERSION_PROTOCOL_IMPLEMENTATION,
	.new_client = impl_new_client,
	.add_server = impl_add_server,
};

static struct spa_pod_builder *
impl_ext_begin_proxy(struct pw_proxy *proxy, uint8_t opcode, struct pw_protocol_native_message **msg)
{
	struct client *impl = SPA_CONTAINER_OF(proxy->remote->conn, struct client, this);
	return pw_protocol_native_connection_begin(impl->connection, proxy->id, opcode, msg);
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

static int impl_ext_end_proxy(struct pw_proxy *proxy,
			       struct spa_pod_builder *builder)
{
	struct client *impl = SPA_CONTAINER_OF(proxy->remote->conn, struct client, this);
	struct pw_remote *remote = proxy->remote;
	return remote->send_seq = pw_protocol_native_connection_end(impl->connection, builder);
}

static struct spa_pod_builder *
impl_ext_begin_resource(struct pw_resource *resource,
		uint8_t opcode, struct pw_protocol_native_message **msg)
{
	struct client_data *data = resource->client->user_data;
	return pw_protocol_native_connection_begin(data->connection, resource->id, opcode, msg);
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

static int impl_ext_end_resource(struct pw_resource *resource,
				  struct spa_pod_builder *builder)
{
	struct client_data *data = resource->client->user_data;
	struct pw_client *client = resource->client;
	return client->send_seq = pw_protocol_native_connection_end(data->connection, builder);
}
const static struct pw_protocol_native_ext protocol_ext_impl = {
	PW_VERSION_PROTOCOL_NATIVE_EXT,
	.begin_proxy = impl_ext_begin_proxy,
	.add_proxy_fd = impl_ext_add_proxy_fd,
	.get_proxy_fd = impl_ext_get_proxy_fd,
	.end_proxy = impl_ext_end_proxy,
	.begin_resource = impl_ext_begin_resource,
	.add_resource_fd = impl_ext_add_resource_fd,
	.get_resource_fd = impl_ext_get_resource_fd,
	.end_resource = impl_ext_end_resource,
};

static void module_destroy(void *data)
{
	struct protocol_data *d = data;

	spa_hook_remove(&d->module_listener);

	pw_protocol_destroy(d->protocol);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
};

SPA_EXPORT
int pipewire__module_init(struct pw_module *module, const char *args)
{
	struct pw_core *core = pw_module_get_core(module);
	struct pw_protocol *this;
	const char *val;
	struct protocol_data *d;
	int res;

	if (pw_core_find_protocol(core, PW_TYPE_INFO_PROTOCOL_Native) != NULL)
		return 0;

	this = pw_protocol_new(core, PW_TYPE_INFO_PROTOCOL_Native, sizeof(struct protocol_data));
	if (this == NULL)
		return -errno;

	debug_messages = pw_debug_is_category_enabled("connection");

	this->implementation = &protocol_impl;
	this->extension = &protocol_ext_impl;

	pw_protocol_native_init(this);

	pw_log_debug(NAME" %p: new %d", this, debug_messages);

	d = pw_protocol_get_user_data(this);
	d->protocol = this;
	d->module = module;

	val = getenv("PIPEWIRE_DAEMON");
	if (val == NULL)
		val = pw_properties_get(pw_core_get_properties(core), PW_KEY_CORE_DAEMON);
	if (val && pw_properties_parse_bool(val)) {
		if (impl_add_server(this, core, NULL) == NULL) {
			res = -errno;
			goto error_cleanup;
		}
	}

	pw_module_add_listener(module, &d->module_listener, &module_events, d);

	pw_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error_cleanup:
	pw_protocol_destroy(this);
	return res;
}
