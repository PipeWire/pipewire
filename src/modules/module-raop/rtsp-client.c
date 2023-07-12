/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <spa/utils/result.h>

#include "rtsp-client.h"

#define pw_rtsp_client_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_rtsp_client_events, m, v, ##__VA_ARGS__)
#define pw_rtsp_client_emit_destroy(c)		pw_rtsp_client_emit(c, destroy, 0)
#define pw_rtsp_client_emit_connected(c)	pw_rtsp_client_emit(c, connected, 0)
#define pw_rtsp_client_emit_disconnected(c)	pw_rtsp_client_emit(c, disconnected, 0)
#define pw_rtsp_client_emit_error(c,r)		pw_rtsp_client_emit(c, error, 0, r)
#define pw_rtsp_client_emit_message(c,...)	pw_rtsp_client_emit(c, message, 0, __VA_ARGS__)

struct message {
	struct spa_list link;
	void *data;
	size_t len;
	size_t offset;
	uint32_t cseq;
	int (*reply) (void *user_data, int status, const struct spa_dict *headers, const struct pw_array *content);
	void *user_data;
};

enum client_recv_state {
	CLIENT_RECV_NONE,
	CLIENT_RECV_STATUS,
	CLIENT_RECV_HEADERS,
	CLIENT_RECV_CONTENT,
};

struct pw_rtsp_client {
	struct pw_loop *loop;
	struct pw_properties *props;

	struct spa_hook_list listener_list;

	char *session_id;
	char *url;

	union {
		struct sockaddr sa;
		struct sockaddr_in in;
		struct sockaddr_in6 in6;
	} local_addr;

	struct spa_source *source;
	unsigned int connecting:1;
	unsigned int need_flush:1;

	enum client_recv_state recv_state;
	int status;
	char line_buf[1024];
	size_t line_pos;
	struct pw_properties *headers;
	struct pw_array content;
	size_t content_length;

	uint32_t cseq;

	struct spa_list messages;
	struct spa_list pending;

	void *user_data;
};

struct pw_rtsp_client *pw_rtsp_client_new(struct pw_loop *main_loop,
				struct pw_properties *props,
				size_t user_data_size)
{
	struct pw_rtsp_client *client;

	client = calloc(1, sizeof(*client) + user_data_size);
	if (client == NULL)
		return NULL;

	client->loop = main_loop;
	client->props = props;
	if (user_data_size > 0)
		client->user_data = SPA_PTROFF(client, sizeof(*client), void);

	spa_list_init(&client->messages);
	spa_list_init(&client->pending);
	spa_hook_list_init(&client->listener_list);
	client->headers = pw_properties_new(NULL, NULL);
	pw_array_init(&client->content, 4096);
	client->recv_state = CLIENT_RECV_NONE;

	pw_log_info("new client %p", client);

	return client;
}

void pw_rtsp_client_destroy(struct pw_rtsp_client *client)
{
	pw_log_info("destroy client %p", client);
	pw_rtsp_client_emit_destroy(client);

	pw_rtsp_client_disconnect(client);
	pw_properties_free(client->headers);
	pw_properties_free(client->props);
	spa_hook_list_clean(&client->listener_list);
	pw_array_clear(&client->content);
	free(client);
}

void *pw_rtsp_client_get_user_data(struct pw_rtsp_client *client)
{
	return client->user_data;
}

const char *pw_rtsp_client_get_url(struct pw_rtsp_client *client)
{
	return client->url;
}

void pw_rtsp_client_add_listener(struct pw_rtsp_client *client,
		struct spa_hook *listener,
		const struct pw_rtsp_client_events *events, void *data)
{
	spa_hook_list_append(&client->listener_list, listener, events, data);
}

const struct pw_properties *pw_rtsp_client_get_properties(struct pw_rtsp_client *client)
{
	return client->props;
}

int pw_rtsp_client_get_local_ip(struct pw_rtsp_client *client,
		int *version, char *ip, size_t len)
{
	if (client->local_addr.sa.sa_family == AF_INET) {
		*version = 4;
		if (ip)
			inet_ntop(client->local_addr.sa.sa_family,
				&client->local_addr.in.sin_addr, ip, len);
	} else if (client->local_addr.sa.sa_family == AF_INET6) {
		*version = 6;
		if (ip)
			inet_ntop(client->local_addr.sa.sa_family,
				&client->local_addr.in6.sin6_addr,
				ip, len);
	} else
		return -EIO;
	return 0;
}

