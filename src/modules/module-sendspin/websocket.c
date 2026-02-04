/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <netdb.h>

#include <spa/utils/result.h>
#include <spa/debug/mem.h>

#include "config.h"

#include "websocket.h"
#include "teeny-sha1.c"
#include "../network-utils.h"
#include "../module-raop/base64.h"

#define pw_websocket_emit(o,m,v,...) 	spa_hook_list_call(&o->listener_list, struct pw_websocket_events, m, v, ##__VA_ARGS__)
#define pw_websocket_emit_destroy(w)		pw_websocket_emit(w, destroy, 0)
#define pw_websocket_emit_connected(w,u,c,p)	pw_websocket_emit(w, connected, 0, u, c, p)

#define pw_websocket_connection_emit(o,m,v,...) 	spa_hook_list_call(&o->listener_list, struct pw_websocket_connection_events, m, v, ##__VA_ARGS__)
#define pw_websocket_connection_emit_destroy(w)		pw_websocket_connection_emit(w, destroy, 0)
#define pw_websocket_connection_emit_error(w,r,m)	pw_websocket_connection_emit(w, error, 0, r, m)
#define pw_websocket_connection_emit_disconnected(w)	pw_websocket_connection_emit(w, disconnected, 0)
#define pw_websocket_connection_emit_drained(w)		pw_websocket_connection_emit(w, drained, 0)
#define pw_websocket_connection_emit_message(w,...)	pw_websocket_connection_emit(w, message, 0, __VA_ARGS__)

#define MAX_CONNECTIONS		64

struct message {
	struct spa_list link;
	size_t len;
	size_t offset;
	uint32_t seq;
	int (*reply) (void *user_data, int status);
	void *user_data;
	unsigned char data[];
};

struct server {
	struct pw_websocket *ws;
	struct spa_list link;

	struct sockaddr_storage addr;
	struct spa_source *source;

	void *user;
	char **paths;

	struct spa_list connections;
	uint32_t n_connections;
};

struct pw_websocket_connection {
	struct pw_websocket *ws;
	struct spa_list link;

	int refcount;

	void *user;
	struct server *server;

	struct spa_hook_list listener_list;

	struct spa_source *source;
	unsigned int connecting:1;
	unsigned int need_flush:1;

	char *host;
	char *path;
	char name[128];
	bool ipv4;
	uint16_t port;

	struct sockaddr_storage addr;

	uint8_t maskbit;

	int status;
	char message[128];
	char key[25];
	size_t content_length;

	uint32_t send_seq;
	uint32_t recv_seq;
	bool draining;

	struct spa_list messages;
	struct spa_list pending;

	struct pw_array data;
	size_t data_wanted;
	size_t data_cursor;
	size_t data_state;
	int (*have_data) (struct pw_websocket_connection *conn,
			void *data, size_t size, size_t current);
};

struct pw_websocket {
	struct pw_loop *loop;

	struct spa_hook_list listener_list;

	struct spa_source *source;

	char *ifname;
	char *ifaddress;
	char *user_agent;
	char *server_name;

	struct spa_list connections;
	struct spa_list servers;
};

void pw_websocket_connection_disconnect(struct pw_websocket_connection *conn, bool drain)
{
	struct message *msg;

	if (drain && !spa_list_is_empty(&conn->messages)) {
		conn->draining = true;
		return;
	}

	if (conn->source != NULL) {
		pw_loop_destroy_source(conn->ws->loop, conn->source);
		conn->source = NULL;
	}
	spa_list_insert_list(&conn->messages, &conn->pending);
	spa_list_consume(msg, &conn->messages, link) {
		spa_list_remove(&msg->link);
		free(msg);
	}
	if (conn->server) {
		conn->server->n_connections--;
		conn->server = NULL;
	}
	pw_websocket_connection_emit_disconnected(conn);
}

static void websocket_connection_unref(struct pw_websocket_connection *conn)
{
	if (--conn->refcount > 0)
		return;
	pw_array_clear(&conn->data);
	free(conn->host);
	free(conn->path);
	free(conn);
}

