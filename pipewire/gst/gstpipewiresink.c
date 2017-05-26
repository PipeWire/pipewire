/* GStreamer
 * Copyright (C) <2015> Wim Taymans <wim.taymans@gmail.com>
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

/**
 * SECTION:element-pipewiresink
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v videotestsrc ! pipewiresink
 * ]| Sends a test video source to PipeWire
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "gstpipewiresink.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gunixfdmessage.h>
#include <gst/allocators/gstfdmemory.h>
#include <gst/video/video.h>

#include <spa/buffer.h>

#include "gstpipewireformat.h"

static GQuark process_mem_data_quark;

GST_DEBUG_CATEGORY_STATIC (pipewire_sink_debug);
#define GST_CAT_DEFAULT pipewire_sink_debug

#define DEFAULT_PROP_MODE GST_PIPEWIRE_SINK_MODE_DEFAULT

enum
{
  PROP_0,
  PROP_PATH,
  PROP_CLIENT_NAME,
  PROP_STREAM_PROPERTIES,
  PROP_MODE
};

GType
gst_pipewire_sink_mode_get_type (void)
{
  static volatile gsize mode_type = 0;
  static const GEnumValue mode[] = {
    {GST_PIPEWIRE_SINK_MODE_DEFAULT, "GST_PIPEWIRE_SINK_MODE_DEFAULT", "default"},
    {GST_PIPEWIRE_SINK_MODE_RENDER, "GST_PIPEWIRE_SINK_MODE_RENDER", "render"},
    {GST_PIPEWIRE_SINK_MODE_PROVIDE, "GST_PIPEWIRE_SINK_MODE_PROVIDE", "provide"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&mode_type)) {
    GType tmp =
        g_enum_register_static ("GstPipeWireSinkMode", mode);
    g_once_init_leave (&mode_type, tmp);
  }

  return (GType) mode_type;
}


static GstStaticPadTemplate gst_pipewire_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );

#define gst_pipewire_sink_parent_class parent_class
G_DEFINE_TYPE (GstPipeWireSink, gst_pipewire_sink, GST_TYPE_BASE_SINK);

static void gst_pipewire_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_pipewire_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstStateChangeReturn
gst_pipewire_sink_change_state (GstElement * element, GstStateChange transition);

static gboolean gst_pipewire_sink_setcaps (GstBaseSink * bsink, GstCaps * caps);
static GstCaps *gst_pipewire_sink_sink_fixate (GstBaseSink * bsink,
    GstCaps * caps);

static GstFlowReturn gst_pipewire_sink_render (GstBaseSink * psink,
    GstBuffer * buffer);
static gboolean gst_pipewire_sink_start (GstBaseSink * basesink);
static gboolean gst_pipewire_sink_stop (GstBaseSink * basesink);

static void
gst_pipewire_sink_finalize (GObject * object)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (object);

  g_object_unref (pwsink->pool);

  pw_thread_main_loop_destroy (pwsink->main_loop);
  pwsink->main_loop = NULL;

  pw_loop_destroy (pwsink->loop);
  pwsink->loop = NULL;

  if (pwsink->properties)
    gst_structure_free (pwsink->properties);
  g_object_unref (pwsink->allocator);
  g_free (pwsink->path);
  g_free (pwsink->client_name);
  g_hash_table_unref (pwsink->buf_ids);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_pipewire_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (bsink);

  gst_query_add_allocation_pool (query, GST_BUFFER_POOL_CAST (pwsink->pool), 0, 0, 0);
  return TRUE;
}

static void
gst_pipewire_sink_class_init (GstPipeWireSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;

  gobject_class->finalize = gst_pipewire_sink_finalize;
  gobject_class->set_property = gst_pipewire_sink_set_property;
  gobject_class->get_property = gst_pipewire_sink_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "Path",
                                                        "The sink path to connect to (NULL = default)",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_CLIENT_NAME,
                                   g_param_spec_string ("client-name",
                                                        "Client Name",
                                                        "The client name to use (NULL = default)",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

   g_object_class_install_property (gobject_class,
                                    PROP_STREAM_PROPERTIES,
                                    g_param_spec_boxed ("stream-properties",
                                                        "Stream properties",
                                                        "List of PipeWire stream properties",
                                                        GST_TYPE_STRUCTURE,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

   g_object_class_install_property (gobject_class,
                                    PROP_MODE,
                                    g_param_spec_enum ("mode",
                                                       "Mode",
                                                       "The mode to operate in",
                                                        GST_TYPE_PIPEWIRE_SINK_MODE,
                                                        DEFAULT_PROP_MODE,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

  gstelement_class->change_state = gst_pipewire_sink_change_state;

  gst_element_class_set_static_metadata (gstelement_class,
      "PipeWire sink", "Sink/Video",
      "Send video to PipeWire", "Wim Taymans <wim.taymans@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pipewire_sink_template));

  gstbasesink_class->set_caps = gst_pipewire_sink_setcaps;
  gstbasesink_class->fixate = gst_pipewire_sink_sink_fixate;
  gstbasesink_class->propose_allocation = gst_pipewire_sink_propose_allocation;
  gstbasesink_class->start = gst_pipewire_sink_start;
  gstbasesink_class->stop = gst_pipewire_sink_stop;
  gstbasesink_class->render = gst_pipewire_sink_render;

  GST_DEBUG_CATEGORY_INIT (pipewire_sink_debug, "pipewiresink", 0,
      "PipeWire Sink");

  process_mem_data_quark = g_quark_from_static_string ("GstPipeWireSinkProcessMemQuark");
}


#define PROP(f,key,type,...)                                                    \
          SPA_POD_PROP (f,key,0,type,1,__VA_ARGS__)
#define PROP_R(f,key,type,...)                                                  \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_READONLY,type,1,__VA_ARGS__)
#define PROP_MM(f,key,type,...)                                                 \
          SPA_POD_PROP (f,key,SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_U_MM(f,key,type,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_MIN_MAX,type,3,__VA_ARGS__)
#define PROP_EN(f,key,type,n,...)                                               \
          SPA_POD_PROP (f,key,SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)
#define PROP_U_EN(f,key,type,n,...)                                             \
          SPA_POD_PROP (f,key,SPA_POD_PROP_FLAG_UNSET |                         \
                              SPA_POD_PROP_RANGE_ENUM,type,n,__VA_ARGS__)
static void
pool_activated (GstPipeWirePool *pool, GstPipeWireSink *sink)
{
  struct pw_context *ctx = sink->stream->context;
  GstStructure *config;
  GstCaps *caps;
  guint size;
  guint min_buffers;
  guint max_buffers;
  struct spa_param *port_params[3];
  struct spa_pod_builder b = { NULL };
  uint8_t buffer[1024];
  struct spa_pod_frame f[2];

  config = gst_buffer_pool_get_config (GST_BUFFER_POOL (pool));
  gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers, &max_buffers);

  spa_pod_builder_init (&b, buffer, sizeof (buffer));
  spa_pod_builder_push_object (&b, &f[0], 0, ctx->type.param_alloc_buffers.Buffers);
  if (size == 0)
    spa_pod_builder_add (&b,
        PROP_U_MM (&f[1], ctx->type.param_alloc_buffers.size, SPA_POD_TYPE_INT, 0, 0, INT32_MAX), 0);
  else
    spa_pod_builder_add (&b,
        PROP_MM (&f[1], ctx->type.param_alloc_buffers.size, SPA_POD_TYPE_INT, size, size, INT32_MAX), 0);

  spa_pod_builder_add (&b,
      PROP_MM (&f[1], ctx->type.param_alloc_buffers.stride,  SPA_POD_TYPE_INT, 0, 0, INT32_MAX),
      PROP_U_MM (&f[1], ctx->type.param_alloc_buffers.buffers, SPA_POD_TYPE_INT, min_buffers, min_buffers, max_buffers ? max_buffers : INT32_MAX),
      PROP    (&f[1], ctx->type.param_alloc_buffers.align,   SPA_POD_TYPE_INT, 16),
      0);
  spa_pod_builder_pop (&b, &f[0]);
  port_params[0] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, struct spa_param);

  spa_pod_builder_object (&b, &f[0], 0, ctx->type.param_alloc_meta_enable.MetaEnable,
      PROP    (&f[1], ctx->type.param_alloc_meta_enable.type, SPA_POD_TYPE_ID, ctx->type.meta.Header),
      PROP    (&f[1], ctx->type.param_alloc_meta_enable.size, SPA_POD_TYPE_INT, sizeof (struct spa_meta_header)));
  port_params[1] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, struct spa_param);

  spa_pod_builder_object (&b, &f[0], 0, ctx->type.param_alloc_meta_enable.MetaEnable,
      PROP    (&f[1], ctx->type.param_alloc_meta_enable.type, SPA_POD_TYPE_ID, ctx->type.meta.Ringbuffer),
      PROP    (&f[1], ctx->type.param_alloc_meta_enable.size, SPA_POD_TYPE_INT, sizeof (struct spa_meta_ringbuffer)),
      PROP    (&f[1], ctx->type.param_alloc_meta_enable.ringbufferSize,   SPA_POD_TYPE_INT,
                                                                         size * SPA_MAX (4,
                                                                                SPA_MAX (min_buffers, max_buffers))),
      PROP    (&f[1], ctx->type.param_alloc_meta_enable.ringbufferStride, SPA_POD_TYPE_INT, 0),
      PROP    (&f[1], ctx->type.param_alloc_meta_enable.ringbufferBlocks, SPA_POD_TYPE_INT, 1),
      PROP    (&f[1], ctx->type.param_alloc_meta_enable.ringbufferAlign,  SPA_POD_TYPE_INT, 16));
  port_params[2] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, struct spa_param);

  pw_thread_main_loop_lock (sink->main_loop);
  pw_stream_finish_format (sink->stream, SPA_RESULT_OK, port_params, 2);
  pw_thread_main_loop_unlock (sink->main_loop);
}

static void
gst_pipewire_sink_init (GstPipeWireSink * sink)
{
  sink->allocator = gst_fd_allocator_new ();
  sink->pool =  gst_pipewire_pool_new ();
  sink->client_name = pw_client_name();
  sink->mode = DEFAULT_PROP_MODE;

  g_signal_connect (sink->pool, "activated", G_CALLBACK (pool_activated), sink);

  sink->buf_ids = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
      (GDestroyNotify) gst_buffer_unref);

  g_queue_init (&sink->queue);

  sink->loop = pw_loop_new ();
  sink->main_loop = pw_thread_main_loop_new (sink->loop, "pipewire-sink-loop");
  GST_DEBUG ("loop %p %p", sink->loop, sink->main_loop);
}

static GstCaps *
gst_pipewire_sink_sink_fixate (GstBaseSink * bsink, GstCaps * caps)
{
  GstStructure *structure;

  caps = gst_caps_make_writable (caps);

  structure = gst_caps_get_structure (caps, 0);

  if (gst_structure_has_name (structure, "video/x-raw")) {
    gst_structure_fixate_field_nearest_int (structure, "width", 320);
    gst_structure_fixate_field_nearest_int (structure, "height", 240);
    gst_structure_fixate_field_nearest_fraction (structure, "framerate", 30, 1);

    if (gst_structure_has_field (structure, "pixel-aspect-ratio"))
      gst_structure_fixate_field_nearest_fraction (structure,
          "pixel-aspect-ratio", 1, 1);
    else
      gst_structure_set (structure, "pixel-aspect-ratio", GST_TYPE_FRACTION, 1, 1,
          NULL);

    if (gst_structure_has_field (structure, "colorimetry"))
      gst_structure_fixate_field_string (structure, "colorimetry", "bt601");
    if (gst_structure_has_field (structure, "chroma-site"))
      gst_structure_fixate_field_string (structure, "chroma-site", "mpeg2");

    if (gst_structure_has_field (structure, "interlace-mode"))
      gst_structure_fixate_field_string (structure, "interlace-mode",
          "progressive");
    else
      gst_structure_set (structure, "interlace-mode", G_TYPE_STRING,
          "progressive", NULL);
  } else if (gst_structure_has_name (structure, "audio/x-raw")) {
    gst_structure_fixate_field_string (structure, "format", "S16LE");
    gst_structure_fixate_field_nearest_int (structure, "channels", 2);
    gst_structure_fixate_field_nearest_int (structure, "rate", 44100);
  }

  caps = GST_BASE_SINK_CLASS (parent_class)->fixate (bsink, caps);

  return caps;
}

static void
gst_pipewire_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (object);

  switch (prop_id) {
    case PROP_PATH:
      g_free (pwsink->path);
      pwsink->path = g_value_dup_string (value);
      break;

    case PROP_CLIENT_NAME:
      g_free (pwsink->client_name);
      pwsink->client_name = g_value_dup_string (value);
      break;

    case PROP_STREAM_PROPERTIES:
      if (pwsink->properties)
        gst_structure_free (pwsink->properties);
      pwsink->properties =
          gst_structure_copy (gst_value_get_structure (value));
      break;

    case PROP_MODE:
      pwsink->mode = g_value_get_enum (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pipewire_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (object);

  switch (prop_id) {
    case PROP_PATH:
      g_value_set_string (value, pwsink->path);
      break;

    case PROP_CLIENT_NAME:
      g_value_set_string (value, pwsink->client_name);
      break;

    case PROP_STREAM_PROPERTIES:
      gst_value_set_structure (value, pwsink->properties);
      break;

    case PROP_MODE:
      g_value_set_enum (value, pwsink->mode);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

typedef struct {
  GstPipeWireSink *sink;
  guint id;
  struct spa_buffer *buf;
  struct spa_meta_header *header;
  guint flags;
  goffset offset;
} ProcessMemData;

static void
process_mem_data_destroy (gpointer user_data)
{
  ProcessMemData *data = user_data;

  gst_object_unref (data->sink);
  g_slice_free (ProcessMemData, data);
}

static void
on_add_buffer (struct pw_listener *listener,
               struct pw_stream   *stream,
               uint32_t       id)
{
  GstPipeWireSink *pwsink = SPA_CONTAINER_OF (listener, GstPipeWireSink, stream_add_buffer);
  struct spa_buffer *b;
  GstBuffer *buf;
  uint32_t i;
  ProcessMemData data;

  GST_LOG_OBJECT (pwsink, "add buffer");

  if (!(b = pw_stream_peek_buffer (pwsink->stream, id))) {
    g_warning ("failed to peek buffer");
    return;
  }

  buf = gst_buffer_new ();

  data.sink = gst_object_ref (pwsink);
  data.id = id;
  data.buf = b;
  data.header = spa_buffer_find_meta (b, stream->context->type.meta.Header);

  for (i = 0; i < b->n_datas; i++) {
    struct spa_data *d = &b->datas[i];
    GstMemory *gmem = NULL;

    if (d->type == stream->context->type.data.MemFd ||
        d->type == stream->context->type.data.DmaBuf) {
      gmem = gst_fd_allocator_alloc (pwsink->allocator, dup (d->fd),
                d->mapoffset + d->maxsize, GST_FD_MEMORY_FLAG_NONE);
      gst_memory_resize (gmem, d->chunk->offset + d->mapoffset, d->chunk->size);
      data.offset = d->mapoffset;
    }
    else if (d->type == stream->context->type.data.MemPtr) {
      gmem = gst_memory_new_wrapped (0, d->data, d->maxsize, d->chunk->offset,
                                     d->chunk->size, NULL, NULL);
      data.offset = 0;
    }
    if (gmem)
      gst_buffer_append_memory (buf, gmem);
  }
  data.flags = GST_BUFFER_FLAGS (buf);
  gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (buf),
                             process_mem_data_quark,
                             g_slice_dup (ProcessMemData, &data),
                             process_mem_data_destroy);

  gst_pipewire_pool_add_buffer (pwsink->pool, buf);
  g_hash_table_insert (pwsink->buf_ids, GINT_TO_POINTER (id), buf);

  pw_thread_main_loop_signal (pwsink->main_loop, FALSE);
}

static void
on_remove_buffer (struct pw_listener *listener,
                  struct pw_stream   *stream,
                  uint32_t       id)
{
  GstPipeWireSink *pwsink = SPA_CONTAINER_OF (listener, GstPipeWireSink, stream_remove_buffer);
  GstBuffer *buf;

  GST_LOG_OBJECT (pwsink, "remove buffer");
  buf = g_hash_table_lookup (pwsink->buf_ids, GINT_TO_POINTER (id));
  if (buf) {
    GST_MINI_OBJECT_CAST (buf)->dispose = NULL;
    if (!gst_pipewire_pool_remove_buffer (pwsink->pool, buf))
      gst_buffer_ref (buf);
    if (g_queue_remove (&pwsink->queue, buf))
      gst_buffer_unref (buf);
    g_hash_table_remove (pwsink->buf_ids, GINT_TO_POINTER (id));
  }
}

static void
on_new_buffer (struct pw_listener *listener,
               struct pw_stream   *stream,
               uint32_t       id)
{
  GstPipeWireSink *pwsink = SPA_CONTAINER_OF (listener, GstPipeWireSink, stream_new_buffer);
  GstBuffer *buf;

  GST_LOG_OBJECT (pwsink, "got new buffer %u", id);
  if (pwsink->stream == NULL) {
    GST_LOG_OBJECT (pwsink, "no stream");
    return;
  }
  buf = g_hash_table_lookup (pwsink->buf_ids, GINT_TO_POINTER (id));

  if (buf) {
    pw_thread_main_loop_signal (pwsink->main_loop, FALSE);
  }
}

static void
do_send_buffer (GstPipeWireSink *pwsink)
{
  GstBuffer *buffer;
  ProcessMemData *data;
  gboolean res;
  guint i;

  buffer = g_queue_pop_head (&pwsink->queue);
  if (buffer == NULL) {
    GST_WARNING ("out of buffers");
    return;
  }

  data = gst_mini_object_get_qdata (GST_MINI_OBJECT_CAST (buffer),
                                    process_mem_data_quark);

  if (data->header) {
    data->header->seq = GST_BUFFER_OFFSET (buffer);
    data->header->pts = GST_BUFFER_PTS (buffer);
    data->header->dts_offset = GST_BUFFER_DTS (buffer);
  }
  for (i = 0; i < data->buf->n_datas; i++) {
    struct spa_data *d = &data->buf->datas[i];
    GstMemory *mem = gst_buffer_peek_memory (buffer, i);
    d->chunk->offset = mem->offset - data->offset;
    d->chunk->size = mem->size;
  }

  if (!(res = pw_stream_send_buffer (pwsink->stream, data->id))) {
    g_warning ("can't send buffer");
    pw_thread_main_loop_signal (pwsink->main_loop, FALSE);
  } else
    pwsink->need_ready--;

  gst_buffer_unref (buffer);
}


static void
on_need_buffer (struct pw_listener *listener,
                struct pw_stream   *stream)
{
  GstPipeWireSink *pwsink = SPA_CONTAINER_OF (listener, GstPipeWireSink, stream_need_buffer);

  pwsink->need_ready++;
  GST_DEBUG ("need buffer %u", pwsink->need_ready);
  do_send_buffer (pwsink);
}

static void
on_state_changed (struct pw_listener *listener,
                  struct pw_stream   *stream)
{
  GstPipeWireSink *pwsink = SPA_CONTAINER_OF (listener, GstPipeWireSink, stream_state_changed);
  enum pw_stream_state state;

  state = stream->state;
  GST_DEBUG ("got stream state %d", state);

  switch (state) {
    case PW_STREAM_STATE_UNCONNECTED:
    case PW_STREAM_STATE_CONNECTING:
    case PW_STREAM_STATE_CONFIGURE:
    case PW_STREAM_STATE_READY:
    case PW_STREAM_STATE_PAUSED:
    case PW_STREAM_STATE_STREAMING:
      break;
    case PW_STREAM_STATE_ERROR:
      GST_ELEMENT_ERROR (pwsink, RESOURCE, FAILED,
          ("stream error: %s", stream->error), (NULL));
      break;
  }
  pw_thread_main_loop_signal (pwsink->main_loop, FALSE);
}

static void
on_format_changed (struct pw_listener *listener,
                   struct pw_stream   *stream,
                   struct spa_format     *format)
{
  GstPipeWireSink *pwsink = SPA_CONTAINER_OF (listener, GstPipeWireSink, stream_format_changed);

  if (gst_buffer_pool_is_active (GST_BUFFER_POOL_CAST (pwsink->pool)))
    pool_activated (pwsink->pool, pwsink);
}

static gboolean
gst_pipewire_sink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstPipeWireSink *pwsink;
  GPtrArray *possible;
  enum pw_stream_state state;
  gboolean res = FALSE;

  pwsink = GST_PIPEWIRE_SINK (bsink);

  possible = gst_caps_to_format_all (caps);

  pw_thread_main_loop_lock (pwsink->main_loop);
  state = pwsink->stream->state;

  if (state == PW_STREAM_STATE_ERROR)
    goto start_error;

  if (state == PW_STREAM_STATE_UNCONNECTED) {
    enum pw_stream_flags flags = 0;

    if (pwsink->mode != GST_PIPEWIRE_SINK_MODE_PROVIDE)
      flags |= PW_STREAM_FLAG_AUTOCONNECT;

    pw_stream_connect (pwsink->stream,
                          PW_DIRECTION_OUTPUT,
                          PW_STREAM_MODE_BUFFER,
                          pwsink->path,
                          flags,
                          possible->len,
                          (struct spa_format **) possible->pdata);

    while (TRUE) {
      state = pwsink->stream->state;

      if (state == PW_STREAM_STATE_CONFIGURE)
        break;

      if (state == PW_STREAM_STATE_ERROR)
        goto start_error;

      pw_thread_main_loop_wait (pwsink->main_loop);
    }
  }
  res = TRUE;

  pw_thread_main_loop_unlock (pwsink->main_loop);

  pwsink->negotiated = res;

  return res;

start_error:
  {
    GST_ERROR ("could not start stream");
    pw_thread_main_loop_unlock (pwsink->main_loop);
    g_ptr_array_unref (possible);
    return FALSE;
  }
}

static GstFlowReturn
gst_pipewire_sink_render (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstPipeWireSink *pwsink;
  GstFlowReturn res = GST_FLOW_OK;

  pwsink = GST_PIPEWIRE_SINK (bsink);

  if (!pwsink->negotiated)
    goto not_negotiated;

  pw_thread_main_loop_lock (pwsink->main_loop);
  if (pwsink->stream->state != PW_STREAM_STATE_STREAMING)
    goto done;

  if (buffer->pool != GST_BUFFER_POOL_CAST (pwsink->pool)) {
    GstBuffer *b = NULL;
    GstMapInfo info = { 0, };

    if (!gst_buffer_pool_is_active (GST_BUFFER_POOL_CAST (pwsink->pool)))
      gst_buffer_pool_set_active (GST_BUFFER_POOL_CAST (pwsink->pool), TRUE);

    if ((res = gst_buffer_pool_acquire_buffer (GST_BUFFER_POOL_CAST (pwsink->pool), &b, NULL)) != GST_FLOW_OK)
      goto done;

    gst_buffer_map (b, &info, GST_MAP_WRITE);
    gst_buffer_extract (buffer, 0, info.data, info.size);
    gst_buffer_unmap (b, &info);
    gst_buffer_resize (b, 0, gst_buffer_get_size (buffer));
    buffer = b;
  } else
    gst_buffer_ref (buffer);

  GST_DEBUG ("push buffer in queue");
  g_queue_push_tail (&pwsink->queue, buffer);

  if (pwsink->need_ready && pwsink->mode == GST_PIPEWIRE_SINK_MODE_PROVIDE)
    do_send_buffer (pwsink);

done:
  pw_thread_main_loop_unlock (pwsink->main_loop);

  return res;

not_negotiated:
  {
    return GST_FLOW_NOT_NEGOTIATED;
  }
}

static gboolean
copy_properties (GQuark field_id,
                 const GValue *value,
                 gpointer user_data)
{
  struct pw_properties *properties = user_data;

  if (G_VALUE_HOLDS_STRING (value))
    pw_properties_set (properties,
                       g_quark_to_string (field_id),
                       g_value_get_string (value));
  return TRUE;
}

static gboolean
gst_pipewire_sink_start (GstBaseSink * basesink)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (basesink);
  struct pw_properties *props;

  pwsink->negotiated = FALSE;

  if (pwsink->properties) {
    props = pw_properties_new (NULL, NULL);
    gst_structure_foreach (pwsink->properties, copy_properties, props);
  } else {
    props = NULL;
  }

  pw_thread_main_loop_lock (pwsink->main_loop);
  pwsink->stream = pw_stream_new (pwsink->ctx, pwsink->client_name, props);
  pwsink->pool->stream = pwsink->stream;

  pw_signal_add (&pwsink->stream->state_changed, &pwsink->stream_state_changed, on_state_changed);
  pw_signal_add (&pwsink->stream->format_changed, &pwsink->stream_format_changed, on_format_changed);
  pw_signal_add (&pwsink->stream->add_buffer, &pwsink->stream_add_buffer, on_add_buffer);
  pw_signal_add (&pwsink->stream->remove_buffer, &pwsink->stream_remove_buffer, on_remove_buffer);
  pw_signal_add (&pwsink->stream->new_buffer, &pwsink->stream_new_buffer, on_new_buffer);
  pw_signal_add (&pwsink->stream->need_buffer, &pwsink->stream_need_buffer, on_need_buffer);
  pw_thread_main_loop_unlock (pwsink->main_loop);

  return TRUE;
}

static gboolean
gst_pipewire_sink_stop (GstBaseSink * basesink)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (basesink);

  pw_thread_main_loop_lock (pwsink->main_loop);
  if (pwsink->stream) {
    pw_stream_disconnect (pwsink->stream);
    pw_stream_destroy (pwsink->stream);
    pwsink->stream = NULL;
    pwsink->pool->stream = NULL;
  }
  pw_thread_main_loop_unlock (pwsink->main_loop);

  pwsink->negotiated = FALSE;

  return TRUE;
}

static void
on_ctx_state_changed (struct pw_listener *listener,
                      struct pw_context  *ctx)
{
  GstPipeWireSink *pwsink = SPA_CONTAINER_OF (listener, GstPipeWireSink, ctx_state_changed);
  enum pw_context_state state;

  state = ctx->state;
  GST_DEBUG ("got context state %d", state);

  switch (state) {
    case PW_CONTEXT_STATE_UNCONNECTED:
    case PW_CONTEXT_STATE_CONNECTING:
    case PW_CONTEXT_STATE_CONNECTED:
      break;
    case PW_CONTEXT_STATE_ERROR:
      GST_ELEMENT_ERROR (pwsink, RESOURCE, FAILED,
          ("context error: %s", ctx->error), (NULL));
      break;
  }
  pw_thread_main_loop_signal (pwsink->main_loop, FALSE);
}

static gboolean
gst_pipewire_sink_open (GstPipeWireSink * pwsink)
{
  if (pw_thread_main_loop_start (pwsink->main_loop) != SPA_RESULT_OK)
    goto mainloop_error;

  pw_thread_main_loop_lock (pwsink->main_loop);
  pwsink->ctx = pw_context_new (pwsink->loop, g_get_application_name (), NULL);

  pw_signal_add (&pwsink->ctx->state_changed, &pwsink->ctx_state_changed, on_ctx_state_changed);

  pw_context_connect (pwsink->ctx, PW_CONTEXT_FLAG_NO_REGISTRY);

  while (TRUE) {
    enum pw_context_state state = pwsink->ctx->state;

    if (state == PW_CONTEXT_STATE_CONNECTED)
      break;

    if (state == PW_CONTEXT_STATE_ERROR)
      goto connect_error;

    pw_thread_main_loop_wait (pwsink->main_loop);
  }
  pw_thread_main_loop_unlock (pwsink->main_loop);

  return TRUE;

  /* ERRORS */