static int handle_connect(struct pw_rtsp_client *client, int fd)
{
	int res, ip_version;
	socklen_t len;
	char local_ip[INET6_ADDRSTRLEN];

	len = sizeof(res);
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &res, &len) < 0) {
		pw_log_error("getsockopt: %m");
		return -errno;
	}
	if (res != 0)
		return -res;

	len = sizeof(client->local_addr.sa);
	if (getsockname(fd, &client->local_addr.sa, &len) < 0)
		return -errno;

	if ((res = pw_rtsp_client_get_local_ip(client, &ip_version,
			local_ip, sizeof(local_ip))) < 0)
		return res;

	if (ip_version == 4)
		asprintf(&client->url, "rtsp://%s/%s", local_ip, client->session_id);
	else
		asprintf(&client->url, "rtsp://[%s]/%s", local_ip, client->session_id);

	pw_log_info("connected local ip %s", local_ip);

	client->connecting = false;

	client->recv_state = CLIENT_RECV_STATUS;
	pw_properties_clear(client->headers);
	client->status = 0;
	client->line_pos = 0;
	client->content_length = 0;

	pw_rtsp_client_emit_connected(client);

	return 0;
}

static int read_line(struct pw_rtsp_client *client, char **buf)
{
	int res;

	while (true) {
		uint8_t c;

		res = read(client->source->fd, &c, 1);
		if (res == 0)
			return -EPIPE;
		if (res < 0) {
			res = -errno;
			if (res == -EINTR)
				continue;
			if (res != -EAGAIN && res != -EWOULDBLOCK)
				return res;
			return 0;
		}
		if (c == '\n') {
			client->line_buf[client->line_pos] = '\0';
			client->line_pos = 0;
			if (buf)
				*buf = client->line_buf;
			return 1;
		}
		if (c == '\r')
			continue;
		if (client->line_pos < sizeof(client->line_buf) - 1)
			client->line_buf[client->line_pos++] = c;
		client->line_buf[client->line_pos] = '\0';
	}
	return 0;
}

static struct message *find_pending(struct pw_rtsp_client *client, uint32_t cseq)
{
	struct message *msg;
	spa_list_for_each(msg, &client->pending, link) {
		if (msg->cseq == cseq)
			return msg;
	}
	return NULL;
}

static int process_status(struct pw_rtsp_client *client, char *buf)
{
	const char *state = NULL, *s;
	size_t len;

	pw_log_info("status: %s", buf);

	s = pw_split_walk(buf, " ", &len, &state);
	if (!spa_strstartswith(s, "RTSP/"))
		return -EPROTO;

	s = pw_split_walk(buf, " ", &len, &state);
	if (s == NULL)
		return -EPROTO;

	client->status = atoi(s);
	if (client->status == 0)
		return -EPROTO;

	s = pw_split_walk(buf, " ", &len, &state);
	if (s == NULL)
		return -EPROTO;

	pw_properties_clear(client->headers);
	client->recv_state = CLIENT_RECV_HEADERS;

	return 0;
}

static void dispatch_handler(struct pw_rtsp_client *client)
{
	uint32_t cseq;
	int res;
	struct message *msg;

	if (pw_properties_fetch_uint32(client->headers, "CSeq", &cseq) < 0)
		return;

	pw_log_info("received reply to request with cseq:%" PRIu32, cseq);

	msg = find_pending(client, cseq);
	if (msg) {
		res = msg->reply(msg->user_data, client->status, &client->headers->dict, &client->content);
		spa_list_remove(&msg->link);
		free(msg);

		if (res < 0)
			pw_log_warn("client %p: handle reply cseq:%u error: %s",
					client, cseq, spa_strerror(res));
	}
	else {
		pw_rtsp_client_emit_message(client, client->status, &client->headers->dict);
	}

	pw_array_reset(&client->content);
}

static void process_received_message(struct pw_rtsp_client *client)
{
	client->recv_state = CLIENT_RECV_STATUS;
	dispatch_handler(client);
}

