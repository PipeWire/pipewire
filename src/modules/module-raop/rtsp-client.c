/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
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
	int cseq;
	void (*reply) (void *user_data, int status, const struct spa_dict *headers);
	void *user_data;
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
	unsigned int wait_status:1;

	int status;
	char line_buf[1024];
	size_t line_pos;
	struct pw_properties *headers;

	char *session;
	int cseq;

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
	free(client);
}

void *pw_rtsp_client_get_user_data(struct pw_rtsp_client *client)
{
	return client->user_data;
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
	client->wait_status = true;
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

static struct message *find_pending(struct pw_rtsp_client *client, int cseq)
{
	struct message *msg;
	spa_list_for_each(msg, &client->pending, link) {
		if (msg->cseq == cseq)
			return msg;
	}
	return NULL;
}

static int process_input(struct pw_rtsp_client *client)
{
	char *buf = NULL;
	int res;

	if ((res = read_line(client, &buf)) <= 0)
		return res;

	pw_log_debug("%s", buf);

	if (client->wait_status) {
		const char *state = NULL, *s;
		size_t len;

		pw_log_info("status: %s", buf);

		s = pw_split_walk(buf, " ", &len, &state);
		if (!spa_strstartswith(s, "RTSP/"))
			goto error;

		s = pw_split_walk(buf, " ", &len, &state);
		if (s == NULL)
			goto error;

		client->status = atoi(s);

		s = pw_split_walk(buf, " ", &len, &state);
		if (s == NULL)
			goto error;

		client->wait_status = false;
		pw_properties_clear(client->headers);
	} else {
		if (strlen(buf) == 0) {
			int cseq;
			struct message *msg;
			const struct spa_dict_item *it;

			spa_dict_for_each(it, &client->headers->dict)
				pw_log_info(" %s: %s", it->key, it->value);

			cseq = pw_properties_get_int32(client->headers, "CSeq", 0);

			if ((msg = find_pending(client, cseq)) != NULL) {
				msg->reply(msg->user_data, client->status, &client->headers->dict);
				spa_list_remove(&msg->link);
				free(msg);
			} else {
				pw_rtsp_client_emit_message(client, client->status,
					&client->headers->dict);
			}
			client->wait_status = true;
		} else {
			char *key, *value;

			key = buf;
			value = strstr(buf, ":");
			if (value == NULL)
				goto error;
			*value++ = '\0';
			while (*value == ' ')
				value++;
			pw_properties_set(client->headers, key, value);
		}
	}
	return 0;
error:
	return -EPROTO;
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
	for (rp = result; rp != NULL; rp = rp->ai_next) {
		fd = socket(rp->ai_family,
				rp->ai_socktype | SOCK_CLOEXEC | SOCK_NONBLOCK,
				rp->ai_protocol);
		if (fd == -1)
			continue;

		res = connect(fd, rp->ai_addr, rp->ai_addrlen);
		if (res == 0 || (res < 0 && errno == EINPROGRESS))
			break;

		close(fd);
	}
	freeaddrinfo(result);

	if (rp == NULL) {
		pw_log_error("Could not connect to %s:%u", hostname, port);
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
	if (client->source == NULL)
		return 0;

	pw_loop_destroy_source(client->loop, client->source);
	client->source = NULL;
	free(client->url);
	client->url = NULL;
	free(client->session_id);
	client->session_id = NULL;
	pw_rtsp_client_emit_disconnected(client);
	return 0;
}

int pw_rtsp_client_send(struct pw_rtsp_client *client,
		const char *cmd, const struct spa_dict *headers,
		const char *content_type, const char *content,
		void (*reply) (void *user_data, int status, const struct spa_dict *headers),
		void *user_data)
{
	FILE *f;
	size_t len;
	const struct spa_dict_item *it;
	struct message *msg;
	int cseq;

	if ((f = open_memstream((char**)&msg, &len)) == NULL)
		return -errno;

	fseek(f, sizeof(*msg), SEEK_SET);

	cseq = ++client->cseq;

	fprintf(f, "%s %s RTSP/1.0\r\n", cmd, client->url);
	fprintf(f, "CSeq: %d\r\n", cseq);

	if (headers != NULL) {
		spa_dict_for_each(it, headers)
			fprintf(f, "%s: %s\r\n", it->key, it->value);
	}
	if (content_type != NULL && content != NULL) {
		fprintf(f, "Content-Type: %s\r\nContent-Length: %d\r\n",
			content_type, (int)strlen(content));
	}
	fprintf(f, "\r\n");

	if (content_type && content)
		fprintf(f, "%s", content);

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
