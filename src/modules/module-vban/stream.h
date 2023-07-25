/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2023 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#ifndef PIPEWIRE_VBAN_STREAM_H
#define PIPEWIRE_VBAN_STREAM_H

#ifdef __cplusplus
extern "C" {
#endif

struct vban_stream;

#define DEFAULT_FORMAT		"S16LE"
#define DEFAULT_RATE		44100
#define DEFAULT_CHANNELS	2
#define DEFAULT_POSITION	"[ FL FR ]"

#define ERROR_MSEC		2
#define DEFAULT_SESS_LATENCY	100

#define DEFAULT_MTU		VBAN_PROTOCOL_MAX_SIZE
#define DEFAULT_MIN_PTIME	2
#define DEFAULT_MAX_PTIME	20

struct vban_stream_events {
#define VBAN_VERSION_STREAM_EVENTS        0
	uint32_t version;

	void (*destroy) (void *data);

	void (*state_changed) (void *data, bool started, const char *error);

	void (*send_packet) (void *data, struct iovec *iov, size_t iovlen);

	void (*send_feedback) (void *data, uint32_t senum);
};

struct vban_stream *vban_stream_new(struct pw_core *core,
		enum pw_direction direction, struct pw_properties *props,
		const struct vban_stream_events *events, void *data);

void vban_stream_destroy(struct vban_stream *s);

int vban_stream_receive_packet(struct vban_stream *s, uint8_t *buffer, size_t len);

uint64_t vban_stream_get_time(struct vban_stream *s, uint64_t *rate);


#ifdef __cplusplus
}
#endif

#endif /* PIPEWIRE_VBAN_STREAM_H */
