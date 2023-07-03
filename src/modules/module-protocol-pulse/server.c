/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

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
#include <pipewire/cleanup.h>
#include <pipewire/pipewire.h>

#include "client.h"
#include "commands.h"
#include "defs.h"
#include "internal.h"
#include "log.h"
#include "message.h"
#include "reply.h"
#include "server.h"
#include "stream.h"
#include "utils.h"
#include "flatpak-utils.h"

#define LISTEN_BACKLOG 32
#define MAX_CLIENTS 64

static int handle_packet(struct client *client, struct message *msg)
{
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

	const struct command *cmd = &commands[command];
	if (cmd->run == NULL) {
		res = -ENOTSUP;
		goto finish;
	}

	if (!client->authenticated && !SPA_FLAG_IS_SET(cmd->access, COMMAND_ACCESS_WITHOUT_AUTH)) {
		res = -EACCES;
		goto finish;
	}

	if (client->manager == NULL && !SPA_FLAG_IS_SET(cmd->access, COMMAND_ACCESS_WITHOUT_MANAGER)) {
		res = -EACCES;
		goto finish;
	}

	res = cmd->run(client, command, tag, msg);

finish:
	message_free(msg, false, false);
	if (res < 0)
		reply_error(client, command, tag, res);

	return 0;
}

