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

/** \page page_streams Media Streams
 *
 * \section sec_overview Overview
 *
 * Media streams are used to exchange data with the PipeWire server. A
 * stream is a wrapper around a \ref pw_client_node with one port.
 *
 * Streams can be used to:
 *
 * \li Consume a stream from PipeWire. This is a PW_DIRECTION_INPUT stream.
 * \li Produce a stream to PipeWire. This is a PW_DIRECTION_OUTPUT stream
 *
 * You can connect the stream port to a specific server port or let PipeWire
 * choose a port for you.
 *
 * For more complicated nodes such as filters or ports with multiple
 * inputs and/or outputs you will need to manage the \ref pw_client_node proxy
 * yourself.
 *
 * \section sec_create Create
 *
 * Make a new stream with \ref pw_stream_new(). You will need to specify
 * a name for the stream and extra properties. You can use \ref
 * pw_fill_stream_properties() to get a basic set of properties for the
 * stream.
 *
 * Once the stream is created, the state_changed signal should be used to
 * track the state of the stream.
 *
 * \section sec_connect Connect
 *
 * The stream is initially unconnected. To connect the stream, use
 * \ref pw_stream_connect(). Pass the desired direction as an argument.
 *
 * \subsection ssec_stream_mode Stream modes
 *
 * The stream mode specifies how the data will be exchanged with PipeWire.
 * The following stream modes are available
 *
 * \li \ref PW_STREAM_MODE_BUFFER: data is exchanged with fixed size
 *	buffers. This is ideal for video frames or equal sized audio
 *	frames.
 * \li \ref PW_STREAM_MODE_RINGBUFFER: data is exhanged with a fixed
 *	size ringbuffer. This is ideal for variable sized audio packets
 *	or compressed media.
 *
 * \subsection ssec_stream_target Stream target
 *
 * To make the newly connected stream automatically connect to an existing
 * PipeWire node, use the \ref PW_STREAM_FLAG_AUTOCONNECT and the port_path
 * argument while connecting.
 *
 * \subsection ssec_stream_formats Stream formats
 *
 * An array of possible formats that this stream can consume or provide
 * must be specified.
 *
 * \section sec_format Format negotiation
 *
 * After connecting the stream, it will transition to the \ref
 * PW_STREAM_STATE_CONFIGURE state. In this state the format will be
 * negotiated by the PipeWire server.
 *
 * Once the format has been selected, the format_changed signal is
 * emited with the configured format as a parameter.
 *
 * The client should now prepare itself to deal with the format and
 * complete the negotiation procedure with a call to \ref
 * pw_stream_finish_format().
 *
 * As arguments to \ref pw_stream_finish_format() an array of spa_param
 * structures must be given. They contain parameters such as buffer size,
 * number of buffers, required metadata and other parameters for the
 * media buffers.
 *
 * \section sec_buffers Buffer negotiation
 *
 * After completing the format negotiation, PipeWire will allocate and
 * notify the stream of the buffers that will be used to exchange data
 * between client and server.
 *
 * With the add_buffer signal, a stream will be notified of a new buffer
 * that can be used for data transport.
 *
 * Afer the buffers are negotiated, the stream will transition to the
 * \ref PW_STREAM_STATE_PAUSED state.
 *
 * \section sec_streaming Streaming
 *
 * From the \ref PW_STREAM_STATE_PAUSED state, the stream can be set to
 * the \ref PW_STREAM_STATE_STREAMING state by the PipeWire server when
 * data transport is started.
 *
 * Depending on how the stream was connected it will need to Produce or
 * Consume data for/from PipeWire as explained in the following
 * subsections.
 *
 * \subsection ssec_consume Consume data
 *
 * The new_buffer signal is emited for each new buffer can can be
 * consumed.
 *
 * \ref pw_stream_peek_buffer() should be used to get the data and metadata
 * of the buffer.
 *
 * When the buffer is no longer in use, call \ref pw_stream_recycle_buffer()
 * to let PipeWire reuse the buffer.
 *
 * \subsection ssec_produce Produce data
 *
 * The need_buffer signal is emited when PipeWire needs a new buffer for this
 * stream.
 *
 * \ref pw_stream_get_empty_buffer() gives the id of an empty buffer.
 * Use \ref pw_stream_peek_buffer() to get the data and metadata that should
 * be filled.
 *
 * To send the filled buffer, use \ref pw_stream_send_buffer().
 *
 * The new_buffer signal is emited when PipeWire no longer uses the buffer
 * and it can be safely reused.
 *
 * \section sec_stream_disconnect Disconnect
 *
 * Use \ref pw_stream_disconnect() to disconnect a stream after use.
 */

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
	PW_STREAM_FLAG_AUTOCONNECT = (1 << 0),	/**< try to automatically connect
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
 *
 * See also \ref page_streams and \ref page_client_api
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
	      const char *name,
	      struct pw_properties *props);
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
