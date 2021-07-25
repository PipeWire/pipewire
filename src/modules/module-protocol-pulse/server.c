/* PipeWire
 *
 * Copyright © 2020 Wim Taymans
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

#include "config.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netinet/ip.h>
#include <unistd.h>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

#include <spa/utils/defs.h>
#include <spa/utils/json.h>
#include <spa/utils/result.h>
#include <pipewire/pipewire.h>

#include "client.h"
#include "commands.h"
#include "defs.h"
#include "internal.h"
#include "message.h"
#include "reply.h"
#include "server.h"
#include "stream.h"
#include "utils.h"

#define LISTEN_BACKLOG 32
#define MAX_CLIENTS 64

static int handle_packet(struct client *client, struct message *msg)
{
	struct impl * const impl = client->impl;
	uint32_t command, tag;
	int res = 0;

	if (message_get(msg,
			TAG_U32, &command,
			TAG_U32, &tag,
			TAG_INVALID) < 0) {
		res = -EPROTO;
		goto finish;
	}

	pw_log_debug("client %p: received packet command:%u tag:%u",
		     client, command, tag);

	if (command >= COMMAND_MAX) {
		res = -EINVAL;
		goto finish;
	}

	if (debug_messages) {
		pw_log_debug("client %p: command:%s", client, commands[command].name);
		message_dump(SPA_LOG_LEVEL_INFO, msg);
	}

	if (commands[command].run == NULL) {
		res = -ENOTSUP;
		goto finish;
	}

	res = commands[command].run(client, command, tag, msg);

finish:
	message_free(impl, msg, false, false);
	if (res < 0)
		reply_error(client, command, tag, res);

	return 0;
}

static int handle_memblock(struct client *client, struct message *msg)
{
	struct impl * const impl = client->impl;
	struct stream *stream;
	uint32_t channel, flags, index;
	int64_t offset, diff;
	int32_t filled;
	int res = 0;

	channel = ntohl(client->desc.channel);
	offset = (int64_t) (
		(((uint64_t) ntohl(client->desc.offset_hi)) << 32) |
		(((uint64_t) ntohl(client->desc.offset_lo))));
	flags = ntohl(client->desc.flags);

	pw_log_debug("client %p: received memblock channel:%d offset:%" PRIi64 " flags:%08x size:%u",
		     client, channel, offset, flags, msg->length);

	stream = pw_map_lookup(&client->streams, channel);
	if (stream == NULL || stream->type == STREAM_TYPE_RECORD) {
		res = -EINVAL;
		goto finish;
	}

	filled = spa_ringbuffer_get_write_index(&stream->ring, &index);
	pw_log_debug("new block %p %p/%u filled:%d index:%d flags:%02x offset:%" PRIu64,
		     msg, msg->data, msg->length, filled, index, flags, offset);

	switch (flags & FLAG_SEEKMASK) {
	case SEEK_RELATIVE:
		diff = offset;
		break;
	case SEEK_ABSOLUTE:
		diff = offset - (int64_t)stream->write_index;
		break;
	case SEEK_RELATIVE_ON_READ:
	case SEEK_RELATIVE_END:
		diff = offset - (int64_t)filled;
		break;
	default:
		pw_log_warn("client %p [%s]: received memblock frame with invalid seek mode: %" PRIu32,
			    client, client->name, (uint32_t)(flags & FLAG_SEEKMASK));
		res = -EPROTO;
		goto finish;
	}

	index += diff;
	filled += diff;
	stream->write_index += diff;
	stream->missing -= diff;

	if (filled < 0) {
		/* underrun, reported on reader side */
	} else if (filled + msg->length > stream->attr.maxlength) {
		/* overrun */
		stream_send_overflow(stream);
	}

	/* always write data to ringbuffer, we expect the other side
	 * to recover */
	spa_ringbuffer_write_data(&stream->ring,
			stream->buffer, stream->attr.maxlength,
			index % stream->attr.maxlength,
			msg->data,
			SPA_MIN(msg->length, stream->attr.maxlength));
	index += msg->length;
	stream->write_index += msg->length;
	spa_ringbuffer_write_update(&stream->ring, index);
	stream->requested -= SPA_MIN(msg->length, stream->requested);