void pw_websocket_connection_destroy(struct pw_websocket_connection *conn)
{
	pw_log_debug("destroy connection %p", conn);
	spa_list_remove(&conn->link);

	pw_websocket_connection_emit_destroy(conn);

	pw_websocket_connection_disconnect(conn, false);
	spa_hook_list_clean(&conn->listener_list);

	websocket_connection_unref(conn);
}

void pw_websocket_connection_add_listener(struct pw_websocket_connection *conn,
		struct spa_hook *listener,
		const struct pw_websocket_connection_events *events, void *data)
{
	spa_hook_list_append(&conn->listener_list, listener, events, data);
}

struct pw_websocket *pw_websocket_new(struct pw_loop *main_loop, struct spa_dict *props)
{
	struct pw_websocket *ws;
	uint32_t i;

	if ((ws = calloc(1, sizeof(*ws))) == NULL)
		return NULL;

	for (i = 0; props && i < props->n_items; i++) {
		const char *k = props->items[i].key;
		const char *s = props->items[i].value;
		if (spa_streq(k, "local.ifname"))
			ws->ifname = s ? strdup(s) : NULL;
		if (spa_streq(k, "local.ifaddress"))
			ws->ifaddress = s ? strdup(s) : NULL;
		if (spa_streq(k, "http.user-agent"))
			ws->user_agent = s ? strdup(s) : NULL;
		if (spa_streq(k, "http.server-name"))
			ws->server_name = s ? strdup(s) : NULL;
	}
	if (ws->user_agent == NULL)
		ws->user_agent = spa_aprintf("PipeWire/%s", PACKAGE_VERSION);
	if (ws->server_name == NULL)
		ws->server_name = spa_aprintf("PipeWire/%s", PACKAGE_VERSION);

	ws->loop = main_loop;
	spa_hook_list_init(&ws->listener_list);

	spa_list_init(&ws->connections);
	spa_list_init(&ws->servers);
	return ws;
}

static void server_free(struct server *server)
{
	struct pw_websocket *ws = server->ws;
	struct pw_websocket_connection *conn;

	pw_log_debug("%p: free server %p", ws, server);

	spa_list_remove(&server->link);
	spa_list_consume(conn, &server->connections, link)
		pw_websocket_connection_destroy(conn);
	if (server->source)
		pw_loop_destroy_source(ws->loop, server->source);
	pw_free_strv(server->paths);
	free(server);
}

void pw_websocket_destroy(struct pw_websocket *ws)
{
	struct server *server;
	struct pw_websocket_connection *conn;

	pw_log_info("destroy sebsocket %p", ws);
	pw_websocket_emit_destroy(ws);

	spa_list_consume(server, &ws->servers, link)
		server_free(server);
	spa_list_consume(conn, &ws->connections, link)
		pw_websocket_connection_destroy(conn);

	spa_hook_list_clean(&ws->listener_list);
	free(ws->ifname);
	free(ws->ifaddress);
	free(ws->user_agent);
	free(ws->server_name);
	free(ws);
}

void pw_websocket_add_listener(struct pw_websocket *ws,
		struct spa_hook *listener,
		const struct pw_websocket_events *events, void *data)
{
	spa_hook_list_append(&ws->listener_list, listener, events, data);
}

static int update_io(struct pw_websocket_connection *conn, int io, bool active)
{
	if (conn->source) {
		uint32_t mask = conn->source->mask;
		SPA_FLAG_UPDATE(mask, io, active);
		if (mask != conn->source->mask)
			pw_loop_update_io(conn->ws->loop, conn->source, mask);
	}
	return 0;
}

static int receiver_expect(struct pw_websocket_connection *conn, size_t wanted,
		int (*have_data) (struct pw_websocket_connection *conn,
			void *data, size_t size, size_t current))
{
	pw_array_reset(&conn->data);
	conn->data_wanted = wanted;
	conn->data_cursor = 0;
	conn->data_state = 0;
	conn->have_data = have_data;
	return update_io(conn, SPA_IO_IN, wanted);
}

