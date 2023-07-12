/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_RTSP_CLIENT_H
#define PIPEWIRE_RTSP_CLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdarg.h>

#include <pipewire/pipewire.h>

struct pw_rtsp_client;

struct pw_rtsp_client_events {
#define PW_VERSION_RTSP_CLIENT_EVENTS	0
	uint32_t version;

	void (*destroy) (void *data);

	void (*connected) (void *data);
	void (*error) (void *data, int res);
	void (*disconnected) (void *data);

	void (*message) (void *data, int status,
			const struct spa_dict *headers);

};

struct pw_rtsp_client * pw_rtsp_client_new(struct pw_loop *main_loop,
				struct pw_properties *props,
				size_t user_data_size);

void pw_rtsp_client_destroy(struct pw_rtsp_client *client);

void *pw_rtsp_client_get_user_data(struct pw_rtsp_client *client);
const char *pw_rtsp_client_get_url(struct pw_rtsp_client *client);

void pw_rtsp_client_add_listener(struct pw_rtsp_client *client,
		struct spa_hook *listener,
		const struct pw_rtsp_client_events *events, void *data);

const struct pw_properties *pw_rtsp_client_get_properties(struct pw_rtsp_client *client);

int pw_rtsp_client_connect(struct pw_rtsp_client *client,
		const char *hostname, uint16_t port, const char *session_id);
int pw_rtsp_client_disconnect(struct pw_rtsp_client *client);

int pw_rtsp_client_get_local_ip(struct pw_rtsp_client *client,
		int *version, char *ip, size_t len);

int pw_rtsp_client_url_send(struct pw_rtsp_client *client, const char *url,
		const char *cmd, const struct spa_dict *headers,
		const char *content_type, const void *content, size_t content_length,
		int (*reply) (void *user_data, int status, const struct spa_dict *headers, const struct pw_array *content),
		void *user_data);

int pw_rtsp_client_send(struct pw_rtsp_client *client,
		const char *cmd, const struct spa_dict *headers,
		const char *content_type, const char *content,
		int (*reply) (void *user_data, int status, const struct spa_dict *headers, const struct pw_array *content),
		void *user_data);


#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_RTSP_CLIENT_H */