finish:
	message_free(impl, msg, false, false);
	return res;
}

static int do_read(struct client *client)
{
	struct impl * const impl = client->impl;
	size_t size;
	int res = 0;
	void *data;

	if (client->in_index < sizeof(client->desc)) {
		data = SPA_PTROFF(&client->desc, client->in_index, void);
		size = sizeof(client->desc) - client->in_index;
	} else {
		uint32_t idx = client->in_index - sizeof(client->desc);

		if (client->message == NULL) {
			res = -EPROTO;
			goto exit;
		}

		data = SPA_PTROFF(client->message->data, idx, void);
		size = client->message->length - idx;
	}

	while (true) {
		ssize_t r = recv(client->source->fd, data, size, MSG_DONTWAIT);

		if (r == 0 && size != 0) {
			res = -EPIPE;
			goto exit;
		} else if (r < 0) {
			if (errno == EINTR)
				continue;
			res = -errno;
			if (res != -EAGAIN && res != -EWOULDBLOCK)
				pw_log_warn("recv client:%p res %zd: %m", client, r);
			goto exit;
		}

		client->in_index += r;
		break;
	}

	if (client->in_index == sizeof(client->desc)) {
		uint32_t flags, length, channel;

		flags = ntohl(client->desc.flags);
		if ((flags & FLAG_SHMMASK) != 0) {
			res = -EPROTO;
			goto exit;
		}

		length = ntohl(client->desc.length);
		if (length > FRAME_SIZE_MAX_ALLOW || length <= 0) {
			pw_log_warn("client %p: received invalid frame size: %u",
				    client, length);
			res = -EPROTO;
			goto exit;
		}

		channel = ntohl(client->desc.channel);
		if (channel == (uint32_t) -1) {
			if (flags != 0) {
				pw_log_warn("client %p: received packet frame with invalid flags",
					    client);
				res = -EPROTO;
				goto exit;
			}
		}

		if (client->message)
			message_free(impl, client->message, false, false);

		client->message = message_alloc(impl, channel, length);
	} else if (client->message &&
	    client->in_index >= client->message->length + sizeof(client->desc)) {
		struct message * const msg = client->message;

		client->message = NULL;
		client->in_index = 0;

		if (msg->channel == (uint32_t)-1)
			res = handle_packet(client, msg);
		else
			res = handle_memblock(client, msg);
	}

exit:
	return res;
}

static void
on_client_data(void *data, int fd, uint32_t mask)
{
	struct client * const client = data;
	struct impl * const impl = client->impl;
	int res;

	client->ref++;

	if (mask & SPA_IO_HUP) {
		res = -EPIPE;
		goto error;
	}

	if (mask & SPA_IO_ERR) {
		res = -EIO;
		goto error;
	}

	if (mask & SPA_IO_IN) {
		pw_log_trace("client %p: can read", client);
		while (true) {
			res = do_read(client);
			if (res < 0) {
				if (res != -EAGAIN && res != -EWOULDBLOCK)
					goto error;
				break;
			}
		}
	}

	if (mask & SPA_IO_OUT || client->need_flush) {
		pw_log_trace("client %p: can write", client);
		client->need_flush = false;
		res = client_flush_messages(client);
		if (res >= 0) {
			int m = client->source->mask;
			SPA_FLAG_CLEAR(m, SPA_IO_OUT);
			pw_loop_update_io(impl->loop, client->source, m);
		} else if (res != -EAGAIN && res != -EWOULDBLOCK)
			goto error;
	}

done:
	/* drop the reference that was acquired at the beginning of the function */
	client_unref(client);
	return;

error:
	switch (res) {
	case -EPIPE:
		pw_log_info("server %p: client %p [%s] disconnected",
			    client->server, client, client->name);
		SPA_FALLTHROUGH;
	case -EPROTO:
		/*
		 * drop the server's reference to the client
		 * (if it hasn't been dropped already),
		 * it is guaranteed that this will not call `client_free()`
		 * since at the beginning of this function an extra reference
		 * has been acquired which will keep the client alive
		 */
		if (client_detach(client))
			client_unref(client);

		/* then disconnect the client */
		client_disconnect(client);
		break;
	default:
		pw_log_error("server %p: client %p [%s] error %d (%s)",
			     client->server, client, client->name, res, spa_strerror(res));
		break;
	}

	goto done;
}