static int queue_message(struct pw_websocket_connection *conn, struct message *msg)
{
	spa_list_append(&conn->messages, &msg->link);
	conn->need_flush = true;
	return update_io(conn, SPA_IO_OUT, true);
}

static int receive_websocket(struct pw_websocket_connection *conn,
		void *data, size_t size, size_t current)
{
	uint8_t *d = data;
	int need = 0, header = 0, i;
	if (conn->data_state == 0) {
		/* header done */
		conn->status = d[0] & 0xf;
		if (d[1] & 0x80)
			header =+ 4;
		if ((d[1] & 0x7f) == 126)
			header += 2;
		else if ((d[1] & 0x7f) == 127)
			header += 8;
		else
			need += d[1] & 0x7f;
		conn->data_cursor = 2 + header;
		need += header;
		conn->data_state++;
	}
	else if (conn->data_state == 1) {
		/* extra length and mask */
		size_t payload_len = 0;
		if ((d[1] & 0x7f) == 126)
			header = 2;
		else if ((d[1] & 0x7f) == 127)
			header = 8;
		for (i = 0; i < header; i++)
			payload_len = (payload_len << 8) | d[i + 2];
		need += payload_len;
		conn->data_state++;
	}
	if (need == 0) {
		uint8_t *payload = &d[conn->data_cursor];
		size_t i, payload_size = conn->data.size - conn->data_cursor;
		struct iovec iov[1] = {{ payload, payload_size }};

		if (d[1] & 0x80) {
			uint8_t *mask = &d[conn->data_cursor - 4];
			for (i = 0; i < payload_size; i++)
				payload[i] ^= mask[i & 3];
		}

		switch (conn->status) {
		case PW_WEBSOCKET_OPCODE_PING:
			pw_log_info("received ping");
			pw_websocket_connection_send(conn, PW_WEBSOCKET_OPCODE_PONG, iov, 1);
			break;
		case PW_WEBSOCKET_OPCODE_CLOSE:
			pw_log_info("received close");
			pw_websocket_connection_send(conn, PW_WEBSOCKET_OPCODE_CLOSE, iov, 1);
			pw_websocket_connection_disconnect(conn, true);
			break;
		default:
			pw_log_debug("received message %02x", conn->status);
			pw_websocket_connection_emit_message(conn, conn->status,
					payload, payload_size);
		}
		receiver_expect(conn, 2, receive_websocket);
	}
	return need;
}

static int connection_upgrade_failed(struct pw_websocket_connection *conn,
		int status, const char *message)
{
	FILE *f;
	size_t len;
	struct message *msg;

	if ((f = open_memstream((char**)&msg, &len)) == NULL)
		return -errno;

	fseek(f, offsetof(struct message, data), SEEK_SET);
	fprintf(f, "HTTP/1.1 %d %s\r\n", status, message);
	fprintf(f, "Transfer-Encoding: chunked\r\n");
	fprintf(f, "Content-Type: application/octet-stream\r\n");
	fprintf(f, "Server: %s\r\n", conn->ws->server_name);
	fprintf(f, "\r\n");
	fclose(f);

	msg->len = len - offsetof(struct message, data);
	pw_log_info("send error %d %s", status, message);
	return queue_message(conn, msg);
}

static void make_accept(const char *key, char *accept)
{
	static const char *str = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
	uint8_t tmp[24 + 36], sha1[20];
	memcpy(&tmp[ 0], key, 24);
	memcpy(&tmp[24], str, 36);
	sha1digest(sha1, NULL, tmp, sizeof(tmp));
	pw_base64_encode(sha1, sizeof(sha1), accept, '=');
}

static int connection_upgraded_send(struct pw_websocket_connection *conn)
{
	FILE *f;
	size_t len;
	struct message *msg;
	char accept[29];

	if ((f = open_memstream((char**)&msg, &len)) == NULL)
		return -errno;

	make_accept(conn->key, accept);

	fseek(f, offsetof(struct message, data), SEEK_SET);
	fprintf(f, "HTTP/1.1 101 Switching Protocols\r\n");
	fprintf(f, "Upgrade: websocket\r\n");
	fprintf(f, "Connection: Upgrade\r\n");
	fprintf(f, "Sec-WebSocket-Accept: %s\r\n", accept);
	fprintf(f, "\r\n");
	fclose(f);

	msg->len = len - offsetof(struct message, data);
	pw_log_info("send upgrade %s", msg->data);
	return queue_message(conn, msg);
}

