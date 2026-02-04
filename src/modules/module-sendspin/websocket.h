/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2026 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_WEBSOCKET_H
#define PIPEWIRE_WEBSOCKET_H

#include <stdarg.h>

#include <pipewire/pipewire.h>

#ifdef __cplusplus
extern "C" {
#endif

struct pw_websocket;
struct pw_websocket_connection;

#define PW_WEBSOCKET_OPCODE_TEXT	0x1
#define PW_WEBSOCKET_OPCODE_BINARY	0x2
#define PW_WEBSOCKET_OPCODE_CLOSE	0x8
#define PW_WEBSOCKET_OPCODE_PING	0x9
#define PW_WEBSOCKET_OPCODE_PONG	0xa

struct pw_websocket_connection_events {
#define PW_VERSION_WEBSOCKET_CONNECTION_EVENTS	0
	uint32_t version;

	void (*destroy) (void *data);
	void (*error) (void *data, int res, const char *reason);
	void (*disconnected) (void *data);

	void (*message) (void *data,
			int opcode, void *payload, size_t size);
};

void pw_websocket_connection_add_listener(struct pw_websocket_connection *conn,
		struct spa_hook *listener,
		const struct pw_websocket_connection_events *events, void *data);

void pw_websocket_connection_destroy(struct pw_websocket_connection *conn);
void pw_websocket_connection_disconnect(struct pw_websocket_connection *conn, bool drain);

int pw_websocket_connection_address(struct pw_websocket_connection *conn,
		struct sockaddr *addr, socklen_t addr_len);

int pw_websocket_connection_send(struct pw_websocket_connection *conn,
		uint8_t opcode, const struct iovec *iov, size_t iov_len);

int pw_websocket_connection_send_text(struct pw_websocket_connection *conn,
		const char *payload, size_t payload_len);


struct pw_websocket_events {
#define PW_VERSION_WEBSOCKET_EVENTS	0
	uint32_t version;

	void (*destroy) (void *data);

	void (*connected) (void *data, void *user,
			struct pw_websocket_connection *conn, const char *path);
};

struct pw_websocket * pw_websocket_new(struct pw_loop *main_loop,
				struct spa_dict *props);

void pw_websocket_destroy(struct pw_websocket *ws);

void pw_websocket_add_listener(struct pw_websocket *ws,
		struct spa_hook *listener,
		const struct pw_websocket_events *events, void *data);

int pw_websocket_connect(struct pw_websocket *ws, void *user,
		const char *hostname, const char *service, const char *path);

int pw_websocket_listen(struct pw_websocket *ws, void *user,
		const char *hostname, const char *service, const char *paths);

int pw_websocket_cancel(struct pw_websocket *ws, void *user);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_WEBSOCKET_H */
