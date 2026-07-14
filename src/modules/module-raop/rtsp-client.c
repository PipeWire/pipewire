/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>

#include <spa/utils/result.h>

#include "network-utils.h"
#include "rtsp-client.h"

#define pw_rtsp_client_emit(o,m,v,...) spa_hook_list_call(&o->listener_list, struct pw_rtsp_client_events, m, v, ##__VA_ARGS__)
#define pw_rtsp_client_emit_destroy(c)		pw_rtsp_client_emit(c, destroy, 0)
#define pw_rtsp_client_emit_connected(c)	pw_rtsp_client_emit(c, connected, 0)
#define pw_rtsp_client_emit_disconnected(c)	pw_rtsp_client_emit(c, disconnected, 0)
#define pw_rtsp_client_emit_error(c,r)		pw_rtsp_client_emit(c, error, 0, r)
#define pw_rtsp_client_emit_message(c,...)	pw_rtsp_client_emit(c, message, 0, __VA_ARGS__)

struct message {
	struct spa_list link;
	size_t len;
	size_t offset;
	uint32_t cseq;
	int (*reply) (void *user_data, int status, const struct spa_dict *headers, const struct pw_array *content);
	void *user_data;
	unsigned char *payload; // pointer to actual data buffer
	unsigned char data[];   // flexible array for default storage
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

	struct sockaddr_storage local_addr;

	struct spa_source *source;
	unsigned int connecting:1;
	unsigned int need_flush:1;

	enum client_recv_state recv_state;
	struct pw_rtsp_cipher_context *cipher_ctx;
	ssize_t (*decrypt) (uint8_t **plaintext, size_t *plaintext_len, const uint8_t *ciphertext, size_t ciphertext_len, struct pw_rtsp_cipher_context *cctx);
	ssize_t (*encrypt) (uint8_t **ciphertext, size_t *ciphertext_len, const uint8_t *plaintext, size_t plaintext_len, struct pw_rtsp_cipher_context *cctx);
	int status;
	char line_buf[1024];
	size_t line_pos;
	struct pw_properties *headers;
	struct pw_array content;
	size_t content_length;

	/* When a cipher is installed, incoming data is an AEAD block stream.
	 * in_cipher accumulates raw socket bytes until whole blocks can be
	 * decrypted; in_plain holds the decrypted plaintext the RTSP parser
	 * consumes (from in_plain_offset onwards). */
	struct pw_array in_cipher;
	struct pw_array in_plain;
	size_t in_plain_offset;

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
	if (client->headers == NULL) {
		free(client);
		return NULL;
	}
	pw_array_init(&client->content, 4096);
	pw_array_init(&client->in_cipher, 4096);
	pw_array_init(&client->in_plain, 4096);
	client->in_plain_offset = 0;
	client->recv_state = CLIENT_RECV_NONE;
	client->cipher_ctx = NULL;
	client->decrypt = NULL;
	client->encrypt = NULL;

	pw_log_info("new client %p", client);

	return client;
}

void pw_rtsp_client_set_decryption(struct pw_rtsp_client *client,
		ssize_t (*decrypt) (uint8_t **plaintext, size_t *plaintext_len, const uint8_t *ciphertext, size_t ciphertext_len, struct pw_rtsp_cipher_context *cctx))
{
	client->decrypt = decrypt;
}

void pw_rtsp_client_set_encryption(struct pw_rtsp_client *client,
		ssize_t (*encrypt) (uint8_t **ciphertext, size_t *ciphertext_len, const uint8_t *plaintext, size_t plaintext_len, struct pw_rtsp_cipher_context *cctx))
{
	client->encrypt = encrypt;
}

void pw_rtsp_client_set_cipher_ctx(struct pw_rtsp_client *client,
		struct pw_rtsp_cipher_context *cipher_ctx)
{
	client->cipher_ctx = cipher_ctx;
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
	pw_array_clear(&client->in_cipher);
	pw_array_clear(&client->in_plain);
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
	bool is_ipv4;
	int res;
	if ((res = pw_net_get_ip(&client->local_addr, ip, len, &is_ipv4, NULL)) < 0)
		return res;
	*version = is_ipv4 ? 4 : 6;
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

	len = sizeof(client->local_addr);
	if (getsockname(fd, (struct sockaddr*)&client->local_addr, &len) < 0)
		return -errno;

	if ((res = pw_rtsp_client_get_local_ip(client, &ip_version,
			local_ip, sizeof(local_ip))) < 0)
		return res;