static void
on_connect(void *data, int fd, uint32_t mask)
{
	struct server * const server = data;
	struct impl * const impl = server->impl;
	struct sockaddr_storage name;
	socklen_t length;
	int client_fd, val;
	struct client *client = NULL;
	pid_t pid;

	length = sizeof(name);
	client_fd = accept4(fd, (struct sockaddr *) &name, &length, SOCK_CLOEXEC);
	if (client_fd < 0) {
		if (errno == EMFILE || errno == ENFILE) {
			if (server->n_clients > 0) {
				int m = server->source->mask;
				SPA_FLAG_CLEAR(m, SPA_IO_IN);
				pw_loop_update_io(impl->loop, server->source, m);
				server->wait_clients++;
			}
		}
		goto error;
	}

	if (server->n_clients >= MAX_CLIENTS) {
		close(client_fd);
		errno = ECONNREFUSED;
		goto error;
	}

	client = calloc(1, sizeof(*client));
	if (client == NULL)
		goto error;

	client->impl = impl;
	client->ref = 1;
	client->connect_tag = SPA_ID_INVALID;
	client->server = server;
	spa_list_append(&server->clients, &client->link);
	server->n_clients++;
	pw_map_init(&client->streams, 16, 16);
	spa_list_init(&client->out_messages);
	spa_list_init(&client->operations);
	spa_list_init(&client->pending_samples);

	pw_log_debug("server %p: new client %p fd:%d", server, client, client_fd);

	client->source = pw_loop_add_io(impl->loop,
					client_fd,
					SPA_IO_ERR | SPA_IO_HUP | SPA_IO_IN,
					true, on_client_data, client);
	if (client->source == NULL)
		goto error;

	client->props = pw_properties_new(
			PW_KEY_CLIENT_API, "pipewire-pulse",
			NULL);
	if (client->props == NULL)
		goto error;

	pw_properties_setf(client->props,
			"pulse.server.type", "%s",
			server->addr.ss_family == AF_UNIX ? "unix" : "tcp");

	client->routes = pw_properties_new(NULL, NULL);
	if (client->routes == NULL)
		goto error;

	if (server->addr.ss_family == AF_UNIX) {
#ifdef SO_PRIORITY
		val = 6;
		if (setsockopt(client_fd, SOL_SOCKET, SO_PRIORITY, &val, sizeof(val)) < 0)
			pw_log_warn("setsockopt(SO_PRIORITY) failed: %m");
#endif
		pid = get_client_pid(client, client_fd);
		if (pid != 0 && check_flatpak(client, pid) == 1)
			pw_properties_set(client->props, PW_KEY_CLIENT_ACCESS, "flatpak");
	}
	else if (server->addr.ss_family == AF_INET || server->addr.ss_family == AF_INET6) {
		val = 1;
		if (setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &val, sizeof(val)) < 0)
			pw_log_warn("setsockopt(TCP_NODELAY) failed: %m");

		if (server->addr.ss_family == AF_INET) {
			val = IPTOS_LOWDELAY;
			if (setsockopt(client_fd, IPPROTO_IP, IP_TOS, &val, sizeof(val)) < 0)
				pw_log_warn("setsockopt(IP_TOS) failed: %m");
		}

		pw_properties_set(client->props, PW_KEY_CLIENT_ACCESS, "restricted");
	}

	return;

error:
	pw_log_error("server %p: failed to create client: %m", server);
	if (client)
		client_free(client);
}

