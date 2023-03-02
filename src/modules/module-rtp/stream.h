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

#define ERROR_MSEC			2
#define DEFAULT_SESS_LATENCY		100

#define DEFAULT_MTU		1280
#define DEFAULT_MIN_PTIME	2
#define DEFAULT_MAX_PTIME	20

struct rtp_stream_events {
#define RTP_VERSION_STREAM_EVENTS        0
	uint32_t version;

	void (*destroy) (void *data);

	void (*state_changed) (void *data, bool started, const char *error);

	void (*send_packet) (void *data, struct iovec *iov, size_t iovlen);
};

struct rtp_stream *rtp_stream_new(struct pw_core *core,
		enum pw_direction direction, struct pw_properties *props,
		const struct rtp_stream_events *events, void *data);

void rtp_stream_destroy(struct rtp_stream *s);

int rtp_stream_receive_packet(struct rtp_stream *s, uint8_t *buffer, size_t len);


#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_RTP_STREAM_H */