	if (ip_version == 4)
		res = asprintf(&client->url, "rtsp://%s/%s", local_ip, client->session_id);
	else
		res = asprintf(&client->url, "rtsp://[%s]/%s", local_ip, client->session_id);
	if (res < 0) {
		client->url = NULL;
		return -ENOMEM;
	}

	pw_log_info("connected local ip %s", local_ip);

	client->connecting = false;

	client->recv_state = CLIENT_RECV_STATUS;
	pw_properties_clear(client->headers);
	pw_array_reset(&client->content);
	pw_array_reset(&client->in_cipher);
	pw_array_reset(&client->in_plain);
	client->in_plain_offset = 0;
	client->status = 0;
	client->line_pos = 0;
	client->content_length = 0;
	/* A fresh connection starts unencrypted; the owner re-installs the cipher
	 * after (re-)pairing. The cipher context is owned by the caller, not freed
	 * here. */
	client->decrypt = NULL;
	client->encrypt = NULL;
	client->cipher_ctx = NULL;

	pw_rtsp_client_emit_connected(client);

	return 0;
}

/* Reset the decrypted-input buffer once fully consumed. */
static void input_compact(struct pw_rtsp_client *client)
{
	if (client->in_plain_offset >= client->in_plain.size) {
		client->in_plain_offset = 0;
		pw_array_reset(&client->in_plain);
	}
}

/* Read from the socket into the cipher buffer and decrypt as many whole
 * AEAD blocks as are available into the plaintext buffer. Returns >0 if
 * plaintext was produced, 0 if nothing (yet) available, -errno on error. */
static int fill_plain(struct pw_rtsp_client *client)
{
	uint8_t tmp[4096];
	uint8_t *plain = NULL;
	size_t plain_len = 0;
	ssize_t res, consumed;
	void *p;

	res = read(client->source->fd, tmp, sizeof(tmp));
	if (res == 0)
		return -EPIPE;
	if (res < 0) {
		res = -errno;
		if (res == -EINTR || res == -EAGAIN || res == -EWOULDBLOCK)
			return 0;
		return res;
	}
	if ((p = pw_array_add(&client->in_cipher, res)) == NULL)
		return -ENOMEM;
	memcpy(p, tmp, res);

	consumed = client->decrypt(&plain, &plain_len,
			client->in_cipher.data, client->in_cipher.size, client->cipher_ctx);
	if (consumed < 0) {
		free(plain);
		pw_log_error("client %p: decryption failed", client);
		return -EIO;
	}
	if (plain != NULL && plain_len > 0) {
		if ((p = pw_array_add(&client->in_plain, plain_len)) == NULL) {
			free(plain);
			return -ENOMEM;
		}
		memcpy(p, plain, plain_len);
	}
	free(plain);

	/* Drop the ciphertext bytes that formed complete blocks, keeping any
	 * partial trailing block for the next read. */
	if (consumed > 0) {
		size_t remain = client->in_cipher.size - consumed;
		if (remain > 0)
			memmove(client->in_cipher.data,
				SPA_PTROFF(client->in_cipher.data, consumed, void), remain);
		client->in_cipher.size = remain;
	}
	return plain_len > 0 ? 1 : 0;
}

/* Fetch one input byte from the decrypted stream (when a cipher is installed)
 * or directly from the socket. Returns 1 (*c set), 0 if no data is currently
 * available, or -errno on error/EOF. */
static int input_get_byte(struct pw_rtsp_client *client, uint8_t *c)
{
	if (client->decrypt != NULL && client->cipher_ctx != NULL) {
		if (client->in_plain_offset >= client->in_plain.size) {
			int res = fill_plain(client);
			if (res <= 0)
				return res;
		}
		*c = *SPA_PTROFF(client->in_plain.data, client->in_plain_offset++, uint8_t);
		input_compact(client);
		return 1;
	}
	while (true) {
		ssize_t res = read(client->source->fd, c, 1);
		if (res == 0)
			return -EPIPE;
		if (res < 0) {
			res = -errno;
			if (res == -EINTR)
				continue;
			if (res == -EAGAIN || res == -EWOULDBLOCK)
				return 0;
			return res;
		}
		return 1;
	}
}

/* Read up to len bytes of message body from the decrypted stream or socket.
 * Returns bytes read (>0), 0 if none available, or -errno on error/EOF. */
