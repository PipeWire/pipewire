/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 * Copyright © 2019 Collabora Ltd.
 *   @author George Kiagiadakis <george.kiagiadakis@collabora.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstpwaudioringbuffer.h"

#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

GST_DEBUG_CATEGORY_STATIC (pw_audio_ring_buffer_debug);
#define GST_CAT_DEFAULT pw_audio_ring_buffer_debug

#define gst_pw_audio_ring_buffer_parent_class parent_class
G_DEFINE_TYPE (GstPwAudioRingBuffer, gst_pw_audio_ring_buffer, GST_TYPE_AUDIO_RING_BUFFER);

enum
{
  PROP_0,
  PROP_ELEMENT,
  PROP_DIRECTION,
  PROP_PROPS
};

static void
gst_pw_audio_ring_buffer_init (GstPwAudioRingBuffer * self)
{
  self->loop = pw_loop_new (NULL);
  self->main_loop = pw_thread_loop_new (self->loop, "pw-audioringbuffer-loop");
  self->core = pw_core_new (self->loop, NULL, 0);
}

static void
gst_pw_audio_ring_buffer_finalize (GObject * object)
{
  GstPwAudioRingBuffer *self = GST_PW_AUDIO_RING_BUFFER (object);

  pw_core_destroy (self->core);
  pw_thread_loop_destroy (self->main_loop);
  pw_loop_destroy (self->loop);
}

