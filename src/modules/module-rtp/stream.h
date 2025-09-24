/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_RTP_STREAM_H
#define PIPEWIRE_RTP_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

struct rtp_stream;

#define DEFAULT_FORMAT		"S16BE"
#define DEFAULT_RATE		48000
#define DEFAULT_CHANNELS	2
#define DEFAULT_POSITION	"[ FL FR ]"

#define ERROR_MSEC		2.0f
#define DEFAULT_SESS_LATENCY	100.0f

#define IP4_HEADER_SIZE		20
#define IP6_HEADER_SIZE		40
#define UDP_HEADER_SIZE		8
/* 12 bytes RTP header */
#define RTP_HEADER_SIZE		12

#define DEFAULT_MTU		1280
#define DEFAULT_MIN_PTIME	2.0f
#define DEFAULT_MAX_PTIME	20.0f

struct rtp_stream_events {
#define RTP_VERSION_STREAM_EVENTS        0
	uint32_t version;

	void (*destroy) (void *data);

	void (*report_error) (void *data, const char *error);

	/* Requests the network connection to be opened. If result is non-NULL,
	 * the call sets it to >0 in case of success, and a negative errno error
	 * code in case of failure. (Result value 0 is unused.) */
	void (*open_connection) (void *data, int *result);

	/* Requests the network connection to be closed. If result is non-NULL,
	 * the call sets it to >0 in case of success, 0 if the connection was
	 * already closed, and a negative errno error code in case of failure. */
	void (*close_connection) (void *data, int *result);

	void (*param_changed) (void *data, uint32_t id, const struct spa_pod *param);

	void (*send_packet) (void *data, struct iovec *iov, size_t iovlen);

	void (*send_feedback) (void *data, uint32_t seqnum);
};

struct rtp_stream *rtp_stream_new(struct pw_core *core,
		enum pw_direction direction, struct pw_properties *props,
		const struct rtp_stream_events *events, void *data);

void rtp_stream_destroy(struct rtp_stream *s);

int rtp_stream_update_properties(struct rtp_stream *s, const struct spa_dict *dict);

int rtp_stream_receive_packet(struct rtp_stream *s, uint8_t *buffer, size_t len);

uint64_t rtp_stream_get_time(struct rtp_stream *s, uint32_t *rate);

uint16_t rtp_stream_get_seq(struct rtp_stream *s);

size_t rtp_stream_get_mtu(struct rtp_stream *s);

void rtp_stream_set_first(struct rtp_stream *s);

int rtp_stream_set_active(struct rtp_stream *s, bool active);
void rtp_stream_set_error(struct rtp_stream *s, int res, const char *error);
enum pw_stream_state rtp_stream_get_state(struct rtp_stream *s, const char **error);

int rtp_stream_set_param(struct rtp_stream *s, uint32_t id, const struct spa_pod *param);

int rtp_stream_update_params(struct rtp_stream *stream,
			const struct spa_pod **params,
			uint32_t n_params);

void rtp_stream_update_process_latency(struct rtp_stream *stream,
				const struct spa_process_latency_info *process_latency);

#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_RTP_STREAM_H */