static ssize_t input_read(struct pw_rtsp_client *client, void *buf, size_t len)
{
	if (client->decrypt != NULL && client->cipher_ctx != NULL) {
		size_t avail;
		if (client->in_plain_offset >= client->in_plain.size) {
			int res = fill_plain(client);
			if (res <= 0)
				return res;
		}
		avail = client->in_plain.size - client->in_plain_offset;
		len = SPA_MIN(avail, len);
		memcpy(buf, SPA_PTROFF(client->in_plain.data, client->in_plain_offset, void), len);
		client->in_plain_offset += len;
		input_compact(client);
		return len;
	}
	while (true) {
		ssize_t res = read(client->source->fd, buf, len);
		if (res == 0)
			return -EPIPE;
		if (res < 0) {
			res = -errno;
			if (res == -EINTR)
				continue;
			if (res == -EAGAIN || res == -EWOULDBLOCK)
				return 0;
			return res;
		}
		return res;
	}
}

/* Read one CRLF-terminated line. On a complete line returns its length and
 * sets *buf; on a partial line (no data yet) returns 0 and sets *buf = NULL;
 * on error returns -errno. */
static int read_line(struct pw_rtsp_client *client, char **buf)
{
	int res, idx = 0;

	*buf = NULL;
	while (true) {
		uint8_t c;

		res = input_get_byte(client, &c);
		if (res < 0)
			return res;
		if (res == 0)
			return 0;
		if (c == '\n') {
			client->line_buf[client->line_pos] = '\0';
			client->line_pos = 0;
			*buf = client->line_buf;
			return idx;
		}
		if (c == '\r')
			continue;
		if (client->line_pos < sizeof(client->line_buf) - 1) {
			client->line_buf[client->line_pos++] = c;
			idx++;
		}
		client->line_buf[client->line_pos] = '\0';
	}
	return idx;
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

/* Free a message and, if it was encrypted, its separately-allocated payload.
 * The payload is kept alive until the message itself is freed. */
static void message_free(struct message *msg)
{
	if (msg->payload != msg->data)
		free(msg->payload);
	free(msg);
}

static int process_status(struct pw_rtsp_client *client, char *buf)
{
	const char *state = NULL, *s;
	size_t len;

	pw_log_info("processing status: %s", buf);

	s = pw_split_walk(buf, " ", &len, &state);
	if (client->encrypt != NULL && client->cipher_ctx != NULL) {
		if (!spa_strstartswith(s, "HTTP/"))
			return -EPROTO;
	} else {
		if (!spa_strstartswith(s, "RTSP/"))
			return -EPROTO;
	}

	s = pw_split_walk(buf, " ", &len, &state);
	if (s == NULL)
		return -EPROTO;

	client->status = strtol(s, NULL, 10);
	if (client->status < 100 || client->status > 599)
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
		/* Unlink before dispatching: a reply handler may synchronously
		 * disconnect the client (e.g. TEARDOWN with Connection: close), and
		 * pw_rtsp_client_disconnect() frees every message still on the pending
		 * list. With msg already unlinked, disconnect leaves it alone and we
		 * free it exactly once below — avoiding a use-after-free/double-free. */
		spa_list_remove(&msg->link);

		res = msg->reply(msg->user_data, client->status, &client->headers->dict, &client->content);
		message_free(msg);

		if (res < 0) {
			pw_log_warn("client %p: handle reply cseq:%u error: %s",
					client, cseq, spa_strerror(res));
			/* A reply handler that fails fatally (e.g. a broken AP2
			 * handshake step) must not leave the client wedged: surface
			 * it as a terminal error so the owner can tear down. */
			pw_rtsp_client_emit_error(client, res);
		}
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
			pw_log_debug(" %s: %s", it->key, it->value);

		client->content_length = pw_properties_get_uint32(client->headers, "Content-Length", 0);
		if (client->content_length > 64 * 1024) {
			pw_log_error("Content-Length %zu exceeds maximum",
					client->content_length);
			return -EOVERFLOW;
		}
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

		ssize_t res = input_read(client, buf, max_recv);
		if (res == 0)
			return 0;
		if (res < 0)
			return res;

		void *p = pw_array_add(&client->content, res);
		if (p == NULL)
			return -ENOMEM;
		memcpy(p, buf, res);

		spa_assert((size_t) res <= client->content_length);
		client->content_length -= res;
		pw_log_debug("processing content (%zu of %zu bytes)",
				client->content.size, client->content.size + client->content_length);
	}

	if (client->content_length == 0)
		process_received_message(client);

	return 0;
}