static int complete_upgrade(struct pw_websocket_connection *conn)
{
	pw_websocket_emit_connected(conn->ws, conn->user, conn, conn->path);
	return receiver_expect(conn, 2, receive_websocket);
}

static int header_key_val(char *buf, char **key, char **val)
{
	char *v;
	*key = buf;
	if ((v = strstr(buf, ":")) == NULL)
		return -EPROTO;
	*v++ = '\0';
	*val = pw_strip(v, " ");
	return 0;
}

static int receive_http_request(struct pw_websocket_connection *conn,
		void *data, size_t size, size_t current)
{
	char *d = data, *l;
	char c = d[current];
	int need = 1;

	if (conn->data_state == 0) {
		if (c == '\n') {
			int v1, v2;
			d[current] = '\0';
			l = pw_strip(&d[conn->data_cursor], "\n\r ");
			conn->data_cursor = current+1;
			if (sscanf(l, "GET %ms HTTP/%d.%d", &conn->path, &v1, &v2) != 3)
				return -EPROTO;
			conn->data_state++;
		}
	}
	else if (conn->data_state == 1) {
		if (c == '\n') {
			char *key, *val;
			d[current] = '\0';
			l = pw_strip(&d[conn->data_cursor], "\n\r ");
			if (strlen(l) > 0) {
				conn->data_cursor = current+1;
				if (header_key_val(l, &key, &val) < 0)
					return -EPROTO;
				if (spa_streq(key, "Sec-WebSocket-Key"))
					strncpy(conn->key, val, sizeof(conn->key)-1);
			} else {
				conn->data_state++;
				need = 0;
			}
		}
	}
	if (need == 0) {
		if (conn->server && conn->server->paths &&
		    pw_strv_find(conn->server->paths, conn->path) < 0) {
			connection_upgrade_failed(conn, 404, "Not Found");
		} else {
			connection_upgraded_send(conn);
			complete_upgrade(conn);
		}
	}
	return need;
}

static struct message *find_pending(struct pw_websocket_connection *conn, uint32_t seq)
{
	struct message *msg;
	spa_list_for_each(msg, &conn->pending, link) {
		if (msg->seq == seq)
			return msg;
	}
	return NULL;
}

static int receive_http_reply(struct pw_websocket_connection *conn,
		void *data, size_t size, size_t current)
{
	char *d = data, *l;
	char c = d[current];
	int need = 1;

	if (conn->data_state == 0) {
		if (c == '\n') {
			int v1, v2, status, message;
			/* status complete */
			d[current] = '\0';
			l = pw_strip(&d[conn->data_cursor], "\n\r ");
			conn->data_cursor = current+1;
			if (sscanf(l, "HTTP/%d.%d %n%d", &v1, &v2, &message, &status) != 3)
				return -EPROTO;
			conn->status = status;
			strcpy(conn->message, &l[message]);
			conn->content_length = 0;
			conn->data_state++;
		}
	}
	else if (conn->data_state == 1) {
		if (c == '\n') {
			/* header line complete */
			d[current] = '\0';
			l = pw_strip(&d[conn->data_cursor], "\n\r ");
			conn->data_cursor = current+1;
			if (strlen(l) > 0) {
				char *key, *value;
				if (header_key_val(l, &key, &value) < 0)
					return -EPROTO;
				if (spa_streq(key, "Sec-WebSocket-Accept")) {
					char accept[29];
					make_accept(conn->key, accept);
					if (!spa_streq(value, accept)) {
						pw_log_error("got Accept:%s expected:%s", value, accept);
						return -EPROTO;
					}
				}
				else if (spa_streq(key, "Content-Length"))
					conn->content_length = atoi(value);
			} else {
				conn->data_state++;
				need = conn->content_length;
			}
		}
	}
	if (need == 0) {
		/* message completed */
		uint32_t seq;
		int res;
		struct message *msg;

		seq = conn->recv_seq++;

		pw_log_info("received reply to request with seq:%" PRIu32, seq);

		if ((msg = find_pending(conn, seq)) != NULL) {
			res = msg->reply(msg->user_data, conn->status);
			spa_list_remove(&msg->link);
			free(msg);

			if (res < 0)
				pw_websocket_connection_emit_error(conn, res, conn->message);
		}
	}
	return need;
}

