/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include "config.h"

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/file.h>
#include <ctype.h>
#ifdef HAVE_PWD_H
#include <pwd.h>
#endif
#if defined(__FreeBSD__) || defined(__MidnightBSD__)
#include <sys/ucred.h>
#endif

#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include <pipewire/impl.h>
#include <pipewire/extensions/protocol-native.h>

#include "pipewire/private.h"

#include "modules/module-protocol-native/connection.h"
#include "modules/module-protocol-native/defs.h"
#include "modules/module-protocol-native/protocol-footer.h"


#define NAME "protocol-native"
PW_LOG_TOPIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic
PW_LOG_TOPIC(mod_topic_connection, "conn." NAME);

#undef spa_debug
#define spa_debug(...) pw_logt_debug(mod_topic_connection, __VA_ARGS__)

#include <spa/debug/pod.h>
#include <spa/debug/types.h>

/** \page page_module_protocol_native PipeWire Module: Protocol Native
 *
 * The native protocol module implements the PipeWire communication between
 * a client and a server using unix local sockets.
 *
 * Normally this module is loaded in both client and server config files
 * so that they cam communicate.
 *
 * ## Module Options
 *
 * The module has no options.
 *
 * ## General Options
 *
 * The name of the core is obtained as:
 *
 * - PIPEWIRE_CORE : the environment variable with the name of the core
 * - \ref PW_KEY_CORE_NAME : in the context properties
 * - a name based on the process id
 *
 * The context will also become a server if:
 *
 * - PIPEWIRE_DAEMON : the environment is true
 * - \ref PW_KEY_CORE_DAEMON : in the context properties is true
 *
 * The socket will be located in the directory obtained by looking at the
 * following environment variables:
 *
 * - PIPEWIRE_RUNTIME_DIR
 * - XDG_RUNTIME_DIR
 * - USERPROFILE
 *
 * The socket address will be written into the notification file descriptor
 * if the following environment variable is set:
 *
 * - PIPEWIRE_NOTIFICATION_FD
 *
 * When a client connect, the connection will be made to:
 *
 * - PIPEWIRE_REMOTE : the environment with the remote name
 * - \ref PW_KEY_REMOTE_NAME : the property in the context.
 * - The default remote named "pipewire-0"
 *
 * A Special remote named "internal" can be used to make a connection to the
 * local context. This can be done even when the server is not a daemon. It can
 * be used to treat a local context as if it was a server.
 *
 * ## Example configuration
 *
 *\code{.unparsed}
 * context.modules = [
   { name = libpipewire-module-protocol-native }
 * ]
 *\endcode
 */

#ifndef UNIX_PATH_MAX
#define UNIX_PATH_MAX   108
#endif

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Native protocol using unix sockets" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

/* Required for s390x */
#ifndef SO_PEERSEC
#define SO_PEERSEC 31
#endif

static bool debug_messages = 0;

#define LOCK_SUFFIX     ".lock"
#define LOCK_SUFFIXLEN  5

void pw_protocol_native_init(struct pw_protocol *protocol);
void pw_protocol_native0_init(struct pw_protocol *protocol);

struct protocol_data {
	struct pw_impl_module *module;
	struct spa_hook module_listener;
	struct pw_protocol *protocol;

	struct server *local;
};

struct client {
	struct pw_protocol_client this;
	struct pw_context *context;

	struct spa_source *source;

	struct pw_protocol_native_connection *connection;
	struct spa_hook conn_listener;

	int ref;

	struct footer_core_global_state footer_state;

	unsigned int connected:1;
	unsigned int disconnecting:1;
	unsigned int need_flush:1;
	unsigned int paused:1;
};

static void client_unref(struct client *impl)
{
	if (--impl->ref == 0)
		free(impl);
}

struct server {
	struct pw_protocol_server this;

	int fd_lock;
	struct sockaddr_un addr;
	char lock_addr[UNIX_PATH_MAX + LOCK_SUFFIXLEN];

	struct pw_loop *loop;
	struct spa_source *source;
	struct spa_source *resume;
	unsigned int activated:1;
};

struct client_data {
	struct pw_impl_client *client;
	struct spa_hook client_listener;

	struct spa_list protocol_link;
	struct server *server;

	struct spa_source *source;
	struct pw_protocol_native_connection *connection;
	struct spa_hook conn_listener;

	struct footer_client_global_state footer_state;

	unsigned int busy:1;
	unsigned int need_flush:1;

	struct protocol_compat_v2 compat_v2;
};

static void debug_msg(const char *prefix, const struct pw_protocol_native_message *msg, bool hex)
{
	struct spa_pod *pod;
	pw_logt_debug(mod_topic_connection,
		      "%s: id:%d op:%d size:%d seq:%d fds:%d", prefix,
		      msg->id, msg->opcode, msg->size, msg->seq, msg->n_fds);

	if ((pod = get_first_pod_from_data(msg->data, msg->size, 0)) != NULL)
		spa_debug_pod(0, NULL, pod);
	else
		hex = true;
	if (hex)
		spa_debug_mem(0, msg->data, msg->size);

	pw_logt_debug(mod_topic_connection, "%s ****", prefix);

}