static void
gst_pw_audio_ring_buffer_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPwAudioRingBuffer *self = GST_PW_AUDIO_RING_BUFFER (object);

  switch (prop_id) {
    case PROP_ELEMENT:
      self->elem = g_value_get_object (value);
      break;

    case PROP_DIRECTION:
      self->direction = g_value_get_int (value);
      break;

    case PROP_PROPS:
      self->props = g_value_get_pointer (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
on_remote_state_changed (void *data, enum pw_remote_state old,
    enum pw_remote_state state, const char *error)
{
  GstPwAudioRingBuffer *self = GST_PW_AUDIO_RING_BUFFER (data);

  GST_DEBUG_OBJECT (self->elem, "got remote state %d", state);

  switch (state) {
    case PW_REMOTE_STATE_UNCONNECTED:
    case PW_REMOTE_STATE_CONNECTING:
    case PW_REMOTE_STATE_CONNECTED:
      break;
    case PW_REMOTE_STATE_ERROR:
      GST_ELEMENT_ERROR (self->elem, RESOURCE, FAILED,
          ("remote error: %s", error), (NULL));
      break;
  }
  pw_thread_loop_signal (self->main_loop, FALSE);
}

static const struct pw_remote_events remote_events = {
  PW_VERSION_REMOTE_EVENTS,
  .state_changed = on_remote_state_changed,
};

static gboolean
wait_for_remote_state (GstPwAudioRingBuffer *self,
    enum pw_remote_state target)
{
  while (TRUE) {
    enum pw_remote_state state = pw_remote_get_state (self->remote, NULL);
    if (state == target)
      return TRUE;
    if (state == PW_REMOTE_STATE_ERROR)
      return FALSE;
    pw_thread_loop_wait (self->main_loop);
  }
}

static gboolean
gst_pw_audio_ring_buffer_open_device (GstAudioRingBuffer *buf)
{
  GstPwAudioRingBuffer *self = GST_PW_AUDIO_RING_BUFFER (buf);

  GST_DEBUG_OBJECT (self->elem, "open device");

  if (pw_thread_loop_start (self->main_loop) < 0)
    goto mainloop_error;

  pw_thread_loop_lock (self->main_loop);

  self->remote = pw_remote_new (self->core, NULL, 0);
  pw_remote_add_listener (self->remote, &self->remote_listener, &remote_events,
      self);

  if (self->props->fd == -1)
    pw_remote_connect (self->remote);
  else
    pw_remote_connect_fd (self->remote, self->props->fd);

  GST_DEBUG_OBJECT (self->elem, "waiting for connection");

  if (!wait_for_remote_state (self, PW_REMOTE_STATE_CONNECTED))
    goto connect_error;

  pw_thread_loop_unlock (self->main_loop);

  return TRUE;

  /* ERRORS */
mainloop_error:
  {
    GST_ELEMENT_ERROR (self->elem, RESOURCE, FAILED,
        ("Failed to start mainloop"), (NULL));
    return FALSE;
  }
connect_error:
  {
    pw_thread_loop_unlock (self->main_loop);
    return FALSE;
  }
}

static gboolean
gst_pw_audio_ring_buffer_close_device (GstAudioRingBuffer *buf)
{
  GstPwAudioRingBuffer *self = GST_PW_AUDIO_RING_BUFFER (buf);

  GST_DEBUG_OBJECT (self->elem, "closing device");

  pw_thread_loop_lock (self->main_loop);
  if (self->remote) {
    pw_remote_disconnect (self->remote);
    wait_for_remote_state (self, PW_REMOTE_STATE_UNCONNECTED);
  }
  pw_thread_loop_unlock (self->main_loop);

  pw_thread_loop_stop (self->main_loop);

  if (self->remote) {
    pw_remote_destroy (self->remote);
    self->remote = NULL;
  }
  return TRUE;
}

static void
on_stream_state_changed (void *data, enum pw_stream_state old,
    enum pw_stream_state state, const char *error)
{
  GstPwAudioRingBuffer *self = GST_PW_AUDIO_RING_BUFFER (data);
  GstMessage *msg;

  GST_DEBUG_OBJECT (self->elem, "got stream state: %s",
      pw_stream_state_as_string (state));

  switch (state) {
    case PW_STREAM_STATE_ERROR:
      GST_ELEMENT_ERROR (self->elem, RESOURCE, FAILED,
          ("stream error: %s", error), (NULL));
      break;
    case PW_STREAM_STATE_UNCONNECTED:
      GST_ELEMENT_ERROR (self->elem, RESOURCE, FAILED,
          ("stream disconnected unexpectedly"), (NULL));
      break;
    case PW_STREAM_STATE_CONNECTING:
    case PW_STREAM_STATE_CONFIGURE:
    case PW_STREAM_STATE_READY:
      break;
    case PW_STREAM_STATE_PAUSED:
      if (old == PW_STREAM_STATE_STREAMING) {
        if (GST_STATE (self->elem) != GST_STATE_PAUSED &&
            GST_STATE_TARGET (self->elem) != GST_STATE_PAUSED) {
          GST_DEBUG_OBJECT (self->elem, "requesting GST_STATE_PAUSED");
          msg = gst_message_new_request_state (GST_OBJECT (self->elem),
              GST_STATE_PAUSED);
          gst_element_post_message (self->elem, msg);
        }
      }
      break;
    case PW_STREAM_STATE_STREAMING:
      if (GST_STATE (self->elem) != GST_STATE_PLAYING &&
          GST_STATE_TARGET (self->elem) != GST_STATE_PLAYING) {
        GST_DEBUG_OBJECT (self->elem, "requesting GST_STATE_PLAYING");
        msg = gst_message_new_request_state (GST_OBJECT (self->elem),
            GST_STATE_PLAYING);
        gst_element_post_message (self->elem, msg);
      }
      break;
  }
  pw_thread_loop_signal (self->main_loop, FALSE);
}

static gboolean
wait_for_stream_state (GstPwAudioRingBuffer *self,
    enum pw_stream_state target)
{
  while (TRUE) {
    enum pw_stream_state state = pw_stream_get_state (self->stream, NULL);
    if (state >= target)
      return TRUE;
    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
      return FALSE;
    pw_thread_loop_wait (self->main_loop);
  }
}

static void
on_stream_format_changed (void *data, const struct spa_pod *format)
{
  GstPwAudioRingBuffer *self = GST_PW_AUDIO_RING_BUFFER (data);
  const struct spa_pod *params[1];
  struct spa_pod_builder b = { NULL };
  uint8_t buffer[512];
  const gint b_size = self->segsize * self->channels;

  spa_pod_builder_init (&b, buffer, sizeof (buffer));
  params[0] = spa_pod_builder_add_object (&b,
      SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(16, 1, INT32_MAX),
      SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
      SPA_PARAM_BUFFERS_size,    SPA_POD_Int(b_size),
      SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(self->bpf),
      SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));

  GST_DEBUG_OBJECT (self->elem, "doing finish format, buffer size:%d", b_size);
  pw_stream_finish_format (self->stream, 0, params, 1);
}

static void
on_stream_process (void *data)
{
  GstPwAudioRingBuffer *self = GST_PW_AUDIO_RING_BUFFER (data);
  GstAudioRingBuffer *buf = GST_AUDIO_RING_BUFFER (data);
  struct pw_buffer *b;
  struct spa_data *d;
  gint size;       /*< size to read/write from/to the spa buffer */
  gint offset;     /*< offset to read/write from/to in the spa buffer */
  gint segment;    /*< the current segment number in the ringbuffer */
  guint8 *ringptr; /*< pointer to the beginning of the current segment */
  gint segsize;    /*< the size of one segment in the ringbuffer */
  gint copy_size;  /*< the bytes to copy in one memcpy() invocation */
  gint remain;     /*< remainder of bytes available in the spa buffer */

  if (g_atomic_int_get (&buf->state) != GST_AUDIO_RING_BUFFER_STATE_STARTED) {
    GST_LOG_OBJECT (self->elem, "ring buffer is not started");
    return;
  }

  b = pw_stream_dequeue_buffer (self->stream);
  if (!b) {
    GST_WARNING_OBJECT (self->elem, "no pipewire buffer available");
    return;
  }

  d = &b->buffer->datas[0];

  if (self->direction == PW_DIRECTION_OUTPUT) {
    /* in output mode, always fill the entire spa buffer */
    offset = d->chunk->offset = 0;
    size = d->chunk->size = d->maxsize;
    b->size = size / self->bpf;
  } else {
    offset = SPA_MIN (d->chunk->offset, d->maxsize);
    size = SPA_MIN (d->chunk->size, d->maxsize - offset);
  }

  do {
    gst_audio_ring_buffer_prepare_read (buf, &segment, &ringptr, &segsize);

    /* in INPUT (src) mode, it is possible that the skew algorithm
     * advances the ringbuffer behind our back */
    if (self->segoffset > 0 && self->cur_segment != segment)
      self->segoffset = 0;

    copy_size = SPA_MIN (size, segsize - self->segoffset);

    if (self->direction == PW_DIRECTION_OUTPUT) {
      memcpy (((guint8*) d->data) + offset, ringptr + self->segoffset,
          copy_size);
    } else {
      memcpy (ringptr + self->segoffset, ((guint8*) d->data) + offset,
          copy_size);
    }

    remain = size - (segsize - self->segoffset);

    GST_TRACE_OBJECT (self->elem,
        "seg %d: %s %d bytes remained:%d offset:%d segoffset:%d", segment,
        self->direction == PW_DIRECTION_INPUT ? "INPUT" : "OUTPUT",
        copy_size, remain, offset, self->segoffset);

    if (remain >= 0) {
      offset += (segsize - self->segoffset);
      size = remain;

      /* write silence on the segment we just read */
      if (self->direction == PW_DIRECTION_OUTPUT)
        gst_audio_ring_buffer_clear (buf, segment);

      /* notify that we have read a complete segment */
      gst_audio_ring_buffer_advance (buf, 1);
      self->segoffset = 0;
    } else {
      self->segoffset += size;
      self->cur_segment = segment;
    }
  } while (remain > 0);

  pw_stream_queue_buffer (self->stream, b);
}

static const struct pw_stream_events stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = on_stream_state_changed,
  .format_changed = on_stream_format_changed,
  .process = on_stream_process,
};