static int on_upgrade_reply(void *user_data, int status)
{
	struct pw_websocket_connection *conn = user_data;
	if (status != 101)
		return -EPROTO;
	return complete_upgrade(conn);
}

static int handle_connect(struct pw_websocket_connection *conn, int fd)
{
	int res = 0;
	socklen_t res_len;
	FILE *f;
	size_t len;
	struct message *msg;
	uint8_t key[16];

	len = sizeof(res);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &res, &res_len) < 0) {
		pw_log_error("getsockopt: %m");
		return -errno;
	}
	if (res != 0)
		return -res;

	pw_log_info("connected to %s:%u", conn->name, conn->port);

	conn->connecting = false;
	conn->status = 0;

	if ((f = open_memstream((char**)&msg, &len)) == NULL)
		return -errno;

	fseek(f, offsetof(struct message, data), SEEK_SET);

	/* make a key */
	pw_random(key, sizeof(key));
	pw_base64_encode(key, sizeof(key), conn->key, '=');

	fprintf(f, "GET %s HTTP/1.1\r\n", conn->path);
	fprintf(f, "Host: %s\r\n", conn->host);
	fprintf(f, "Upgrade: websocket\r\n");
	fprintf(f, "Connection: Upgrade\r\n");
	fprintf(f, "Sec-WebSocket-Version: 13\r\n");
	fprintf(f, "Sec-WebSocket-Key: %s\r\n", conn->key);
	fprintf(f, "Accept: */*\r\n");
	fprintf(f, "User-Agent: %s\r\n", conn->ws->user_agent);
	fprintf(f, "\r\n");
	fclose(f);

	msg->len = len - offsetof(struct message, data);
	msg->reply = on_upgrade_reply;
	msg->user_data = conn;
	msg->seq = conn->send_seq++;

	pw_log_info("%s", msg->data);

	receiver_expect(conn, 1, receive_http_reply);

	return queue_message(conn, msg);
}

static int handle_input(struct pw_websocket_connection *conn)
{
	int res;

	while (conn->data.size < conn->data_wanted) {
		size_t current = conn->data.size;
		size_t pending = conn->data_wanted - current;
		void *b;

		if (conn->source == NULL)
			return -EPIPE;

		if ((res = pw_array_ensure_size(&conn->data, pending)) < 0)
			return res;
		b = SPA_PTROFF(conn->data.data, current, void);

		res = read(conn->source->fd, b, pending);
		if (res == 0)
			return 0;
		if (res < 0) {
			res = -errno;
			if (res == -EINTR)
				continue;
			if (res != -EAGAIN && res != -EWOULDBLOCK)
				return res;
			return -EAGAIN;
		}
		conn->data.size += res;
		if (conn->data.size == conn->data_wanted) {
			if ((res = conn->have_data(conn,
					conn->data.data,
					conn->data.size,
					current)) < 0)
				return res;

			conn->data_wanted += res;
		}
	}
	return 0;
}