static void pre_demarshal(struct pw_protocol_native_connection *conn,
		const struct pw_protocol_native_message *msg,
		void *object, const struct footer_demarshal *opcodes, size_t n_opcodes)
{
	struct spa_pod *footer = NULL;
	struct spa_pod_parser parser;
	struct spa_pod_frame f[2];
	uint32_t opcode;
	int ret;

	footer = pw_protocol_native_connection_get_footer(conn, msg);
	if (footer == NULL)
		return;   /* No valid footer. Ignore silently. */

	/*
	 * Version 3 footer
	 *
	 * spa_pod Struct { [Id opcode, Struct { ... }]* }
	 */

	spa_pod_parser_pod(&parser, footer);
	if (spa_pod_parser_push_struct(&parser, &f[0]) < 0) {
		pw_log_error("malformed message footer");
		return;
	}

	while (1) {
		if (spa_pod_parser_get_id(&parser, &opcode) < 0)
			break;
		if (spa_pod_parser_push_struct(&parser, &f[1]) < 0)
			break;
		if (opcode < n_opcodes) {
			if ((ret = opcodes[opcode].demarshal(object, &parser)) < 0)
				pw_log_error("failed processing message footer (opcode %u): %d (%s)",
						opcode, ret, spa_strerror(ret));
		} else {
			/* Ignore (don't log errors), in case we need to extend this later. */
			pw_log_debug("unknown message footer opcode %u", opcode);
		}
		spa_pod_parser_pop(&parser, &f[1]);
	}
}

static int
process_messages(struct client_data *data)
{
	struct pw_protocol_native_connection *conn = data->connection;
	struct pw_impl_client *client = data->client;
	struct pw_context *context = client->context;
	const struct pw_protocol_native_message *msg;
	struct pw_resource *resource;
	int res;

	context->current_client = client;

	/* when the client is busy processing an async action, stop processing messages
	 * for the client until it finishes the action */
	while (!data->busy) {
		const struct pw_protocol_native_demarshal *demarshal;
	        const struct pw_protocol_marshal *marshal;
		uint32_t permissions, required;

		res = pw_protocol_native_connection_get_next(conn, &msg);
		if (res < 0) {
			if (res == -EAGAIN)
				break;
			goto error;
		}
		if (res == 0)
			break;

		if (client->core_resource == NULL) {
			pw_log_debug("%p: no core resource", client);
			res = -EPROTO;
			goto error;
		}

		client->recv_seq = msg->seq;

		pw_log_trace("%p: got message %d from %u", client->protocol,
			     msg->opcode, msg->id);

		if (debug_messages)
			debug_msg("<<<<<< in", msg, false);

		pre_demarshal(conn, msg, client, footer_client_demarshal,
				SPA_N_ELEMENTS(footer_client_demarshal));

		resource = pw_impl_client_find_resource(client, msg->id);
		if (resource == NULL) {
			pw_resource_errorf(client->core_resource,
					-ENOENT, "unknown resource %u op:%u", msg->id, msg->opcode);
			continue;
		}

		marshal = pw_resource_get_marshal(resource);
		if (marshal == NULL || msg->opcode >= marshal->n_client_methods) {
			pw_resource_errorf_id(resource, msg->id,
					-ENOSYS, "invalid method id:%u op:%u",
					msg->id, msg->opcode);
			continue;
		}

		demarshal = marshal->server_demarshal;
		if (!demarshal[msg->opcode].func) {
			pw_resource_errorf_id(resource, msg->id,
					-ENOTSUP, "function not supported id:%u op:%u",
					msg->id, msg->opcode);
			continue;
		}

		permissions = pw_resource_get_permissions(resource);
		required = demarshal[msg->opcode].permissions | PW_PERM_X;

		if ((required & permissions) != required) {
			pw_resource_errorf_id(resource, msg->id,
				-EACCES, "no permission to call method %u on %u "
				"(requires "PW_PERMISSION_FORMAT", have "PW_PERMISSION_FORMAT")",
				msg->opcode, msg->id,
				PW_PERMISSION_ARGS(required), PW_PERMISSION_ARGS(permissions));
			continue;
		}

		resource->refcount++;
		pw_protocol_native_connection_enter(conn);
		res = demarshal[msg->opcode].func(resource, msg);
		pw_protocol_native_connection_leave(conn);
		pw_resource_unref(resource);

		if (res < 0) {
			pw_resource_errorf_id(resource, msg->id,
					res, "invalid message id:%u op:%u (%s)",
					msg->id, msg->opcode, spa_strerror(res));
			debug_msg("*invalid message*", msg, true);
		}
	}
	res = 0;
done:
	context->current_client = NULL;

	return res;

error:
	pw_resource_errorf(client->core_resource, res, "client error %d (%s)",
				res, spa_strerror(res));
	goto done;
}