mainloop_error:
  {
    GST_ELEMENT_ERROR (pwsink, RESOURCE, FAILED,
        ("Failed to start mainloop"), (NULL));
    return FALSE;
  }
connect_error:
  {
    pw_thread_main_loop_unlock (pwsink->main_loop);
    return FALSE;
  }
}

static gboolean
gst_pipewire_sink_close (GstPipeWireSink * pwsink)
{
  pw_thread_main_loop_lock (pwsink->main_loop);
  if (pwsink->stream) {
    pw_stream_disconnect (pwsink->stream);
  }
  if (pwsink->ctx) {
    pw_context_disconnect (pwsink->ctx);

    while (TRUE) {
      enum pw_context_state state = pwsink->ctx->state;

      if (state == PW_CONTEXT_STATE_UNCONNECTED)
        break;

      if (state == PW_CONTEXT_STATE_ERROR)
        break;

      pw_thread_main_loop_wait (pwsink->main_loop);
    }
  }
  pw_thread_main_loop_unlock (pwsink->main_loop);

  pw_thread_main_loop_stop (pwsink->main_loop);

  if (pwsink->stream) {
    pw_stream_destroy (pwsink->stream);
    pwsink->stream = NULL;
  }

  if (pwsink->ctx) {
    pw_context_destroy (pwsink->ctx);
    pwsink->ctx = NULL;
  }

  return TRUE;
}

static GstStateChangeReturn
gst_pipewire_sink_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstPipeWireSink *this = GST_PIPEWIRE_SINK_CAST (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_pipewire_sink_open (this))
        goto open_failed;
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      /* uncork and start recording */
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      /* stop recording ASAP by corking */
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      gst_pipewire_sink_close (this);
      break;
    default:
      break;
  }
  return ret;

  /* ERRORS */
open_failed:
  {
    return GST_STATE_CHANGE_FAILURE;
  }
}