static int handle_memblock(struct client *client, struct message *msg)
{
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
		pw_log_info("client %p [%s]: received memblock for unknown channel %d",
			    client, client->name, channel);
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
	if ((flags & FLAG_SEEKMASK) == SEEK_RELATIVE)
		stream->requested -= diff;

	if (filled < 0) {
		/* underrun, reported on reader side */
	} else if (filled + msg->length > stream->attr.maxlength) {
		/* overrun */
		stream_send_overflow(stream);
	}

	/* always write data to ringbuffer, we expect the other side
	 * to recover */
	spa_ringbuffer_write_data(&stream->ring,
			stream->buffer, MAXLENGTH,
			index % MAXLENGTH,
			msg->data,
			SPA_MIN(msg->length, MAXLENGTH));
	index += msg->length;
	spa_ringbuffer_write_update(&stream->ring, index);

	stream->write_index += msg->length;
	stream->requested -= msg->length;

	stream_send_request(stream);

	if (stream->is_paused && !stream->corked)
		stream_set_paused(stream, false, "new data");

finish:
	message_free(msg, false, false);
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

		if (client->message == NULL || client->message->length < idx) {
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
			if (res != -EAGAIN && res != -EWOULDBLOCK &&
			    res != -EPIPE && res != -ECONNRESET)
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
			message_free(client->message, false, false);

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

	if (mask & SPA_IO_OUT || client->new_msg_since_last_flush) {
		res = client_flush_messages(client);
		if (res < 0)
			goto error;
	}

done:
	/* drop the reference that was acquired at the beginning of the function */
	client_unref(client);
	return;

error:
	switch (res) {
	case -EPIPE:
	case -ECONNRESET:
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
	const char *client_access = NULL;
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

	if (server->n_clients >= server->max_clients) {
		close(client_fd);
		errno = ECONNREFUSED;
		goto error;
	}

	client = client_new(server);
	if (client == NULL)
		goto error;

	pw_log_debug("server %p: new client %p fd:%d", server, client, client_fd);

	client->source = pw_loop_add_io(impl->loop,
					client_fd,
					SPA_IO_ERR | SPA_IO_HUP | SPA_IO_IN,
					true, on_client_data, client);
	if (client->source == NULL)
		goto error;

	client->props = pw_properties_new(
			PW_KEY_CLIENT_API, "pipewire-pulse",
			"config.ext", pw_properties_get(impl->props, "config.ext"),
			NULL);
	if (client->props == NULL)
		goto error;

	pw_properties_setf(client->props,
			"pulse.server.type", "%s",
			server->addr.ss_family == AF_UNIX ? "unix" : "tcp");

	client->routes = pw_properties_new(NULL, NULL);
	if (client->routes == NULL)
		goto error;

	if (server->client_access[0] != '\0')
		client_access = server->client_access;

	if (server->addr.ss_family == AF_UNIX) {
		spa_autofree char *app_id = NULL, *devices = NULL;

#ifdef SO_PRIORITY
		val = 6;
		if (setsockopt(client_fd, SOL_SOCKET, SO_PRIORITY, &val, sizeof(val)) < 0)
			pw_log_warn("setsockopt(SO_PRIORITY) failed: %m");
#endif
		pid = get_client_pid(client, client_fd);
		if (pid != 0 && pw_check_flatpak(pid, &app_id, &devices) == 1) {
			/*
			 * XXX: we should really use Portal client access here
			 *
			 * However, session managers currently support only camera
			 * permissions, and the XDG Portal doesn't have a "Sound Manager"
			 * permission defined. So for now, use access=flatpak, and determine
			 * extra permissions here.
			 *
			 * The application has access to the Pulseaudio socket,
			 * and with real PA it would always then have full sound access.
			 * We'll restrict the full access here behind devices=all;
			 * if the application can access all devices it can then
			 * also sound and camera devices directly, so granting also the
			 * Manager permissions here is reasonable.
			 *
			 * The "Manager" permission in any case is also currently not safe
			 * as the session manager does not check any permission store
			 * for it.
			 */
			client_access = "flatpak";
			pw_properties_set(client->props, "pipewire.access.portal.app_id",
					app_id);

			if (devices && (spa_streq(devices, "all") ||
							spa_strstartswith(devices, "all;") ||
							strstr(devices, ";all;")))
				pw_properties_set(client->props, PW_KEY_MEDIA_CATEGORY, "Manager");
			else
				pw_properties_set(client->props, PW_KEY_MEDIA_CATEGORY, NULL);
		}
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
		if (client_access == NULL)
			client_access = "restricted";
	}
	pw_properties_set(client->props, PW_KEY_CLIENT_ACCESS, client_access);

	return;

error:
	pw_log_error("server %p: failed to create client: %m", server);
	if (client)
		client_free(client);
}

static int parse_unix_address(const char *address, struct sockaddr_storage *addrs, int len)
{
	struct sockaddr_un addr = {0};
	int res;

	if (address[0] != '/') {
		char runtime_dir[PATH_MAX];

		if ((res = get_runtime_dir(runtime_dir, sizeof(runtime_dir))) < 0)
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

	if (len < 1)
		return -ENOSPC;

	addr.sun_family = AF_UNIX;

	memcpy(&addrs[0], &addr, sizeof(addr));
	return 1;
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

	if (chmod(addr_un->sun_path, 0777) < 0)
		pw_log_warn("server %p: chmod('%s') failed: %m",
				    server, addr_un->sun_path);

	if (listen(fd, server->listen_backlog) < 0) {
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
	bool is_ipv6 = false;
	int port;

	switch (addr->ss_family) {
	case AF_INET:
		src = &((struct sockaddr_in *) addr)->sin_addr.s_addr;
		port = ntohs(((struct sockaddr_in *) addr)->sin_port);
		break;
	case AF_INET6:
		is_ipv6 = true;
		src = &((struct sockaddr_in6 *) addr)->sin6_addr.s6_addr;
		port = ntohs(((struct sockaddr_in6 *) addr)->sin6_port);
		break;
	default:
		return -EAFNOSUPPORT;
	}

	if (inet_ntop(addr->ss_family, src, ip, sizeof(ip)) == NULL)
		return -errno;

	return snprintf(buffer, buflen, "%s%s%s:%d",
			is_ipv6 ? "[" : "",
			ip,
			is_ipv6 ? "]" : "",
			port);
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

static int parse_ip_address(const char *address, struct sockaddr_storage *addrs, int len)
{
	char ip[FORMATTED_IP_ADDR_STRLEN];
	struct sockaddr_storage addr;
	int res;

	res = parse_ipv6_address(address, (struct sockaddr_in6 *) &addr);
	if (res == 0) {
		if (len < 1)
			return -ENOSPC;
		addrs[0] = addr;
		return 1;
	}

	res = parse_ipv4_address(address, (struct sockaddr_in *) &addr);
	if (res == 0) {
		if (len < 1)
			return -ENOSPC;
		addrs[0] = addr;
		return 1;
	}

	res = parse_port(address);
	if (res < 0)
		return res;

	if (len < 2)
		return -ENOSPC;

	snprintf(ip, sizeof(ip), "0.0.0.0:%d", res);
	spa_assert_se(parse_ipv4_address(ip, (struct sockaddr_in *) &addr) == 0);
	addrs[0] = addr;

	snprintf(ip, sizeof(ip), "[::]:%d", res);
	spa_assert_se(parse_ipv6_address(ip, (struct sockaddr_in6 *) &addr) == 0);
	addrs[1] = addr;

	return 2;
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

	if (listen(fd, server->listen_backlog) < 0) {
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
	struct impl * const impl = server->impl;
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
	if (res >= 0)
		spa_hook_list_call(&impl->hooks, struct impl_events, server_started, 0, server);

	return res;
}

static int parse_address(const char *address, struct sockaddr_storage *addrs, int len)
{
	if (spa_strstartswith(address, "tcp:"))
		return parse_ip_address(address + strlen("tcp:"), addrs, len);

	if (spa_strstartswith(address, "unix:"))
		return parse_unix_address(address + strlen("unix:"), addrs, len);

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
	int len, res, count = 0, err = 0; /* store the first error to return when no servers could be created */
	const char *v;
	struct spa_json it[3];

	/* update `err` if it hasn't been set to an errno */
#define UPDATE_ERR(e) do { if (err == 0) err = (e); } while (false)

	/* collect addresses into an array of `struct sockaddr_storage` */
	spa_json_init(&it[0], addresses, strlen(addresses));

	/* [ <server-spec> ... ] */
	if (spa_json_enter_array(&it[0], &it[1]) < 0)
		return -EINVAL;

	/* a server-spec is either an address or an object */
	while ((len = spa_json_next(&it[1], &v)) > 0) {
		char addr_str[FORMATTED_SOCKET_ADDR_STRLEN] = { 0 };
		char key[128], client_access[64] = { 0 };
		struct sockaddr_storage addrs[2];
		int i, max_clients = MAX_CLIENTS, listen_backlog = LISTEN_BACKLOG, n_addr;

		if (spa_json_is_object(v, len)) {
			spa_json_enter(&it[1], &it[2]);
			while (spa_json_get_string(&it[2], key, sizeof(key)) > 0) {
				if ((len = spa_json_next(&it[2], &v)) <= 0)
					break;

				if (spa_streq(key, "address")) {
					spa_json_parse_stringn(v, len, addr_str, sizeof(addr_str));
				} else if (spa_streq(key, "max-clients")) {
					spa_json_parse_int(v, len, &max_clients);
				} else if (spa_streq(key, "listen-backlog")) {
					spa_json_parse_int(v, len, &listen_backlog);
				} else if (spa_streq(key, "client.access")) {
					spa_json_parse_stringn(v, len, client_access, sizeof(client_access));
				}
			}
		} else {
			spa_json_parse_stringn(v, len, addr_str, sizeof(addr_str));
		}

		n_addr = parse_address(addr_str, addrs, SPA_N_ELEMENTS(addrs));
		if (n_addr < 0) {
			pw_log_warn("pulse-server %p: failed to parse address '%s': %s",
				    impl, addr_str, spa_strerror(n_addr));
			UPDATE_ERR(n_addr);
			continue;
		}

		/* try to create sockets for each address in the list */
		for (i = 0; i < n_addr; i++) {
			const struct sockaddr_storage *addr = &addrs[i];
			struct server * const server = server_new(impl);

			if (server == NULL) {
				UPDATE_ERR(-errno);
				continue;
			}

			server->max_clients = max_clients;
			server->listen_backlog = listen_backlog;
			memcpy(server->client_access, client_access, sizeof(client_access));

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
	}
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

	spa_hook_list_call(&impl->hooks, struct impl_events, server_stopped, 0, server);

	if (server->source)
		pw_loop_destroy_source(impl->loop, server->source);

	if (server->addr.ss_family == AF_UNIX && !server->activated)
		unlink(((const struct sockaddr_un *) &server->addr)->sun_path);

	free(server);
}