static void
client_busy_changed(void *data, bool busy)
{
	struct client_data *c = data;
	struct server *s = c->server;
	struct pw_impl_client *client = c->client;
	uint32_t mask = c->source->mask;

	c->busy = busy;

	SPA_FLAG_UPDATE(mask, SPA_IO_IN, !busy);

	pw_log_debug("%p: busy changed %d", client->protocol, busy);
	pw_loop_update_io(client->context->main_loop, c->source, mask);

	if (!busy)
		pw_loop_signal_event(s->loop, s->resume);
}

static void handle_client_error(struct pw_impl_client *client, int res, const char *msg)
{
	if (res == -EPIPE || res == -ECONNRESET)
		pw_log_info("%p: %s: client %p disconnected", client->protocol, msg, client);
	else
		pw_log_error("%p: %s: client %p error %d (%s)", client->protocol, msg,
				client, res, spa_strerror(res));
	if (!client->destroyed)
		pw_impl_client_destroy(client);
}

static void
connection_data(void *data, int fd, uint32_t mask)
{
	struct client_data *this = data;
	struct pw_impl_client *client = this->client;
	int res;

	client->refcount++;

	if (mask & SPA_IO_HUP) {
		res = -EPIPE;
		goto error;
	}
	if (mask & SPA_IO_ERR) {
		res = -EIO;
		goto error;
	}
	if (mask & SPA_IO_IN) {
		if ((res = process_messages(this)) < 0)
			goto error;
	}
	if (mask & SPA_IO_OUT || this->need_flush) {
		this->need_flush = false;
		res = pw_protocol_native_connection_flush(this->connection);
		if (res >= 0) {
			pw_loop_update_io(client->context->main_loop,
					this->source, this->source->mask & ~SPA_IO_OUT);
		} else if (res != -EAGAIN)
			goto error;
	}
done:
	pw_impl_client_unref(client);
	return;
error:
	handle_client_error(client, res, "connection_data");
	goto done;
}

static void client_destroy(void *data)
{
	struct client_data *this = data;
	pw_log_debug("%p: destroy", this);
	spa_list_remove(&this->protocol_link);
}

static void client_free(void *data)
{
	struct client_data *this = data;
	struct pw_impl_client *client = this->client;

	pw_log_debug("%p: free", this);
	spa_hook_remove(&this->client_listener);

	if (this->source)
		pw_loop_destroy_source(client->context->main_loop, this->source);
	if (this->connection)
		pw_protocol_native_connection_destroy(this->connection);

	pw_map_clear(&this->compat_v2.types);
}

static const struct pw_impl_client_events client_events = {
	PW_VERSION_IMPL_CLIENT_EVENTS,
	.destroy = client_destroy,
	.free = client_free,
	.busy_changed = client_busy_changed,
};

static void on_server_connection_destroy(void *data)
{
	struct client_data *this = data;
	spa_hook_remove(&this->conn_listener);
}

static void on_start(void *data, uint32_t version)
{
	struct client_data *this = data;
	struct pw_impl_client *client = this->client;

	pw_log_debug("version %d", version);

	if (client->core_resource != NULL)
		pw_resource_remove(client->core_resource);

	if (pw_global_bind(pw_impl_core_get_global(client->core), client,
			PW_PERM_ALL, version, 0) < 0)
		return;

	if (version == 0)
		client->compat_v2 = &this->compat_v2;

	return;
}

static void on_server_need_flush(void *data)
{
	struct client_data *this = data;
	struct pw_impl_client *client = this->client;

	pw_log_trace("need flush");
	this->need_flush = true;

	if (this->source && !(this->source->mask & SPA_IO_OUT)) {
		pw_loop_update_io(client->context->main_loop,
				this->source, this->source->mask | SPA_IO_OUT);
	}
}

static const struct pw_protocol_native_connection_events server_conn_events = {
	PW_VERSION_PROTOCOL_NATIVE_CONNECTION_EVENTS,
	.destroy = on_server_connection_destroy,
	.start = on_start,
	.need_flush = on_server_need_flush,
};

static bool check_print(const uint8_t *buffer, int len)
{
	int i;
	while (len > 1 && buffer[len-1] == 0)
		len--;
	for (i = 0; i < len; i++)
		if (!isprint(buffer[i]))
			return false;
	return true;
}

static struct client_data *client_new(struct server *s, int fd)
{
	struct client_data *this;
	struct pw_impl_client *client;
	struct pw_protocol *protocol = s->this.protocol;
	socklen_t len;
#if defined(__FreeBSD__) || defined(__MidnightBSD__)
	struct xucred xucred;
#else
	struct ucred ucred;
#endif
	struct pw_context *context = protocol->context;
	struct pw_properties *props;
	uint8_t buffer[1024];
	struct protocol_data *d = pw_protocol_get_user_data(protocol);
	int i, res;