static int process_header(struct pw_rtsp_client *client, char *buf)
{
	if (strlen(buf) > 0) {
		char *key = buf, *value;

		value = strstr(buf, ":");
		if (value == NULL)
			return -EPROTO;

		*value++ = '\0';

		value = pw_strip(value, " ");

		pw_properties_set(client->headers, key, value);
	}
	else {
		const struct spa_dict_item *it;
		spa_dict_for_each(it, &client->headers->dict)
			pw_log_info(" %s: %s", it->key, it->value);

		client->content_length = pw_properties_get_uint32(client->headers, "Content-Length", 0);
		if (client->content_length > 0)
			client->recv_state = CLIENT_RECV_CONTENT;
		else
			process_received_message(client);
	}

	return 0;
}

static int process_content(struct pw_rtsp_client *client)
{
	uint8_t buf[4096];

	while (client->content_length > 0) {
		const size_t max_recv = SPA_MIN(sizeof(buf), client->content_length);

		ssize_t res = read(client->source->fd, buf, max_recv);
		if (res == 0)
			return -EPIPE;

		if (res < 0) {
			res = -errno;
			if (res == -EAGAIN || res == -EWOULDBLOCK)
				return 0;

			return res;
		}

		void *p = pw_array_add(&client->content, res);
		memcpy(p, buf, res);

		spa_assert((size_t) res <= client->content_length);
		client->content_length -= res;
	}

	if (client->content_length == 0)
		process_received_message(client);

	return 0;
}

static int process_input(struct pw_rtsp_client *client)
{
	if (client->recv_state == CLIENT_RECV_STATUS || client->recv_state == CLIENT_RECV_HEADERS) {
		char *buf = NULL;
		int res;

		if ((res = read_line(client, &buf)) <= 0)
			return res;

		pw_log_debug("received line: %s", buf);

		switch (client->recv_state) {
		case CLIENT_RECV_STATUS:
			return process_status(client, buf);
		case CLIENT_RECV_HEADERS:
			return process_header(client, buf);
		default:
			spa_assert_not_reached();
		}
	}
	else if (client->recv_state == CLIENT_RECV_CONTENT) {
		return process_content(client);
	}
	else {
		spa_assert_not_reached();
	}
}