static int process_input(struct pw_rtsp_client *client)
{
	/* Drain all currently-available input: on a level-triggered fd (and with
	 * the decrypted-input buffer) a single wakeup can carry many lines and
	 * whole message bodies, so loop until no further progress can be made,
	 * or until a reply handler disconnected the client, which drops the
	 * socket and any input still buffered for it. */
	while (client->source != NULL) {
		if (client->recv_state == CLIENT_RECV_STATUS ||
		    client->recv_state == CLIENT_RECV_HEADERS) {
			char *buf = NULL;
			int res;

			if ((res = read_line(client, &buf)) < 0)
				return res;
			if (buf == NULL)
				return 0;	/* partial line, wait for more data */

			if (client->recv_state == CLIENT_RECV_STATUS)
				res = process_status(client, buf);
			else
				res = process_header(client, buf);
			if (res < 0)
				return res;
		}
		else if (client->recv_state == CLIENT_RECV_CONTENT) {
			int res = process_content(client);
			if (res < 0)
				return res;
			if (client->recv_state == CLIENT_RECV_CONTENT)
				return 0;	/* need more body bytes */
		}
		else {
			spa_assert_not_reached();
		}
	}
	return 0;
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
			data = SPA_PTROFF(msg->payload, msg->offset, void);
			size = msg->len - msg->offset;
		} else {
			pw_log_debug("client %p: sent %zu bytes", client, msg->len);
			spa_list_remove(&msg->link);
			/* Keep the payload alive until the reply is handled (or the
			 * message is dropped); message_free releases both together. */
			if (msg->reply != NULL)
				spa_list_append(&client->pending, &msg->link);
			else
				message_free(msg);
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
		socklen_t len = sizeof(res);
		if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &res, &len) < 0)
			res = errno;
		res = -res;
		goto error;
	}
	if (mask & SPA_IO_IN) {
		if ((res = process_input(client)) < 0)
			goto error;
		/* A reply handler may have disconnected the client. */
		if (client->source == NULL)
			goto done;
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
	int res, fd = -1;
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

	client->source = pw_loop_add_io(client->loop, spa_steal_fd(fd),
			SPA_IO_IN | SPA_IO_OUT | SPA_IO_HUP | SPA_IO_ERR,
			true, on_source_io, client);

	if (client->source == NULL) {
		res = -errno;
		pw_log_error("%p: source create failed: %m", client);
		goto error;
	}
	client->connecting = true;
	free(client->session_id);
	client->session_id = strdup(session_id);
	if (client->session_id == NULL) {
		res = -errno;
		goto error;
	}
	pw_log_info("%p: connecting", client);

	return 0;
error:
	if (client->source)
		pw_loop_destroy_source(client->loop, client->source);
	client->source = NULL;
	return res;
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
	/* The cipher context is owned by the caller (installed via
	 * pw_rtsp_client_set_cipher_ctx); only drop our borrowed reference. */
	client->cipher_ctx = NULL;
	client->decrypt = NULL;
	client->encrypt = NULL;
	pw_array_reset(&client->in_cipher);
	pw_array_reset(&client->in_plain);
	client->in_plain_offset = 0;

	spa_list_consume(msg, &client->messages, link) {
		spa_list_remove(&msg->link);
		message_free(msg);
	}
	spa_list_consume(msg, &client->pending, link) {
		spa_list_remove(&msg->link);
		message_free(msg);
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

	fseek(f, offsetof(struct message, data), SEEK_SET);

	if (client->encrypt != NULL && client->cipher_ctx != NULL)
		fprintf(f, "%s %s HTTP/1.1\r\n", cmd, url);
	else
		fprintf(f, "%s %s RTSP/1.0\r\n", cmd, url);

	cseq = ++client->cseq;
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

	if (client->encrypt != NULL && client->cipher_ctx != NULL) {
		uint8_t *enc_buf;
		size_t enc_len;
		ssize_t enc = client->encrypt(&enc_buf, &enc_len, SPA_PTROFF(msg, sizeof(*msg), uint8_t), len - sizeof(*msg), client->cipher_ctx);
		if (enc < 0) {
			free(msg);
			return enc;
		}
		msg->payload = enc_buf;
		msg->len = enc_len;
	}
	else {
		msg->payload = SPA_PTROFF(msg, offsetof(struct message, data), unsigned char);
		msg->len = len - offsetof(struct message, data);
	}
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