	props = pw_properties_new(PW_KEY_PROTOCOL, "protocol-native", NULL);
	if (props == NULL)
		goto exit;

#if defined(__linux__)
	len = sizeof(ucred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) < 0) {
		pw_log_warn("server %p: no peercred: %m", s);
	} else {
		pw_properties_setf(props, PW_KEY_SEC_PID, "%d", ucred.pid);
		pw_properties_setf(props, PW_KEY_SEC_UID, "%d", ucred.uid);
		pw_properties_setf(props, PW_KEY_SEC_GID, "%d", ucred.gid);
	}

	len = sizeof(buffer);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERSEC, buffer, &len) < 0) {
		if (errno == ENOPROTOOPT)
			pw_log_info("server %p: security label not available", s);
		else
			pw_log_warn("server %p: security label error: %m", s);
	} else {
		if (!check_print(buffer, len)) {
			char *hex, *p;
			static const char *ch = "0123456789abcdef";

			p = hex = alloca(len * 2 + 10);
			p += snprintf(p, 5, "hex:");
			for(i = 0; i < (int)len; i++)
				p += snprintf(p, 3, "%c%c",
						ch[buffer[i] >> 4], ch[buffer[i] & 0xf]);
			pw_properties_set(props, PW_KEY_SEC_LABEL, hex);

		} else {
			/* buffer is not null terminated, must use length explicitly */
			pw_properties_setf(props, PW_KEY_SEC_LABEL, "%.*s",
					(int)len, buffer);
		}
	}
#elif defined(__FreeBSD__) || defined(__MidnightBSD__)
	len = sizeof(xucred);
	if (getsockopt(fd, 0, LOCAL_PEERCRED, &xucred, &len) < 0) {
		pw_log_warn("server %p: no peercred: %m", s);
	} else {
#if __FreeBSD__ >= 13
		pw_properties_setf(props, PW_KEY_SEC_PID, "%d", xucred.cr_pid);
#endif
		pw_properties_setf(props, PW_KEY_SEC_UID, "%d", xucred.cr_uid);
		pw_properties_setf(props, PW_KEY_SEC_GID, "%d", xucred.cr_gid);
		// this is what Linuxulator does at the moment, see sys/compat/linux/linux_socket.c
		pw_properties_set(props, PW_KEY_SEC_LABEL, "unconfined");
	}
#endif

	pw_properties_setf(props, PW_KEY_MODULE_ID, "%d", d->module->global->id);

	client = pw_context_create_client(s->this.core,
			protocol, props, sizeof(struct client_data));
	if (client == NULL)
		goto exit;

	this = pw_impl_client_get_user_data(client);
	spa_list_append(&s->this.client_list, &this->protocol_link);

	this->server = s;
	this->client = client;
	pw_map_init(&this->compat_v2.types, 0, 32);

	pw_impl_client_add_listener(client, &this->client_listener, &client_events, this);

	this->source = pw_loop_add_io(pw_context_get_main_loop(context),
				      fd, SPA_IO_ERR | SPA_IO_HUP, true,
				      connection_data, this);
	if (this->source == NULL) {
		res = -errno;
		goto cleanup_client;
	}

	this->connection = pw_protocol_native_connection_new(protocol->context, fd);
	if (this->connection == NULL) {
		res = -errno;
		goto cleanup_client;
	}

	pw_protocol_native_connection_add_listener(this->connection,
						   &this->conn_listener,
						   &server_conn_events,
						   this);

	if ((res = pw_impl_client_register(client, NULL)) < 0)
		goto cleanup_client;

	if (!client->busy)
		pw_loop_update_io(pw_context_get_main_loop(context),
				this->source, this->source->mask | SPA_IO_IN);

	return this;

cleanup_client:
	pw_impl_client_destroy(client);
	errno = -res;
exit:
	return NULL;
}

static const char *
get_runtime_dir(void)
{
	const char *runtime_dir;

	runtime_dir = getenv("PIPEWIRE_RUNTIME_DIR");
	if (runtime_dir == NULL)
		runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (runtime_dir == NULL)
		runtime_dir = getenv("USERPROFILE");
	return runtime_dir;
}


