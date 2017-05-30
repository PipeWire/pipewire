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

/** \enum pw_stream_state The state of a stream \memberof pw_stream */
enum pw_stream_state {
	PW_STREAM_STATE_ERROR = -1,		/**< the strean is in error */
	PW_STREAM_STATE_UNCONNECTED = 0,	/**< unconnected */
	PW_STREAM_STATE_CONNECTING = 1,		/**< connection is in progress */
	PW_STREAM_STATE_CONFIGURE = 2,		/**< stream is being configured */
	PW_STREAM_STATE_READY = 3,		/**< stream is ready */
	PW_STREAM_STATE_PAUSED = 4,		/**< paused, fully configured but not
						  *  processing data yet */
	PW_STREAM_STATE_STREAMING = 5		/**< streaming */
};

/** Convert a stream state to a readable string \memberof pw_stream */
const char * pw_stream_state_as_string(enum pw_stream_state state);

/** \enum pw_stream_flags Extra flags that can be used in \ref pw_stream_connect() \memberof pw_stream */
enum pw_stream_flags {
	PW_STREAM_FLAG_NONE = 0,		/**< no flags */
	PW_STREAM_FLAG_AUTOCONNECT = (1 << 0),	/**< don't try to automatically connect
						  *  this stream */
	PW_STREAM_FLAG_CLOCK_UPDATE = (1 << 1),	/**< request periodic clock updates for
						  *  this stream */
};

/** \enum pw_stream_mode The method for transfering data for a stream \memberof pw_stream */
enum pw_stream_mode {
	PW_STREAM_MODE_BUFFER = 0,	/**< data is placed in buffers */
	PW_STREAM_MODE_RINGBUFFER = 1,	/**< a ringbuffer is used to exchange data */
};

/** A time structure \memberof pw_stream */
struct pw_time {
	int64_t now;		/**< the monotonic time */
	int64_t ticks;		/**< the ticks at \a now */
	int32_t rate;		/**< the rate of \a ticks */
};

/** \class pw_stream
 *
 * \brief PipeWire stream object class
 *
 * The stream object provides a convenient way to send and
 * receive data streams from/to PipeWire.
 */
struct pw_stream {
	struct pw_context *context;	/**< the owner context */
	struct spa_list link;		/**< link in the context */

	char *name;				/**< the name of the stream */
	struct pw_properties *properties;	/**< properties of the stream */

	/** Emited when the stream is destroyed */
	PW_SIGNAL(destroy_signal, (struct pw_listener *listener, struct pw_stream *stream));

	enum pw_stream_state state;		/**< stream state */
	char *error;				/**< error reason when state is in error */
	/** Emited when the stream state changes */
	PW_SIGNAL(state_changed, (struct pw_listener *listener, struct pw_stream *stream));

	/** Emited when the format changed. The listener should call
	 * pw_stream_finish_format() to complete format negotiation */
	PW_SIGNAL(format_changed, (struct pw_listener *listener,
				   struct pw_stream *stream, struct spa_format *format));

	/** Emited when a new buffer was created for this stream */
	PW_SIGNAL(add_buffer, (struct pw_listener *listener,
			       struct pw_stream *stream, uint32_t id));
	/** Emited when a buffer was destroyed for this stream */
	PW_SIGNAL(remove_buffer, (struct pw_listener *listener,
				  struct pw_stream *stream, uint32_t id));
	/** Emited when a buffer can be reused (for playback streams) or
	 *  is filled (for capture streams */
	PW_SIGNAL(new_buffer, (struct pw_listener *listener,
			       struct pw_stream *stream, uint32_t id));
	/** Emited when a buffer is needed (for playback streams) */
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