static int parse_unix_address(const char *address, struct pw_array *addrs)
{
	struct sockaddr_un addr = {0}, *s;
	int res;

	if (address[0] != '/') {
		char runtime_dir[PATH_MAX];

		if ((res = get_runtime_dir(runtime_dir, sizeof(runtime_dir), "pulse")) < 0)
			return res;

		res = snprintf(addr.sun_path, sizeof(addr.sun_path),
			       "%s/%s", runtime_dir, address);
	}
	else {
		res = snprintf(addr.sun_path, sizeof(addr.sun_path),
			       "%s", address);
	}

	if (res < 0)
		return -EINVAL;

	if ((size_t) res >= sizeof(addr.sun_path)) {
		pw_log_warn("'%s...' too long", addr.sun_path);
		return -ENAMETOOLONG;
	}

	s = pw_array_add(addrs, sizeof(struct sockaddr_storage));
	if (s == NULL)
		return -ENOMEM;

	addr.sun_family = AF_UNIX;

	*s = addr;

	return 0;
}

#ifndef SUN_LEN
#define SUN_LEN(addr_un) \
	(offsetof(struct sockaddr_un, sun_path) + strlen((addr_un)->sun_path))
#endif

static bool is_stale_socket(int fd, const struct sockaddr_un *addr_un)
{
	if (connect(fd, (const struct sockaddr *) addr_un, SUN_LEN(addr_un)) < 0) {
		if (errno == ECONNREFUSED)
			return true;
	}

	return false;
}

#ifdef HAVE_SYSTEMD
static int check_systemd_activation(const char *path)
{
	const int n = sd_listen_fds(0);

	for (int i = 0; i < n; i++) {
		const int fd = SD_LISTEN_FDS_START + i;

		if (sd_is_socket_unix(fd, SOCK_STREAM, 1, path, 0) > 0)
			return fd;
	}

	return -1;
}
#else
static inline int check_systemd_activation(SPA_UNUSED const char *path)
{
	return -1;
}
#endif