static int init_socket_name(struct server *s, const char *name)
{
	int name_size;
	const char *runtime_dir;
	bool path_is_absolute;

	path_is_absolute = name[0] == '/';

	runtime_dir = get_runtime_dir();

	pw_log_debug("name:%s runtime_dir:%s", name, runtime_dir);

	if (runtime_dir == NULL && !path_is_absolute) {
		pw_log_error("server %p: name %s is not an absolute path and no runtime dir found. "
				"Set one of PIPEWIRE_RUNTIME_DIR, XDG_RUNTIME_DIR or "
				"USERPROFILE in the environment", s, name);
		return -ENOENT;
	}

	s->addr.sun_family = AF_LOCAL;
	if (path_is_absolute)
		name_size = snprintf(s->addr.sun_path, sizeof(s->addr.sun_path),
			     "%s", name) + 1;
	else
		name_size = snprintf(s->addr.sun_path, sizeof(s->addr.sun_path),
			     "%s/%s", runtime_dir, name) + 1;

	if (name_size > (int) sizeof(s->addr.sun_path)) {
		if (path_is_absolute)
			pw_log_error("server %p: socket path \"%s\" plus null terminator exceeds %i bytes",
				s, name, (int) sizeof(s->addr.sun_path));
		else
			pw_log_error("server %p: socket path \"%s/%s\" plus null terminator exceeds %i bytes",
				s, runtime_dir, name, (int) sizeof(s->addr.sun_path));
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
	struct client_data *client;
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
}

static int write_socket_address(struct server *s)
{
	long v;
	int fd, res = 0;
	char *endptr;
	const char *env = getenv("PIPEWIRE_NOTIFICATION_FD");

	if (env == NULL || env[0] == '\0')
		return 0;

	errno = 0;
	v = strtol(env, &endptr, 10);
	if (endptr[0] != '\0')
		errno = EINVAL;
	if (errno != 0) {
		res = -errno;
		pw_log_error("server %p: strtol() failed with error: %m", s);
		goto error;
	}
	fd = (int)v;
	if (v != fd) {
		res = -ERANGE;
		pw_log_error("server %p: invalid fd %ld: %s", s, v, spa_strerror(res));
		goto error;
	}
	if (dprintf(fd, "%s\n", s->addr.sun_path) < 0) {
		res = -errno;
		pw_log_error("server %p: dprintf() failed with error: %m", s);
		goto error;
	}
	close(fd);
	unsetenv("PIPEWIRE_NOTIFICATION_FD");
	return 0;

error:
	return res;
}

static int add_socket(struct pw_protocol *protocol, struct server *s)
{
	socklen_t size;
	int fd = -1, res;
	bool activated = false;

#ifdef HAVE_SYSTEMD
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
		struct stat socket_stat;

		if ((fd = socket(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
			res = -errno;
			goto error;
		}
		if (stat(s->addr.sun_path, &socket_stat) < 0) {
			if (errno != ENOENT) {
				res = -errno;
				pw_log_error("server %p: stat %s failed with error: %m",
						s, s->addr.sun_path);
				goto error_close;
			}
		} else if (socket_stat.st_mode & S_IWUSR || socket_stat.st_mode & S_IWGRP) {
			unlink(s->addr.sun_path);
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

	res = write_socket_address(s);
	if (res < 0) {
		pw_log_error("server %p: failed to write socket address: %s", s,
				spa_strerror(res));
		goto error_close;
	}
	s->activated = activated;
	s->loop = pw_context_get_main_loop(protocol->context);
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

	fd = fcntl(impl->source->fd, F_DUPFD_CLOEXEC, 3);
	if (fd < 0)
		return -errno;

	pw_protocol_client_disconnect(client);
	return fd;
}

static int
process_remote(struct client *impl)
{
	const struct pw_protocol_native_message *msg;
	struct pw_protocol_native_connection *conn = impl->connection;
	struct pw_core *this = impl->this.core;
	int res = 0;

	impl->ref++;
	while (!impl->disconnecting && !impl->paused) {
		struct pw_proxy *proxy;
		const struct pw_protocol_native_demarshal *demarshal;
		const struct pw_protocol_marshal *marshal;

		res = pw_protocol_native_connection_get_next(conn, &msg);
		if (res < 0) {
			if (res == -EAGAIN)
				res = 0;
			break;
		}
		if (res == 0)
			break;

		pw_log_trace("%p: got message %d from %u seq:%d",
			this, msg->opcode, msg->id, msg->seq);

		this->recv_seq = msg->seq;

		if (debug_messages)
			debug_msg("<<<<<< in", msg, false);

		pre_demarshal(conn, msg, this, footer_core_demarshal,
				SPA_N_ELEMENTS(footer_core_demarshal));

		proxy = pw_core_find_proxy(this, msg->id);
		if (proxy == NULL || proxy->zombie) {
			uint32_t i;

			if (proxy == NULL)
				pw_log_error("%p: could not find proxy %u", this, msg->id);
			else
				pw_log_debug("%p: zombie proxy %u", this, msg->id);

			/* close fds */
			for (i = 0; i < msg->n_fds; i++) {
				pw_log_debug("%p: close fd:%d", conn, msg->fds[i]);
				close(msg->fds[i]);
			}
			continue;
		}

		marshal = pw_proxy_get_marshal(proxy);
		if (marshal == NULL || msg->opcode >= marshal->n_server_methods) {
			pw_log_error("%p: invalid method %u for %u (%d)",
					this, msg->opcode, msg->id,
					marshal ? marshal->n_server_methods : (uint32_t)-1);
			continue;
		}

		demarshal = marshal->client_demarshal;
		if (!demarshal[msg->opcode].func) {
                               pw_log_error("%p: function %d not implemented on %u",
					this, msg->opcode, msg->id);
			continue;
		}
		proxy->refcount++;
		pw_protocol_native_connection_enter(conn);
		res = demarshal[msg->opcode].func(proxy, msg);
		pw_protocol_native_connection_leave(conn);
		pw_proxy_unref(proxy);

		if (res < 0) {
			pw_log_error("%p: invalid message received %u for %u: %s",
					this, msg->opcode, msg->id, spa_strerror(res));
			debug_msg("*invalid*", msg, true);
		}
		res = 0;
	}
	client_unref(impl);
	return res;
}

static void
on_remote_data(void *data, int fd, uint32_t mask)
{
	struct client *impl = data;
	struct pw_core *this = impl->this.core;
	struct pw_proxy *core_proxy = (struct pw_proxy*)this;
	struct pw_protocol_native_connection *conn = impl->connection;
	struct pw_context *context = pw_core_get_context(this);
	struct pw_loop *loop = pw_context_get_main_loop(context);
	int res;

	core_proxy->refcount++;
	impl->ref++;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		res = -EPIPE;
		goto error;
	}
	if (mask & SPA_IO_IN) {
		if ((res = process_remote(impl)) < 0)
			goto error;
	}
	if (mask & SPA_IO_OUT || impl->need_flush) {
		if (!impl->connected) {
			socklen_t len = sizeof res;

			if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &res, &len) < 0) {
				res = -errno;
				pw_log_error("getsockopt: %m");
				goto error;
			}
			if (res != 0) {
				res = -res;
				goto error;
			}
			impl->connected = true;
			pw_log_debug("%p: connected, fd %d", impl, fd);
		}
		impl->need_flush = false;
		res = pw_protocol_native_connection_flush(conn);
		if (res >= 0) {
			pw_loop_update_io(loop, impl->source,
					impl->source->mask & ~SPA_IO_OUT);
		} else if (res != -EAGAIN)
			goto error;
	}

done:
	client_unref(impl);
	pw_proxy_unref(core_proxy);
	return;
error:
	pw_log_debug("%p: got connection error %d (%s)", impl, res, spa_strerror(res));
	if (impl->source) {
		pw_loop_destroy_source(loop, impl->source);
		impl->source = NULL;
	}
	pw_proxy_notify(core_proxy,
			struct pw_core_events, error, 0, 0,
			this->recv_seq, res, "connection error");
	goto done;
}

static int impl_connect_fd(struct pw_protocol_client *client, int fd, bool do_close)
{
	struct client *impl = SPA_CONTAINER_OF(client, struct client, this);

	impl->connected = false;
	impl->disconnecting = false;

	pw_protocol_native_connection_set_fd(impl->connection, fd);
	impl->source = pw_loop_add_io(impl->context->main_loop,
					fd,
					SPA_IO_IN | SPA_IO_OUT | SPA_IO_HUP | SPA_IO_ERR,
					do_close, on_remote_data, impl);
	if (impl->source == NULL)
		return -errno;

	return 0;
}

static void impl_disconnect(struct pw_protocol_client *client)
{
	struct client *impl = SPA_CONTAINER_OF(client, struct client, this);

	impl->disconnecting = true;

	if (impl->source)
                pw_loop_destroy_source(impl->context->main_loop, impl->source);
	impl->source = NULL;

	pw_protocol_native_connection_set_fd(impl->connection, -1);
}

static void impl_destroy(struct pw_protocol_client *client)
{
	struct client *impl = SPA_CONTAINER_OF(client, struct client, this);

	impl_disconnect(client);

	if (impl->connection)
                pw_protocol_native_connection_destroy(impl->connection);
	impl->connection = NULL;

	spa_list_remove(&client->link);
	client_unref(impl);
}

static int impl_set_paused(struct pw_protocol_client *client, bool paused)
{
	struct client *impl = SPA_CONTAINER_OF(client, struct client, this);
	uint32_t mask;

	if (impl->source == NULL)
		return -EIO;

	mask = impl->source->mask;

	impl->paused = paused;

	SPA_FLAG_UPDATE(mask, SPA_IO_IN, !paused);

	pw_log_debug("%p: paused %d", client->protocol, paused);
	pw_loop_update_io(impl->context->main_loop, impl->source, mask);

	return paused ? 0 : process_remote(impl);
}

static int pw_protocol_native_connect_internal(struct pw_protocol_client *client,
					    const struct spa_dict *props,
					    void (*done_callback) (void *data, int res),
					    void *data)
{
	int res, sv[2];
	struct pw_protocol *protocol = client->protocol;
	struct protocol_data *d = pw_protocol_get_user_data(protocol);
	struct server *s = d->local;
	struct pw_permission permissions[1];
	struct client_data *c;

	pw_log_debug("server %p: internal connect", s);

	if (socketpair(PF_LOCAL, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0, sv) < 0) {
		res = -errno;
		pw_log_error("server %p: socketpair() failed with error: %m", s);
		goto error;
	}

	c = client_new(s, sv[0]);
	if (c == NULL) {
		res = -errno;
		pw_log_error("server %p: failed to create client: %m", s);
		goto error_close;
	}
	permissions[0] = PW_PERMISSION_INIT(PW_ID_ANY, PW_PERM_ALL);
	pw_impl_client_update_permissions(c->client, 1, permissions);

	res = pw_protocol_client_connect_fd(client, sv[1], true);
done:
	if (done_callback)
		done_callback(data, res);
	return res;

error_close:
	close(sv[0]);
	close(sv[1]);
error:
	goto done;
}

static void on_client_connection_destroy(void *data)
{
	struct client *impl = data;
	spa_hook_remove(&impl->conn_listener);
}

static void on_client_need_flush(void *data)
{
        struct client *impl = data;

	pw_log_trace("need flush");
	impl->need_flush = true;

	if (impl->source && !(impl->source->mask & SPA_IO_OUT)) {
		pw_loop_update_io(impl->context->main_loop,
				impl->source, impl->source->mask | SPA_IO_OUT);
	}
}

static const struct pw_protocol_native_connection_events client_conn_events = {
	PW_VERSION_PROTOCOL_NATIVE_CONNECTION_EVENTS,
	.destroy = on_client_connection_destroy,
	.need_flush = on_client_need_flush,
};

static struct pw_protocol_client *
impl_new_client(struct pw_protocol *protocol,
		struct pw_core *core,
		const struct spa_dict *props)
{
	struct client *impl;
	struct pw_protocol_client *this;
	const char *str = NULL;
	int res;

	if ((impl = calloc(1, sizeof(struct client))) == NULL)
		return NULL;

	pw_log_debug("%p: new client %p", protocol, impl);

	this = &impl->this;
	this->protocol = protocol;
	this->core = core;

	impl->ref = 1;
	impl->context = protocol->context;
	impl->connection = pw_protocol_native_connection_new(protocol->context, -1);
	if (impl->connection == NULL) {
		res = -errno;
		goto error_free;
	}
	pw_protocol_native_connection_add_listener(impl->connection,
						   &impl->conn_listener,
						   &client_conn_events,
						   impl);

	if (props) {
		str = spa_dict_lookup(props, PW_KEY_REMOTE_INTENTION);
		if (str == NULL &&
		   (str = spa_dict_lookup(props, PW_KEY_REMOTE_NAME)) != NULL &&
		    spa_streq(str, "internal"))
			str = "internal";
	}
	if (str == NULL)
		str = "generic";

	pw_log_debug("%p: connect %s", protocol, str);

	if (spa_streq(str, "screencast"))
		this->connect = pw_protocol_native_connect_portal_screencast;
	else if (spa_streq(str, "internal"))
		this->connect = pw_protocol_native_connect_internal;
	else
		this->connect = pw_protocol_native_connect_local_socket;

	this->steal_fd = impl_steal_fd;
	this->connect_fd = impl_connect_fd;
	this->disconnect = impl_disconnect;
	this->destroy = impl_destroy;
	this->set_paused = impl_set_paused;

	spa_list_append(&protocol->client_list, &this->link);

	return this;

error_free:
	free(impl);
	errno = -res;
	return NULL;
}

static void destroy_server(struct pw_protocol_server *server)
{
	struct server *s = SPA_CONTAINER_OF(server, struct server, this);
	struct client_data *data, *tmp;

	pw_log_debug("%p: server %p", s->this.protocol, s);

	spa_list_remove(&server->link);

	spa_list_for_each_safe(data, tmp, &server->client_list, protocol_link)
		pw_impl_client_destroy(data->client);

	if (s->source)
		pw_loop_destroy_source(s->loop, s->source);
	if (s->resume)
		pw_loop_destroy_source(s->loop, s->resume);
	if (s->addr.sun_path[0] && !s->activated)
		unlink(s->addr.sun_path);
	if (s->lock_addr[0])
		unlink(s->lock_addr);
	if (s->fd_lock != -1)
		close(s->fd_lock);
	free(s);
}

static void do_resume(void *_data, uint64_t count)
{
	struct server *server = _data;
	struct pw_protocol_server *this = &server->this;
	struct client_data *data, *tmp;
	int res;

	pw_log_debug("flush");

	spa_list_for_each_safe(data, tmp, &this->client_list, protocol_link) {
		data->client->refcount++;
		if ((res = process_messages(data)) < 0)
			handle_client_error(data->client, res, "do_resume");
		pw_impl_client_unref(data->client);
	}
	return;
}

static const char *
get_server_name(const struct spa_dict *props)
{
	const char *name = NULL;

	name = getenv("PIPEWIRE_CORE");
	if (name == NULL && props != NULL)
		name = spa_dict_lookup(props, PW_KEY_CORE_NAME);
	if (name == NULL)
		name = PW_DEFAULT_REMOTE;
	return name;
}

static struct server *
create_server(struct pw_protocol *protocol,
		struct pw_impl_core *core,
                const struct spa_dict *props)
{
	struct pw_protocol_server *this;
	struct server *s;

	if ((s = calloc(1, sizeof(struct server))) == NULL)
		return NULL;

	s->fd_lock = -1;

	this = &s->this;
	this->protocol = protocol;
	this->core = core;
	spa_list_init(&this->client_list);
	this->destroy = destroy_server;

	spa_list_append(&protocol->server_list, &this->link);

	pw_log_debug("%p: created server %p", protocol, this);

	return s;
}

static struct pw_protocol_server *
impl_add_server(struct pw_protocol *protocol,
		struct pw_impl_core *core,
                const struct spa_dict *props)
{
	struct pw_protocol_server *this;
	struct server *s;
	const char *name;
	int res;

	if ((s = create_server(protocol, core, props)) == NULL)
		return NULL;

	this = &s->this;

	name = get_server_name(props);

	if ((res = init_socket_name(s, name)) < 0)
		goto error;

	if ((res = lock_socket(s)) < 0)
		goto error;

	if ((res = add_socket(protocol, s)) < 0)
		goto error;

	if ((s->resume = pw_loop_add_event(s->loop, do_resume, s)) == NULL)
		goto error;

	pw_log_info("%p: Listening on '%s'", protocol, name);

	return this;

error:
	destroy_server(this);
	errno = -res;
	return NULL;
}

static const struct pw_protocol_implementation protocol_impl = {
	PW_VERSION_PROTOCOL_IMPLEMENTATION,
	.new_client = impl_new_client,
	.add_server = impl_add_server,
};

static struct spa_pod_builder *
impl_ext_begin_proxy(struct pw_proxy *proxy, uint8_t opcode, struct pw_protocol_native_message **msg)
{
	struct client *impl = SPA_CONTAINER_OF(proxy->core->conn, struct client, this);
	return pw_protocol_native_connection_begin(impl->connection, proxy->id, opcode, msg);
}

static uint32_t impl_ext_add_proxy_fd(struct pw_proxy *proxy, int fd)
{
	struct client *impl = SPA_CONTAINER_OF(proxy->core->conn, struct client, this);
	return pw_protocol_native_connection_add_fd(impl->connection, fd);
}

static int impl_ext_get_proxy_fd(struct pw_proxy *proxy, uint32_t index)
{
	struct client *impl = SPA_CONTAINER_OF(proxy->core->conn, struct client, this);
	return pw_protocol_native_connection_get_fd(impl->connection, index);
}

static void assert_single_pod(struct spa_pod_builder *builder)
{
	/*
	 * Check the invariant that the message we just marshaled
	 * consists of at most one POD.
	 */
	struct spa_pod *pod = builder->data;
	spa_assert(builder->data == NULL ||
			builder->state.offset < sizeof(struct spa_pod) ||
			builder->state.offset == SPA_POD_SIZE(pod));
}

static int impl_ext_end_proxy(struct pw_proxy *proxy,
			       struct spa_pod_builder *builder)
{
	struct pw_core *core = proxy->core;
	struct client *impl = SPA_CONTAINER_OF(core->conn, struct client, this);
	assert_single_pod(builder);
	marshal_core_footers(&impl->footer_state, core, builder);
	return core->send_seq = pw_protocol_native_connection_end(impl->connection, builder);
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
	struct pw_impl_client *client = resource->client;
	assert_single_pod(builder);
	marshal_client_footers(&data->footer_state, client, builder);
	return client->send_seq = pw_protocol_native_connection_end(data->connection, builder);
}
static const struct pw_protocol_native_ext protocol_ext_impl = {
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

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static int need_server(struct pw_context *context, const struct spa_dict *props)
{
	const char *val = NULL;

	val = getenv("PIPEWIRE_DAEMON");
	if (val == NULL && props != NULL)
		val = spa_dict_lookup(props, PW_KEY_CORE_DAEMON);
	if (val && pw_properties_parse_bool(val))
		return 1;
	return 0;
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_protocol *this;
	struct pw_impl_core *core = context->core;
	struct protocol_data *d;
	const struct pw_properties *props;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);
	PW_LOG_TOPIC_INIT(mod_topic_connection);

	if (pw_context_find_protocol(context, PW_TYPE_INFO_PROTOCOL_Native) != NULL) {
		pw_log_error("protocol %s is already loaded", PW_TYPE_INFO_PROTOCOL_Native);
		return -EEXIST;
	}

	this = pw_protocol_new(context, PW_TYPE_INFO_PROTOCOL_Native, sizeof(struct protocol_data));
	if (this == NULL)
		return -errno;

	debug_messages = mod_topic_connection->level >= SPA_LOG_LEVEL_DEBUG;

	this->implementation = &protocol_impl;
	this->extension = &protocol_ext_impl;

	pw_protocol_native_init(this);
	pw_protocol_native0_init(this);

	pw_log_debug("%p: new debug:%d", this, debug_messages);

	d = pw_protocol_get_user_data(this);
	d->protocol = this;
	d->module = module;

	props = pw_context_get_properties(context);
	d->local = create_server(this, core, &props->dict);

	if (need_server(context, &props->dict)) {
		if (impl_add_server(this, core, &props->dict) == NULL) {
			res = -errno;
			goto error_cleanup;
		}
	}

	pw_impl_module_add_listener(module, &d->module_listener, &module_events, d);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error_cleanup:
	pw_protocol_destroy(this);
	return res;
}
