/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __PIPEWIRE_STREAM_H__
#define __PIPEWIRE_STREAM_H__

#include <spa/buffer.h>
#include <spa/format.h>

#include <pipewire/client/context.h>

#ifdef __cplusplus
extern "C" {
#endif

enum pw_stream_state {
	PW_STREAM_STATE_ERROR = -1,
	PW_STREAM_STATE_UNCONNECTED = 0,
	PW_STREAM_STATE_CONNECTING = 1,
	PW_STREAM_STATE_CONFIGURE = 2,
	PW_STREAM_STATE_READY = 3,
	PW_STREAM_STATE_PAUSED = 4,
	PW_STREAM_STATE_STREAMING = 5
};

const char *
pw_stream_state_as_string(enum pw_stream_state state);

enum pw_stream_flags {
	PW_STREAM_FLAG_NONE = 0,
	PW_STREAM_FLAG_AUTOCONNECT = (1 << 0),
	PW_STREAM_FLAG_CLOCK_UPDATE = (1 << 1),
};

enum pw_stream_mode {
	PW_STREAM_MODE_BUFFER = 0,
	PW_STREAM_MODE_RINGBUFFER = 1,
};

struct pw_time {
	int64_t now;
	int64_t ticks;
	int32_t rate;
};

/**
 * pw_stream:
 *
 * PipeWire stream object class.
 */
struct pw_stream {
	struct pw_context *context;
	struct spa_list link;

	char *name;
	struct pw_properties *properties;

	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_stream *stream));

	enum pw_stream_state state;
	char *error;
	PW_SIGNAL(state_changed, (struct pw_listener *listener, struct pw_stream *stream));

	PW_SIGNAL(format_changed, (struct pw_listener *listener,
				    struct pw_stream *stream, struct spa_format *format));

	PW_SIGNAL(add_buffer, (struct pw_listener *listener,
				struct pw_stream *stream, uint32_t id));
	PW_SIGNAL(remove_buffer, (struct pw_listener *listener,
				   struct pw_stream *stream, uint32_t id));
	PW_SIGNAL(new_buffer, (struct pw_listener *listener,
				struct pw_stream *stream, uint32_t id));
	PW_SIGNAL(need_buffer, (struct pw_listener *listener, struct pw_stream *stream));
};

struct pw_stream *
pw_stream_new(struct pw_context *context,
	      const char *name, struct pw_properties *props);
void
pw_stream_destroy(struct pw_stream *stream);

bool
pw_stream_connect(struct pw_stream *stream,
		  enum pw_direction direction,
		  enum pw_stream_mode mode,
		  const char *port_path,
		  enum pw_stream_flags flags,
		  uint32_t n_possible_formats,
		  struct spa_format **possible_formats);
bool
pw_stream_disconnect(struct pw_stream *stream);

bool
pw_stream_finish_format(struct pw_stream *stream,
			int res, struct spa_param **params, uint32_t n_params);

bool
pw_stream_get_time(struct pw_stream *stream, struct pw_time *time);

uint32_t
pw_stream_get_empty_buffer(struct pw_stream *stream);

bool
pw_stream_recycle_buffer(struct pw_stream *stream, uint32_t id);

struct spa_buffer *
pw_stream_peek_buffer(struct pw_stream *stream, uint32_t id);

bool
pw_stream_send_buffer(struct pw_stream *stream, uint32_t id);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_STREAM_H__ */