static int start_unix_server(struct server *server, const struct sockaddr_storage *addr)
{
	const struct sockaddr_un * const addr_un = (const struct sockaddr_un *) addr;
	struct stat socket_stat;
	int fd, res;

	spa_assert(addr_un->sun_family == AF_UNIX);

	fd = check_systemd_activation(addr_un->sun_path);
	if (fd >= 0) {
		server->activated = true;
		pw_log_info("server %p: found systemd socket activation socket for '%s'",
			    server, addr_un->sun_path);
		goto done;
	}
	else {
		server->activated = false;
	}

	fd = socket(addr_un->sun_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (fd < 0) {
		res = -errno;
		pw_log_info("server %p: socket() failed: %m", server);
		goto error;
	}

	if (stat(addr_un->sun_path, &socket_stat) < 0) {
		if (errno != ENOENT) {
			res = -errno;
			pw_log_warn("server %p: stat('%s') failed: %m",
				    server, addr_un->sun_path);
			goto error_close;
		}
	}
	else if (socket_stat.st_mode & S_IWUSR || socket_stat.st_mode & S_IWGRP) {
		if (!S_ISSOCK(socket_stat.st_mode)) {
			res = -EEXIST;
			pw_log_warn("server %p: '%s' exists and is not a socket",
				    server, addr_un->sun_path);
			goto error_close;
		}

		/* socket is there, check if it's stale */
		if (!is_stale_socket(fd, addr_un)) {
			res = -EADDRINUSE;
			pw_log_warn("server %p: socket '%s' is in use",
				    server, addr_un->sun_path);
			goto error_close;
		}

		pw_log_warn("server %p: unlinking stale socket '%s'",
			    server, addr_un->sun_path);

		if (unlink(addr_un->sun_path) < 0)
			pw_log_warn("server %p: unlink('%s') failed: %m",
				    server, addr_un->sun_path);
	}

	if (bind(fd, (const struct sockaddr *) addr_un, SUN_LEN(addr_un)) < 0) {
		res = -errno;
		pw_log_warn("server %p: bind() to '%s' failed: %m",
			    server, addr_un->sun_path);
		goto error_close;
	}

	if (listen(fd, LISTEN_BACKLOG) < 0) {
		res = -errno;
		pw_log_warn("server %p: listen() on '%s' failed: %m",
			    server, addr_un->sun_path);
		goto error_close;
	}

	pw_log_info("server %p: listening on unix:%s", server, addr_un->sun_path);

done:
	server->addr = *addr;

	return fd;

error_close:
	close(fd);
error:
	return res;
}

static int parse_port(const char *port)
{
	const char *end;
	long p;

	if (port[0] == ':')
		port += 1;

	errno = 0;
	p = strtol(port, (char **) &end, 0);

	if (errno != 0)
		return -errno;

	if (end == port || *end != '\0')
		return -EINVAL;

	if (!(1 <= p && p <= 65535))
		return -EINVAL;

	return p;
}

static int parse_ipv6_address(const char *address, struct sockaddr_in6 *out)
{
	char addr_str[INET6_ADDRSTRLEN];
	struct sockaddr_in6 addr = {0};
	const char *end;
	size_t len;
	int res;

	if (address[0] != '[')
		return -EINVAL;

	address += 1;

	end = strchr(address, ']');
	if (end == NULL)
		return -EINVAL;

	len = end - address;
	if (len >= sizeof(addr_str))
		return -ENAMETOOLONG;

	memcpy(addr_str, address, len);
	addr_str[len] = '\0';

	res = inet_pton(AF_INET6, addr_str, &addr.sin6_addr.s6_addr);
	if (res < 0)
		return -errno;
	if (res == 0)
		return -EINVAL;

	res = parse_port(end + 1);
	if (res < 0)
		return res;

	addr.sin6_port = htons(res);
	addr.sin6_family = AF_INET6;

	*out = addr;

	return 0;
}

static int parse_ipv4_address(const char *address, struct sockaddr_in *out)
{
	char addr_str[INET_ADDRSTRLEN];
	struct sockaddr_in addr = {0};
	size_t len;
	int res;

	len = strspn(address, "0123456789.");
	if (len == 0)
		return -EINVAL;
	if (len >= sizeof(addr_str))
		return -ENAMETOOLONG;

	memcpy(addr_str, address, len);
	addr_str[len] = '\0';

	res = inet_pton(AF_INET, addr_str, &addr.sin_addr.s_addr);
	if (res < 0)
		return -errno;
	if (res == 0)
		return -EINVAL;

	res = parse_port(address + len);
	if (res < 0)
		return res;

	addr.sin_port = htons(res);
	addr.sin_family = AF_INET;

	*out = addr;

	return 0;
}

#define FORMATTED_IP_ADDR_STRLEN (INET6_ADDRSTRLEN + 2 + 1 + 5)

static int format_ip_address(const struct sockaddr_storage *addr, char *buffer, size_t buflen)
{
	char ip[INET6_ADDRSTRLEN];
	const void *src;
	const char *fmt;
	int port;

	switch (addr->ss_family) {
	case AF_INET:
		fmt = "%s:%d";
		src = &((struct sockaddr_in *) addr)->sin_addr.s_addr;
		port = ntohs(((struct sockaddr_in *) addr)->sin_port);
		break;
	case AF_INET6:
		fmt = "[%s]:%d";
		src = &((struct sockaddr_in6 *) addr)->sin6_addr.s6_addr;
		port = ntohs(((struct sockaddr_in6 *) addr)->sin6_port);
		break;
	default:
		return -EAFNOSUPPORT;
	}

	if (inet_ntop(addr->ss_family, src, ip, sizeof(ip)) == NULL)
		return -errno;

	return snprintf(buffer, buflen, fmt, ip, port);
}

static int get_ip_address_length(const struct sockaddr_storage *addr)
{
	switch (addr->ss_family) {
	case AF_INET:
		return sizeof(struct sockaddr_in);
	case AF_INET6:
		return sizeof(struct sockaddr_in6);
	default:
		return -EAFNOSUPPORT;
	}
}

static int parse_ip_address(const char *address, struct pw_array *addrs)
{
	char ip[FORMATTED_IP_ADDR_STRLEN];
	struct sockaddr_storage addr, *s;
	int res;

	res = parse_ipv6_address(address, (struct sockaddr_in6 *) &addr);
	if (res == 0) {
		s = pw_array_add(addrs, sizeof(*s));
		if (s == NULL)
			return -ENOMEM;

		*s = addr;
		return 0;
	}

	res = parse_ipv4_address(address, (struct sockaddr_in *) &addr);
	if (res == 0) {
		s = pw_array_add(addrs, sizeof(*s));
		if (s == NULL)
			return -ENOMEM;

		*s = addr;
		return 0;
	}

	res = parse_port(address);
	if (res < 0)
		return res;

	s = pw_array_add(addrs, sizeof(*s) * 2);
	if (s == NULL)
		return -ENOMEM;

	snprintf(ip, sizeof(ip), "[::]:%d", res);
	spa_assert_se(parse_ipv6_address(ip, (struct sockaddr_in6 *) &addr) == 0);
	*s++ = addr;

	snprintf(ip, sizeof(ip), "0.0.0.0:%d", res);
	spa_assert_se(parse_ipv4_address(ip, (struct sockaddr_in *) &addr) == 0);
	*s++ = addr;

	return 0;
}

static int start_ip_server(struct server *server, const struct sockaddr_storage *addr)
{
	char ip[FORMATTED_IP_ADDR_STRLEN];
	int fd, res;

	spa_assert(addr->ss_family == AF_INET || addr->ss_family == AF_INET6);

	fd = socket(addr->ss_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, IPPROTO_TCP);
	if (fd < 0) {
		res = -errno;
		pw_log_warn("server %p: socket() failed: %m", server);
		goto error;
	}

	{
		int on = 1;
		if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) < 0)
			pw_log_warn("server %p: setsockopt(SO_REUSEADDR) failed: %m", server);
	}

	if (addr->ss_family == AF_INET6) {
		int on = 1;
		if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &on, sizeof(on)) < 0)
			pw_log_warn("server %p: setsockopt(IPV6_V6ONLY) failed: %m", server);
	}

	if (bind(fd, (const struct sockaddr *) addr, get_ip_address_length(addr)) < 0) {
		res = -errno;
		pw_log_warn("server %p: bind() failed: %m", server);
		goto error_close;
	}

	if (listen(fd, LISTEN_BACKLOG) < 0) {
		res = -errno;
		pw_log_warn("server %p: listen() failed: %m", server);
		goto error_close;
	}

	spa_assert_se(format_ip_address(addr, ip, sizeof(ip)) >= 0);
	pw_log_info("server %p: listening on tcp:%s", server, ip);

	server->addr = *addr;

	return fd;