static int flush_output(struct pw_websocket_connection *conn)
{
	int res;

	conn->need_flush = false;

	if (conn->source == NULL)
		return -EPIPE;

	while (true) {
		struct message *msg;
		void *data;
		size_t size;

		if (spa_list_is_empty(&conn->messages)) {
			if (conn->draining)
				pw_websocket_connection_disconnect(conn, false);
			break;
		}
		msg = spa_list_first(&conn->messages, struct message, link);

		if (msg->offset < msg->len) {
			data = SPA_PTROFF(msg->data, msg->offset, void);
			size = msg->len - msg->offset;
		} else {
			spa_list_remove(&msg->link);
			if (msg->reply != NULL)
				spa_list_append(&conn->pending, &msg->link);
			else
				free(msg);
			continue;
		}

		while (true) {
			res = send(conn->source->fd, data, size, MSG_NOSIGNAL | MSG_DONTWAIT);
			if (res < 0) {
				res = -errno;
				if (res == -EINTR)
					continue;
				if (res != -EAGAIN && res != -EWOULDBLOCK)
					pw_log_warn("conn %p: send %zu, error %d: %m",
							conn, size, res);
				return res;
			}
			msg->offset += res;
			break;
		}
	}
	return 0;
}

static void
on_source_io(void *data, int fd, uint32_t mask)
{
	struct pw_websocket_connection *conn = data;
	int res;

	conn->refcount++;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		res = -EPIPE;
		goto error;
	}
	if (mask & SPA_IO_IN) {
		if ((res = handle_input(conn)) != -EAGAIN)
			goto error;
	}
	if (mask & SPA_IO_OUT || conn->need_flush) {
		if (conn->connecting) {
			if ((res = handle_connect(conn, fd)) < 0)
				goto error;
		}
		res = flush_output(conn);
		if (res >= 0) {
			if (conn->source)
				pw_loop_update_io(conn->ws->loop, conn->source,
					conn->source->mask & ~SPA_IO_OUT);
		} else if (res != -EAGAIN)
			goto error;
	}
done:
	websocket_connection_unref(conn);
	return;
error:
	if (res < 0) {
		pw_log_error("%p: %s got connection error %d (%s)", conn,
				conn->name, res, spa_strerror(res));
		snprintf(conn->message, sizeof(conn->message), "%s", spa_strerror(res));
		pw_websocket_connection_emit_error(conn, res, conn->message);
	} else {
		pw_log_info("%p: %s connection closed", conn, conn->name);
	}
	pw_websocket_connection_disconnect(conn, false);
	goto done;
}

int pw_websocket_connection_address(struct pw_websocket_connection *conn,
		struct sockaddr *addr, socklen_t addr_len)
{
	memcpy(addr, &conn->addr, SPA_MIN(addr_len, sizeof(conn->addr)));
	return 0;
}

static struct pw_websocket_connection *connection_new(struct pw_websocket *ws, void *user,
		struct sockaddr *addr, socklen_t addr_len, int fd, struct server *server)
{
	struct pw_websocket_connection *conn;

	if ((conn = calloc(1, sizeof(*conn))) == NULL)
		goto error;

	if ((conn->source = pw_loop_add_io(ws->loop, spa_steal_fd(fd),
					SPA_IO_ERR | SPA_IO_HUP | SPA_IO_OUT,
					true, on_source_io, conn)) == NULL)
		goto error;

	memcpy(&conn->addr, addr, SPA_MIN(addr_len, sizeof(conn->addr)));
	conn->ws = ws;
	conn->server = server;
	conn->user = user;
	if (server)
		spa_list_append(&server->connections, &conn->link);
	else
		spa_list_append(&ws->connections, &conn->link);

	conn->refcount = 1;
	if (pw_net_get_ip(&conn->addr, conn->name, sizeof(conn->name),
				&conn->ipv4, &conn->port) < 0)
		snprintf(conn->name, sizeof(conn->name), "connection %p", conn);

	spa_list_init(&conn->messages);
	spa_list_init(&conn->pending);
	spa_hook_list_init(&conn->listener_list);
	pw_array_init(&conn->data, 4096);

	pw_log_debug("new websocket %p connection %p %s:%u", ws,
			conn, conn->name, conn->port);

	return conn;
error:
	if (fd != -1)
		close(fd);
	free(conn);
	return NULL;
}