static int flush_output(struct pw_rtsp_client *client)
{
	int res;

	client->need_flush = false;

	while (true) {
		struct message *msg;
		void *data;
		size_t size;

		if (spa_list_is_empty(&client->messages))
			break;

		msg = spa_list_first(&client->messages, struct message, link);

		if (msg->offset < msg->len) {
			data = SPA_PTROFF(msg->data, msg->offset, void);
			size = msg->len - msg->offset;
		} else {
			pw_log_info("sent: %s", (char *)msg->data);
			spa_list_remove(&msg->link);
			if (msg->reply != NULL)
				spa_list_append(&client->pending, &msg->link);
			else
				free(msg);
			continue;
		}

		while (true) {
			res = send(client->source->fd, data, size, MSG_NOSIGNAL | MSG_DONTWAIT);
			if (res < 0) {
				res = -errno;
				if (res == -EINTR)
					continue;
				if (res != -EAGAIN && res != -EWOULDBLOCK)
					pw_log_warn("client %p: send %zu, error %d: %m",
							client, size, res);
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
	struct pw_rtsp_client *client = data;
	int res;

	if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
		res = -EPIPE;
		goto error;
	}
	if (mask & SPA_IO_IN) {
		if ((res = process_input(client)) < 0)
			goto error;
	}
	if (mask & SPA_IO_OUT || client->need_flush) {
		if (client->connecting) {
			if ((res = handle_connect(client, fd)) < 0)
				goto error;
		}
		res = flush_output(client);
		if (res >= 0) {
			pw_loop_update_io(client->loop, client->source,
				client->source->mask & ~SPA_IO_OUT);
		} else if (res != -EAGAIN)
			goto error;
	}
done:
	return;
error:
	pw_log_error("%p: got connection error %d (%s)", client, res, spa_strerror(res));
	pw_rtsp_client_emit_error(client, res);
	pw_rtsp_client_disconnect(client);
	goto done;
}

int pw_rtsp_client_connect(struct pw_rtsp_client *client,
		const char *hostname, uint16_t port, const char *session_id)
{
	struct addrinfo hints;
	struct addrinfo *result, *rp;
	int res, fd;
	char port_str[12];

	if (client->source != NULL)
		pw_rtsp_client_disconnect(client);

	pw_log_info("%p: connect %s:%u", client, hostname, port);

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = 0;
	hints.ai_protocol = 0;

	spa_scnprintf(port_str, sizeof(port_str), "%u", port);

	if ((res = getaddrinfo(hostname, port_str, &hints, &result)) != 0) {
		pw_log_error("getaddrinfo: %s", gai_strerror(res));
		return -EINVAL;
	}
	res = -ENOENT;
	for (rp = result; rp != NULL; rp = rp->ai_next) {
		fd = socket(rp->ai_family,
				rp->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
				rp->ai_protocol);
		if (fd == -1)
			continue;

		res = connect(fd, rp->ai_addr, rp->ai_addrlen);
		if (res == 0 || (res < 0 && errno == EINPROGRESS))
			break;

		res = -errno;
		close(fd);
	}
	freeaddrinfo(result);

	if (rp == NULL) {
		pw_log_error("Could not connect to %s:%u: %s", hostname, port,
				spa_strerror(res));
		return -EINVAL;
	}

	client->source = pw_loop_add_io(client->loop, fd,
			SPA_IO_IN | SPA_IO_OUT | SPA_IO_HUP | SPA_IO_ERR,
			true, on_source_io, client);

	if (client->source == NULL) {
		res = -errno;
		pw_log_error("%p: source create failed: %m", client);
		close(fd);
		return res;
	}
	client->connecting = true;
	free(client->session_id);
	client->session_id = strdup(session_id);
	pw_log_info("%p: connecting", client);

	return 0;
}

int pw_rtsp_client_disconnect(struct pw_rtsp_client *client)
{
	struct message *msg;

	if (client->source == NULL)
		return 0;

	pw_loop_destroy_source(client->loop, client->source);
	client->source = NULL;
	free(client->url);
	client->url = NULL;
	free(client->session_id);
	client->session_id = NULL;

	spa_list_consume(msg, &client->messages, link) {
		spa_list_remove(&msg->link);
		free(msg);
	}
	pw_rtsp_client_emit_disconnected(client);
	return 0;
}

int pw_rtsp_client_url_send(struct pw_rtsp_client *client, const char *url,
		const char *cmd, const struct spa_dict *headers,
		const char *content_type, const void *content, size_t content_length,
		int (*reply) (void *user_data, int status, const struct spa_dict *headers, const struct pw_array *content),
		void *user_data)
{
	FILE *f;
	size_t len;
	const struct spa_dict_item *it;
	struct message *msg;
	uint32_t cseq;

	if ((f = open_memstream((char**)&msg, &len)) == NULL)
		return -errno;

	fseek(f, sizeof(*msg), SEEK_SET);

	cseq = ++client->cseq;

	fprintf(f, "%s %s RTSP/1.0\r\n", cmd, url);
	fprintf(f, "CSeq: %" PRIu32 "\r\n", cseq);

	if (headers != NULL) {
		spa_dict_for_each(it, headers)
			fprintf(f, "%s: %s\r\n", it->key, it->value);
	}
	if (content_type != NULL && content != NULL) {
		fprintf(f, "Content-Type: %s\r\nContent-Length: %zu\r\n",
			content_type, content_length);
	}
	fprintf(f, "\r\n");

	if (content_type && content)
		fwrite(content, 1, content_length, f);

	fclose(f);

	msg->data = SPA_PTROFF(msg, sizeof(*msg), void);
	msg->len = len - sizeof(*msg);
	msg->offset = 0;
	msg->reply = reply;
	msg->user_data = user_data;
	msg->cseq = cseq;

	spa_list_append(&client->messages, &msg->link);

	client->need_flush = true;
	if (client->source && !(client->source->mask & SPA_IO_OUT)) {
		pw_loop_update_io(client->loop, client->source,
				client->source->mask | SPA_IO_OUT);
	}
	return 0;
}

int pw_rtsp_client_send(struct pw_rtsp_client *client,
		const char *cmd, const struct spa_dict *headers,
		const char *content_type, const char *content,
		int (*reply) (void *user_data, int status, const struct spa_dict *headers, const struct pw_array *content),
		void *user_data)
{
	const size_t content_length = content ? strlen(content) : 0;

	return pw_rtsp_client_url_send(client, client->url, cmd, headers,
				       content_type, content, content_length,
				       reply, user_data);
}