error_close:
	close(fd);
error:
	return res;
}

static struct server *server_new(struct impl *impl)
{
	struct server * const server = calloc(1, sizeof(*server));
	if (server == NULL)
		return NULL;

	server->impl = impl;
	server->addr.ss_family = AF_UNSPEC;
	spa_list_init(&server->clients);
	spa_list_append(&impl->servers, &server->link);

	pw_log_debug("server %p: new", server);

	return server;
}

static int server_start(struct server *server, const struct sockaddr_storage *addr)
{
	const struct impl * const impl = server->impl;
	int res = 0, fd;

	switch (addr->ss_family) {
	case AF_INET:
	case AF_INET6:
		fd = start_ip_server(server, addr);
		break;
	case AF_UNIX:
		fd = start_unix_server(server, addr);
		break;
	default:
		/* shouldn't happen */
		fd = -EAFNOSUPPORT;
		break;
	}

	if (fd < 0)
		return fd;

	server->source = pw_loop_add_io(impl->loop, fd, SPA_IO_IN, true, on_connect, server);
	if (server->source == NULL) {
		res = -errno;
		pw_log_error("server %p: can't create server source: %m", impl);
	}

	return res;
}

static int parse_address(const char *address, struct pw_array *addrs)
{
	if (strncmp(address, "tcp:", strlen("tcp:")) == 0)
		return parse_ip_address(address + strlen("tcp:"), addrs);

	if (strncmp(address, "unix:", strlen("unix:")) == 0)
		return parse_unix_address(address + strlen("unix:"), addrs);

	return -EAFNOSUPPORT;
}