static int make_tcp_socket(struct server *server, const char *name, uint16_t port, const char *ifname,
		const char *ifaddress)
{
	struct sockaddr_storage addr;
	int res, on;
	socklen_t len = 0;
	spa_autoclose int fd = -1;

	if ((res = pw_net_parse_address_port(name, ifaddress, port, &addr, &len)) < 0) {
		pw_log_error("%p: can't parse address %s: %s", server,
				name, spa_strerror(res));
		goto error;
	}

	if ((fd = socket(addr.ss_family, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)) < 0) {
		res = -errno;
		pw_log_error("%p: socket() failed: %m", server);
		goto error;
	}
#ifdef SO_BINDTODEVICE
	if (ifname && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname)) < 0) {
		res = -errno;
		pw_log_error("%p: setsockopt(SO_BINDTODEVICE) failed: %m", server);
		goto error;
	}
#endif
	on = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (const void *) &on, sizeof(on)) < 0)
		pw_log_warn("%p: setsockopt(): %m", server);

	if (bind(fd, (struct sockaddr *) &addr, len) < 0) {
		res = -errno;
		pw_log_error("%p: bind() failed: %m", server);
		goto error;
	}
	if (listen(fd, 5) < 0) {
		res = -errno;
		pw_log_error("%p: listen() failed: %m", server);
		goto error;
	}
	if (getsockname(fd, (struct sockaddr *)&addr, &len) < 0) {
		res = -errno;
		pw_log_error("%p: getsockname() failed: %m", server);
		goto error;
	}

	server->addr = addr;

	return spa_steal_fd(fd);
error:
	return res;
}

static void
on_server_connect(void *data, int fd, uint32_t mask)
{
	struct server *server = data;
	struct pw_websocket *ws = server->ws;
	struct sockaddr_storage addr;
	socklen_t addrlen;
	spa_autoclose int conn_fd = -1;
	int val;
	struct pw_websocket_connection *conn = NULL;

	addrlen = sizeof(addr);
	if ((conn_fd = accept4(fd, (struct sockaddr*)&addr, &addrlen,
					SOCK_NONBLOCK | SOCK_CLOEXEC)) < 0)
		goto error;

	if (server->n_connections >= MAX_CONNECTIONS)
		goto error_refused;

	if ((conn = connection_new(ws, server->user, (struct sockaddr*)&addr, sizeof(addr),
			spa_steal_fd(conn_fd), server)) == NULL)
		goto error;

	server->n_connections++;

	pw_log_info("%p: connection:%p %s:%u connected", ws,
			conn, conn->name, conn->port);

	val = 1;
	if (setsockopt(conn->source->fd, IPPROTO_TCP, TCP_NODELAY,
				(const void *) &val, sizeof(val)) < 0)
		pw_log_warn("TCP_NODELAY failed: %m");

	val = IPTOS_LOWDELAY;
	if (setsockopt(conn->source->fd, IPPROTO_IP, IP_TOS,
				(const void *) &val, sizeof(val)) < 0)
		pw_log_warn("IP_TOS failed: %m");

	receiver_expect(conn, 1, receive_http_request);
	return;

error_refused:
	errno = ECONNREFUSED;
error:
	pw_log_error("%p: failed to create connection: %m", ws);
	return;
}

int pw_websocket_listen(struct pw_websocket *ws, void *user,
		const char *hostname, const char *service, const char *paths)
{
	int res;
	struct server *server;
	uint16_t port = atoi(service);

	if ((server = calloc(1, sizeof(struct server))) == NULL)
		return -errno;

	server->ws = ws;
	spa_list_append(&ws->servers, &server->link);

	server->user = user;
	spa_list_init(&server->connections);

	if ((res = make_tcp_socket(server, hostname, port, ws->ifname, ws->ifaddress)) < 0)
		goto error;

	if ((server->source = pw_loop_add_io(ws->loop, res, SPA_IO_IN,
			true, on_server_connect, server)) == NULL) {
		res = -errno;
		goto error;
	}
	if (paths)
		server->paths = pw_strv_parse(paths, strlen(paths), INT_MAX, NULL);

	pw_log_info("%p: listen %s:%u %s", ws, hostname, port, paths);
	return 0;
error:
	pw_log_error("%p: can't create server: %s", ws, spa_strerror(res));
	server_free(server);
	return res;
}