static gboolean
copy_properties (GQuark field_id, const GValue *value, gpointer user_data)
{
  struct pw_properties *properties = user_data;

  if (G_VALUE_HOLDS_STRING (value))
    pw_properties_set (properties,
                       g_quark_to_string (field_id),
                       g_value_get_string (value));
  return TRUE;
}

static gboolean
gst_pw_audio_ring_buffer_acquire (GstAudioRingBuffer *buf,
    GstAudioRingBufferSpec *spec)
{
  GstPwAudioRingBuffer *self = GST_PW_AUDIO_RING_BUFFER (buf);
  struct pw_properties *props;
  struct spa_pod_builder b = { NULL };
  uint8_t buffer[512];
  const struct spa_pod *params[1];

  g_return_val_if_fail (spec, FALSE);
  g_return_val_if_fail (GST_AUDIO_INFO_IS_VALID (&spec->info), FALSE);
  g_return_val_if_fail (!self->stream, TRUE); /* already acquired */

  g_return_val_if_fail (spec->type == GST_AUDIO_RING_BUFFER_FORMAT_TYPE_RAW, FALSE);
  g_return_val_if_fail (GST_AUDIO_INFO_IS_FLOAT (&spec->info), FALSE);

  GST_DEBUG_OBJECT (self->elem, "acquire");

  /* construct param & props objects */

  if (self->props->properties) {
    props = pw_properties_new (NULL, NULL);
    gst_structure_foreach (self->props->properties, copy_properties, props);
  } else {
    props = NULL;
  }

  spa_pod_builder_init (&b, buffer, sizeof (buffer));
  params[0] = spa_pod_builder_add_object (&b,
      SPA_TYPE_OBJECT_Format,    SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType,      SPA_POD_Id (SPA_MEDIA_TYPE_audio),
      SPA_FORMAT_mediaSubtype,   SPA_POD_Id (SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_AUDIO_format,   SPA_POD_Id (SPA_AUDIO_FORMAT_F32),
      SPA_FORMAT_AUDIO_rate,     SPA_POD_Int (GST_AUDIO_INFO_RATE (&spec->info)),
      SPA_FORMAT_AUDIO_channels, SPA_POD_Int (GST_AUDIO_INFO_CHANNELS (&spec->info)));

  self->segsize = spec->segsize;
  self->bpf = GST_AUDIO_INFO_BPF (&spec->info);
  self->rate = GST_AUDIO_INFO_RATE (&spec->info);
  self->channels = GST_AUDIO_INFO_CHANNELS (&spec->info);
  self->segoffset = 0;

  /* connect stream */

  pw_thread_loop_lock (self->main_loop);

  GST_DEBUG_OBJECT (self->elem, "creating stream");

  self->stream = pw_stream_new (self->remote, self->props->client_name, props);
  pw_stream_add_listener(self->stream, &self->stream_listener, &stream_events,
      self);

  if (pw_stream_connect (self->stream,
          self->direction,
          self->props->path ? (uint32_t)atoi(self->props->path) : SPA_ID_INVALID,
          PW_STREAM_FLAG_AUTOCONNECT |
          PW_STREAM_FLAG_MAP_BUFFERS |
          PW_STREAM_FLAG_RT_PROCESS,
          params, 1) < 0)
    goto start_error;

  GST_DEBUG_OBJECT (self->elem, "waiting for stream CONFIGURE");

  if (!wait_for_stream_state (self, PW_STREAM_STATE_CONFIGURE))
    goto start_error;

  pw_thread_loop_unlock (self->main_loop);

  /* allocate the internal ringbuffer */

  spec->seglatency = spec->segtotal + 1;
  buf->size = spec->segtotal * spec->segsize;
  buf->memory = g_malloc (buf->size);

  gst_audio_format_fill_silence (buf->spec.info.finfo, buf->memory,
      buf->size);

  GST_DEBUG_OBJECT (self->elem, "acquire done");

  return TRUE;

start_error:
  {
    GST_ERROR_OBJECT (self->elem, "could not start stream");
    pw_stream_destroy (self->stream);
    self->stream = NULL;
    pw_thread_loop_unlock (self->main_loop);
    return FALSE;
  }
}

static gboolean
gst_pw_audio_ring_buffer_release (GstAudioRingBuffer *buf)
{
  GstPwAudioRingBuffer *self = GST_PW_AUDIO_RING_BUFFER (buf);

  GST_DEBUG_OBJECT (self->elem, "release");

  pw_thread_loop_lock (self->main_loop);
  if (self->stream) {
    spa_hook_remove (&self->stream_listener);
    pw_stream_disconnect (self->stream);
    pw_stream_destroy (self->stream);
    self->stream = NULL;
  }
  pw_thread_loop_unlock (self->main_loop);

  /* free the buffer */
  g_free (buf->memory);
  buf->memory = NULL;

  return TRUE;
}

static guint
gst_pw_audio_ring_buffer_delay (GstAudioRingBuffer *buf)
{
  GstPwAudioRingBuffer *self = GST_PW_AUDIO_RING_BUFFER (buf);
  struct pw_time t;

  if (!self->stream || pw_stream_get_time (self->stream, &t) < 0)
    return 0;

  if (self->direction == PW_DIRECTION_OUTPUT) {
    /* on output streams, we set the pw_buffer.size in frames,
       so no conversion is necessary */
    return t.queued;
  } else {
    /* on input streams, pw_buffer.size is set by pw_stream in ticks,
       so we need to convert it to frames and also add segoffset, which
       is the number of bytes we have read but not advertised yet, as
       the segment is incomplete */
    if (t.rate.denom > 0)
      return
        gst_util_uint64_scale (t.queued, self->rate * t.rate.num, t.rate.denom)
        + self->segoffset / self->bpf;
    else
      return self->segoffset / self->bpf;
  }

  return 0;
}

static void
gst_pw_audio_ring_buffer_class_init (GstPwAudioRingBufferClass * klass)
{
  GObjectClass *gobject_class;
  GstAudioRingBufferClass *gstaudiorbuf_class;

  gobject_class = (GObjectClass *) klass;
  gstaudiorbuf_class = (GstAudioRingBufferClass *) klass;

  gobject_class->finalize = gst_pw_audio_ring_buffer_finalize;
  gobject_class->set_property = gst_pw_audio_ring_buffer_set_property;

  gstaudiorbuf_class->open_device = gst_pw_audio_ring_buffer_open_device;
  gstaudiorbuf_class->acquire = gst_pw_audio_ring_buffer_acquire;
  gstaudiorbuf_class->release = gst_pw_audio_ring_buffer_release;
  gstaudiorbuf_class->close_device = gst_pw_audio_ring_buffer_close_device;
  gstaudiorbuf_class->delay = gst_pw_audio_ring_buffer_delay;

  g_object_class_install_property (gobject_class, PROP_ELEMENT,
      g_param_spec_object ("element", "Element", "The audio source or sink",
          GST_TYPE_ELEMENT,
          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_DIRECTION,
      g_param_spec_int ("direction", "Direction", "The stream direction",
          PW_DIRECTION_INPUT, PW_DIRECTION_OUTPUT, PW_DIRECTION_INPUT,
          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class, PROP_PROPS,
      g_param_spec_pointer ("props", "Properties", "The properties struct",
          G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  GST_DEBUG_CATEGORY_INIT (pw_audio_ring_buffer_debug, "pwaudioringbuffer", 0,
      "PipeWire Audio Ring Buffer");
}
