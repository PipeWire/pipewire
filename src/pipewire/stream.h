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

#ifdef __cplusplus
extern "C" {
#endif

/** \page page_streams Media Streams
 *
 * \section sec_overview Overview
 *
 * Media streams are used to exchange data with the PipeWire server. A
 * stream is a wrapper around a proxy for a \ref pw_client_node with
 * just one port.
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
 * inputs and/or outputs you will need to create a pw_node yourself and
 * export it with \ref pw_remote_export.
 *
 * \section sec_create Create
 *
 * Make a new stream with \ref pw_stream_new(). You will need to specify
 * a name for the stream and extra properties. You can use \ref
 * pw_fill_stream_properties() to get a basic set of properties for the
 * stream.
 *
 * Once the stream is created, the state_changed event should be used to
 * track the state of the stream.
 *
 * \section sec_connect Connect
 *
 * The stream is initially unconnected. To connect the stream, use
 * \ref pw_stream_connect(). Pass the desired direction as an argument.
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
 * Once the format has been selected, the format_changed event is
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
 * With the add_buffer event, a stream will be notified of a new buffer
 * that can be used for data transport. You can attach user_data to these
 * buffers.
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
 * The process event is emited for each new buffer that can can be
 * consumed.
 *
 * \ref pw_stream_dequeue_buffer() should be used to get the data and
 * metadata of the buffer.
 *
 * When the buffer is no longer in use, call \ref pw_stream_queue_buffer()
 * to let PipeWire reuse the buffer.
 *
 * \subsection ssec_produce Produce data
 *
 * \ref pw_stream_dequeue_buffer() gives an empty buffer that can be filled.
 *
 * Filled buffers should be queued with \ref pw_stream_queue_buffer().
 *
 * The process event is emited when PipeWire has emptied a buffer that
 * can now be refilled.
 *
 * \section sec_stream_disconnect Disconnect
 *
 * Use \ref pw_stream_disconnect() to disconnect a stream after use.
 */
/** \class pw_stream
 *
 * \brief PipeWire stream object class
 *
 * The stream object provides a convenient way to send and
 * receive data streams from/to PipeWire.
 *
 * See also \ref page_streams and \ref page_core_api
 */
struct pw_stream;

#include <spa/buffer/buffer.h>
#include <spa/param/param.h>

#include <pipewire/remote.h>

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

struct pw_buffer {
	struct spa_buffer *buffer;	/* the spa buffer */
	void *user_data;		/* user data attached to the buffer */
	uint64_t size;			/* For input streams, this field is set by pw_stream
					   with the duration of the buffer in ticks.
					   For output streams, this field is set by the user.
					   This field is added for all queued buffers and
					   returned in the time info. */
};

/** Events for a stream */
struct pw_stream_events {
#define PW_VERSION_STREAM_EVENTS	0
	uint32_t version;

	void (*destroy) (void *data);
	/** when the stream state changes */
	void (*state_changed) (void *data, enum pw_stream_state old,
				enum pw_stream_state state, const char *error);
	/** when the format changed. The listener should call
	 * pw_stream_finish_format() from within this callback or later to complete
	 * the format negotiation and start the buffer negotiation. */
	void (*format_changed) (void *data, const struct spa_pod *format);

        /** when a new buffer was created for this stream */
        void (*add_buffer) (void *data, struct pw_buffer *buffer);
        /** when a buffer was destroyed for this stream */
        void (*remove_buffer) (void *data, struct pw_buffer *buffer);

        /** when a buffer can be queued (for playback streams) or
         *  dequeued (for capture streams). This is normally called from the
	 *  mainloop but can also be called directly from the realtime data
	 *  thread if the user is prepared to deal with this. */
        void (*process) (void *data);
};

/** Convert a stream state to a readable string \memberof pw_stream */
const char * pw_stream_state_as_string(enum pw_stream_state state);

/** \enum pw_stream_flags Extra flags that can be used in \ref pw_stream_connect() \memberof pw_stream */
enum pw_stream_flags {
	PW_STREAM_FLAG_NONE = 0,			/**< no flags */
	PW_STREAM_FLAG_AUTOCONNECT	= (1 << 0),	/**< try to automatically connect
							  *  this stream */
	PW_STREAM_FLAG_INACTIVE		= (1 << 1),	/**< start the stream inactive */
	PW_STREAM_FLAG_MAP_BUFFERS	= (1 << 2),	/**< mmap the buffers */
	PW_STREAM_FLAG_DRIVER		= (1 << 3),	/**< be a driver */
	PW_STREAM_FLAG_RT_PROCESS	= (1 << 4),	/**< call process from the realtime
							  *  thread */
	PW_STREAM_FLAG_NO_CONVERT	= (1 << 5),	/**< don't convert format */
	PW_STREAM_FLAG_EXCLUSIVE	= (1 << 6),	/**< require exclusive access to the
							  *  device */
};

/** Create a new unconneced \ref pw_stream \memberof pw_stream
 * \return a newly allocated \ref pw_stream */
struct pw_stream *
pw_stream_new(struct pw_remote *remote,		/**< a \ref pw_remote */
	      const char *name,			/**< a stream name */
	      struct pw_properties *props	/**< stream properties, ownership is taken */);

struct pw_stream *
pw_stream_new_simple(struct pw_loop *loop,	/**< a \ref pw_loop to use */
		     const char *name,		/**< a stream name */
		     struct pw_properties *props,/**< stream properties, ownership is taken */
		     const struct pw_stream_events *events,	/**< stream events */
		     void *data					/**< data passed to events */);

/** Destroy a stream \memberof pw_stream */
void pw_stream_destroy(struct pw_stream *stream);

void pw_stream_add_listener(struct pw_stream *stream,
			    struct spa_hook *listener,
			    const struct pw_stream_events *events,
			    void *data);

enum pw_stream_state pw_stream_get_state(struct pw_stream *stream, const char **error);

const char *pw_stream_get_name(struct pw_stream *stream);

struct pw_remote *pw_stream_get_remote(struct pw_stream *stream);

/** Indicates that the stream is live, boolean default false */
#define PW_STREAM_PROP_IS_LIVE		"pipewire.latency.is-live"
/** The minimum latency of the stream, int, default 0 */
#define PW_STREAM_PROP_LATENCY_MIN	"pipewire.latency.min"
/** The maximum latency of the stream, int default MAXINT */
#define PW_STREAM_PROP_LATENCY_MAX	"pipewire.latency.max"

const struct pw_properties *pw_stream_get_properties(struct pw_stream *stream);

/** Connect a stream for input or output on \a port_path. \memberof pw_stream
 * \return 0 on success < 0 on error.
 *
 * You should connect to the process event and use pw_stream_dequeue_buffer()
 * to get the latest metadata and data. */
int
pw_stream_connect(struct pw_stream *stream,		/**< a \ref pw_stream */
		  enum pw_direction direction,		/**< the stream direction */
		  const char *port_path,		/**< the port path to connect to or NULL
							  *  to let the server choose a port */
		  enum pw_stream_flags flags,		/**< stream flags */
		  const struct spa_pod **params,	/**< an array with params. The params
							  *  should ideally contain supported
							  *  formats. */
		  uint32_t n_params			/**< number of items in \a params */);

/** Get the node ID of the stream. \memberof pw_stream
 * \return node ID. */
uint32_t
pw_stream_get_node_id(struct pw_stream *stream);

/** Disconnect \a stream \memberof pw_stream */
int pw_stream_disconnect(struct pw_stream *stream);

/** Complete the negotiation process with result code \a res \memberof pw_stream
 *
 * This function should be called after notification of the format.

 * When \a res indicates success, \a params contain the parameters for the
 * allocation state.  */
void
pw_stream_finish_format(struct pw_stream *stream,	/**< a \ref pw_stream */
			int res,			/**< a result code */
			const struct spa_pod **params,	/**< an array of params. The params should
							  *  ideally contain parameters for doing
							  *  buffer allocation. */
			uint32_t n_params		/**< number of elements in \a params */);


/** Audio controls */
#define PW_STREAM_CONTROL_VOLUME	"volume"

/** Video controls */
#define PW_STREAM_CONTROL_CONTRAST	"contrast"
#define PW_STREAM_CONTROL_BRIGHTNESS	"brightness"
#define PW_STREAM_CONTROL_HUE		"hue"
#define PW_STREAM_CONTROL_SATURATION	"saturation"

/** Set a control value */
int pw_stream_set_control(struct pw_stream *stream, const char *name, float value);
/** Get a control value */
int pw_stream_get_control(struct pw_stream *stream, const char *name, float *value);

/** Activate or deactivate the stream \memberof pw_stream */
int pw_stream_set_active(struct pw_stream *stream, bool active);

/** A time structure \memberof pw_stream */
struct pw_time {
	int64_t now;			/**< the monotonic time */
	struct spa_fraction rate;	/**< the rate of \a ticks */
	uint64_t ticks;			/**< the ticks at \a now. This is the current time that
					     the remote end is reading/writing. */
	uint64_t delay;			/**< delay to device, add to ticks for INPUT streams and
					     subtract from ticks for OUTPUT streams to get the
					     time of the device. */
	uint64_t queued;		/**< data queued in the stream, this is the sum
					     of the size fields in the pw_buffer that are
					     currently queued */
};

/** Query the time on the stream \memberof pw_stream */
int pw_stream_get_time(struct pw_stream *stream, struct pw_time *time);

/** Get a buffer that can be filled for playback streams or consumed
 * for capture streams.  */
struct pw_buffer *pw_stream_dequeue_buffer(struct pw_stream *stream);

/** Submit a buffer for playback or recycle a buffer for capture. */
int pw_stream_queue_buffer(struct pw_stream *stream, struct pw_buffer *buffer);


#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_STREAM_H__ */