#define SUN_PATH_SIZE (sizeof(((struct sockaddr_un *) NULL)->sun_path))
#define FORMATTED_UNIX_ADDR_STRLEN (SUN_PATH_SIZE + 5)
#define FORMATTED_TCP_ADDR_STRLEN (FORMATTED_IP_ADDR_STRLEN + 4)
#define FORMATTED_SOCKET_ADDR_STRLEN \
	(FORMATTED_UNIX_ADDR_STRLEN > FORMATTED_TCP_ADDR_STRLEN ? \
		FORMATTED_UNIX_ADDR_STRLEN : \
		FORMATTED_TCP_ADDR_STRLEN)

static int format_socket_address(const struct sockaddr_storage *addr, char *buffer, size_t buflen)
{
	if (addr->ss_family == AF_INET || addr->ss_family == AF_INET6) {
		char ip[FORMATTED_IP_ADDR_STRLEN];

		spa_assert_se(format_ip_address(addr, ip, sizeof(ip)) >= 0);

		return snprintf(buffer, buflen, "tcp:%s", ip);
	}
	else if (addr->ss_family == AF_UNIX) {
		const struct sockaddr_un *addr_un = (const struct sockaddr_un *) addr;

		return snprintf(buffer, buflen, "unix:%s", addr_un->sun_path);
	}

	return -EAFNOSUPPORT;
}

int servers_create_and_start(struct impl *impl, const char *addresses, struct pw_array *servers)
{
	struct pw_array addrs = PW_ARRAY_INIT(sizeof(struct sockaddr_storage));
	const struct sockaddr_storage *addr;
	char addr_str[FORMATTED_SOCKET_ADDR_STRLEN];
	int res, count = 0, err = 0; /* store the first error to return when no servers could be created */
	struct spa_json it[2];

	/* update `err` if it hasn't been set to an errno */
#define UPDATE_ERR(e) do { if (err == 0) err = (e); } while (false)

	/* collect addresses into an array of `struct sockaddr_storage` */
	spa_json_init(&it[0], addresses, strlen(addresses));

	if (spa_json_enter_array(&it[0], &it[1]) < 0)
		return -EINVAL;

	while (spa_json_get_string(&it[1], addr_str, sizeof(addr_str) - 1) > 0) {
		res = parse_address(addr_str, &addrs);
		if (res < 0) {
			pw_log_warn("pulse-server %p: failed to parse address '%s': %s",
				    impl, addr_str, spa_strerror(res));

			UPDATE_ERR(res);
		}
	}

	/* try to create sockets for each address in the list */
	pw_array_for_each (addr, &addrs) {
		struct server * const server = server_new(impl);
		if (server == NULL) {
			UPDATE_ERR(-errno);
			continue;
		}

		res = server_start(server, addr);
		if (res < 0) {
			spa_assert_se(format_socket_address(addr, addr_str, sizeof(addr_str)) >= 0);
			pw_log_warn("pulse-server %p: failed to start server on '%s': %s",
				    impl, addr_str, spa_strerror(res));

			UPDATE_ERR(res);
			server_free(server);

			continue;
		}

		if (servers != NULL)
			pw_array_add_ptr(servers, server);

		count += 1;
	}

	pw_array_clear(&addrs);

	if (count == 0) {
		UPDATE_ERR(-EINVAL);
		return err;
	}

	return count;

#undef UPDATE_ERR
}

void server_free(struct server *server)
{
	struct impl * const impl = server->impl;
	struct client *c, *t;

	pw_log_debug("server %p: free", server);

	spa_list_remove(&server->link);

	spa_list_for_each_safe(c, t, &server->clients, link) {
		spa_assert_se(client_detach(c));
		client_unref(c);
	}

	if (server->source)
		pw_loop_destroy_source(impl->loop, server->source);

	if (server->addr.ss_family == AF_UNIX && !server->activated)
		unlink(((const struct sockaddr_un *) &server->addr)->sun_path);

	free(server);
}