int pw_websocket_cancel(struct pw_websocket *ws, void *user)
{
	struct server *s, *ts;
	struct pw_websocket_connection *c, *tc;
	int count = 0;

	spa_list_for_each_safe(s, ts, &ws->servers, link) {
		if (s->user == user) {
			server_free(s);
			count++;
		}
	}
	spa_list_for_each_safe(c, tc, &ws->connections, link) {
		if (c->user == user) {
			pw_websocket_connection_destroy(c);
			count++;
		}
	}
	return count;
}

int pw_websocket_connect(struct pw_websocket *ws, void *user,
		const char *hostname, const char *service, const char *path)
{
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int res, fd;
	struct pw_websocket_connection *conn = NULL;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;

	if ((res = getaddrinfo(hostname, service, &hints, &result)) != 0) {
		pw_log_error("getaddrinfo: %s", gai_strerror(res));
		return -EINVAL;
	}
	res = -ENOENT;
	for (rp = result; rp != NULL; rp = rp->ai_next) {
		if ((fd = socket(rp->ai_family,
				rp->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
				rp->ai_protocol)) == -1)
			continue;

		res = connect(fd, rp->ai_addr, rp->ai_addrlen);
		if (res == 0 || (res < 0 && errno == EINPROGRESS))
			break;

		res = -errno;
		close(fd);
	}
	if (rp == NULL) {
		pw_log_error("Could not connect to %s:%s: %s", hostname, service,
				spa_strerror(res));
	} else {
		if ((conn = connection_new(ws, user, rp->ai_addr, rp->ai_addrlen, fd, NULL)) == NULL)
			res = -errno;
	}
	freeaddrinfo(result);
	if (conn == NULL)
		return res;

	conn->connecting = true;
	conn->maskbit = 0x80;
	conn->path = strdup(path);
	asprintf(&conn->host, "%s:%s", hostname, service);

	pw_log_info("%p: connecting to %s:%u path:%s", conn,
			conn->name, conn->port, path);
	return 0;
}

int pw_websocket_connection_send(struct pw_websocket_connection *conn, uint8_t opcode,
		const struct iovec *iov, size_t iov_len)
{
	struct message *msg;
	size_t len = 2, i, j, k;
	uint8_t *d, *mask = NULL, maskbit = conn->maskbit;
	size_t payload_length = 0;

	for (i = 0; i < iov_len; i++)
		payload_length += iov[i].iov_len;

	if ((msg = calloc(1, sizeof(*msg) + 14 + payload_length)) == NULL)
		return -errno;

	d = msg->data;
	d[0] = 0x80 | opcode;

	if (payload_length < 126)
		k = 0;
	else  if (payload_length < 65536)
		k = 2;
	else
		k = 8;

	d[1] = maskbit | (k == 0 ? payload_length : (k == 2 ? 126 : 127));
	for (i = 0, j = (k-1)*8 ; i < k; i++, j -= 8)
		d[len++] = (payload_length >> j) & 0xff;

	if (maskbit) {
		mask = &d[len];
		pw_random(mask, 4);
		len += 4;
	}
	for (i = 0, k = 0; i < iov_len; i++) {
		if (maskbit)
			for (j = 0; j < iov[i].iov_len; j++, k++)
				d[len+j] = ((uint8_t*)iov[i].iov_base)[j] ^ mask[k & 3];
		else
			memcpy(&d[len], iov[i].iov_base, iov[i].iov_len);

		len += iov[i].iov_len;
	}
	msg->len = len;

	return queue_message(conn, msg);
}

int pw_websocket_connection_send_text(struct pw_websocket_connection *conn,
		const char *payload, size_t payload_len)
{
	struct iovec iov[1] = {{ (void*)payload, payload_len }};
	pw_log_info("send text %.*s", (int)payload_len, payload);
	return pw_websocket_connection_send(conn, PW_WEBSOCKET_OPCODE_TEXT, iov, 1);
}
