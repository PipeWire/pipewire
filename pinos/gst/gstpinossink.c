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
 * SECTION:element-pinossink
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v videotestsrc ! pinossink
 * ]| Sends a test video source to pinos
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "gstpinossink.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gunixfdmessage.h>
#include <gst/allocators/gstfdmemory.h>
#include <gst/video/video.h>

#include <spa/include/spa/buffer.h>

#include "gstpinosformat.h"

static GQuark process_mem_data_quark;

GST_DEBUG_CATEGORY_STATIC (pinos_sink_debug);
#define GST_CAT_DEFAULT pinos_sink_debug

#define DEFAULT_PROP_MODE GST_PINOS_SINK_MODE_DEFAULT

enum
{
  PROP_0,
  PROP_PATH,
  PROP_CLIENT_NAME,
  PROP_STREAM_PROPERTIES,
  PROP_MODE
};

GType
gst_pinos_sink_mode_get_type (void)
{
  static volatile gsize mode_type = 0;
  static const GEnumValue mode[] = {
    {GST_PINOS_SINK_MODE_DEFAULT, "GST_PINOS_SINK_MODE_DEFAULT", "default"},
    {GST_PINOS_SINK_MODE_RENDER, "GST_PINOS_SINK_MODE_RENDER", "render"},
    {GST_PINOS_SINK_MODE_PROVIDE, "GST_PINOS_SINK_MODE_PROVIDE", "provide"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&mode_type)) {
    GType tmp =
        g_enum_register_static ("GstPinosSinkMode", mode);
    g_once_init_leave (&mode_type, tmp);
  }

  return (GType) mode_type;
}


#define PINOSS_VIDEO_CAPS GST_VIDEO_CAPS_MAKE (GST_VIDEO_FORMATS_ALL)

static GstStaticPadTemplate gst_pinos_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );

#define gst_pinos_sink_parent_class parent_class
G_DEFINE_TYPE (GstPinosSink, gst_pinos_sink, GST_TYPE_BASE_SINK);

static void gst_pinos_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_pinos_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstStateChangeReturn
gst_pinos_sink_change_state (GstElement * element, GstStateChange transition);

static gboolean gst_pinos_sink_setcaps (GstBaseSink * bsink, GstCaps * caps);
static GstCaps *gst_pinos_sink_sink_fixate (GstBaseSink * bsink,
    GstCaps * caps);

static GstFlowReturn gst_pinos_sink_render (GstBaseSink * psink,
    GstBuffer * buffer);
static gboolean gst_pinos_sink_start (GstBaseSink * basesink);
static gboolean gst_pinos_sink_stop (GstBaseSink * basesink);

static void
gst_pinos_sink_finalize (GObject * object)
{
  GstPinosSink *pinossink = GST_PINOS_SINK (object);

  g_object_unref (pinossink->pool);

  pinos_thread_main_loop_destroy (pinossink->main_loop);
  pinossink->main_loop = NULL;

  pinos_loop_destroy (pinossink->loop);
  pinossink->loop = NULL;

  if (pinossink->properties)
    gst_structure_free (pinossink->properties);
  g_object_unref (pinossink->allocator);
  g_free (pinossink->path);
  g_free (pinossink->client_name);
  g_hash_table_unref (pinossink->buf_ids);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_pinos_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstPinosSink *pinossink = GST_PINOS_SINK (bsink);

  gst_query_add_allocation_pool (query, GST_BUFFER_POOL_CAST (pinossink->pool), 0, 0, 0);
  return TRUE;
}

static void
gst_pinos_sink_class_init (GstPinosSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;

  gobject_class->finalize = gst_pinos_sink_finalize;
  gobject_class->set_property = gst_pinos_sink_set_property;
  gobject_class->get_property = gst_pinos_sink_get_property;

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
                                                        "List of pinos stream properties",
                                                        GST_TYPE_STRUCTURE,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

   g_object_class_install_property (gobject_class,
                                    PROP_MODE,
                                    g_param_spec_enum ("mode",
                                                       "Mode",
                                                       "The mode to operate in",
                                                        GST_TYPE_PINOS_SINK_MODE,
                                                        DEFAULT_PROP_MODE,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

  gstelement_class->change_state = gst_pinos_sink_change_state;

  gst_element_class_set_static_metadata (gstelement_class,
      "Pinos sink", "Sink/Video",
      "Send video to Pinos", "Wim Taymans <wim.taymans@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pinos_sink_template));

  gstbasesink_class->set_caps = gst_pinos_sink_setcaps;
  gstbasesink_class->fixate = gst_pinos_sink_sink_fixate;
  gstbasesink_class->propose_allocation = gst_pinos_sink_propose_allocation;
  gstbasesink_class->start = gst_pinos_sink_start;
  gstbasesink_class->stop = gst_pinos_sink_stop;
  gstbasesink_class->render = gst_pinos_sink_render;

  GST_DEBUG_CATEGORY_INIT (pinos_sink_debug, "pinossink", 0,
      "Pinos Sink");

  process_mem_data_quark = g_quark_from_static_string ("GstPinosSinkProcessMemQuark");
}

static void
gst_pinos_sink_init (GstPinosSink * sink)
{
  sink->allocator = gst_fd_allocator_new ();
  sink->pool =  gst_pinos_pool_new ();
  sink->client_name = pinos_client_name();
  sink->mode = DEFAULT_PROP_MODE;

  sink->buf_ids = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
      (GDestroyNotify) gst_buffer_unref);

  g_queue_init (&sink->queue);

  sink->loop = pinos_loop_new ();
  sink->main_loop = pinos_thread_main_loop_new (sink->loop, "pinos-sink-loop");
  GST_DEBUG ("loop %p %p", sink->loop, sink->main_loop);
}

static GstCaps *
gst_pinos_sink_sink_fixate (GstBaseSink * bsink, GstCaps * caps)
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
gst_pinos_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPinosSink *pinossink = GST_PINOS_SINK (object);

  switch (prop_id) {
    case PROP_PATH:
      g_free (pinossink->path);
      pinossink->path = g_value_dup_string (value);
      break;

    case PROP_CLIENT_NAME:
      g_free (pinossink->client_name);
      pinossink->client_name = g_value_dup_string (value);
      break;

    case PROP_STREAM_PROPERTIES:
      if (pinossink->properties)
        gst_structure_free (pinossink->properties);
      pinossink->properties =
          gst_structure_copy (gst_value_get_structure (value));
      break;

    case PROP_MODE:
      pinossink->mode = g_value_get_enum (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pinos_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPinosSink *pinossink = GST_PINOS_SINK (object);

  switch (prop_id) {
    case PROP_PATH:
      g_value_set_string (value, pinossink->path);
      break;

    case PROP_CLIENT_NAME:
      g_value_set_string (value, pinossink->client_name);
      break;

    case PROP_STREAM_PROPERTIES:
      gst_value_set_structure (value, pinossink->properties);
      break;

    case PROP_MODE:
      g_value_set_enum (value, pinossink->mode);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

typedef struct {
  GstPinosSink *sink;
  guint id;
  SpaBuffer *buf;
  SpaMetaHeader *header;
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
on_add_buffer (PinosListener *listener,
               PinosStream   *stream,
               uint32_t       id)
{
  GstPinosSink *pinossink = SPA_CONTAINER_OF (listener, GstPinosSink, stream_add_buffer);
  SpaBuffer *b;
  GstBuffer *buf;
  uint32_t i;
  ProcessMemData data;

  GST_LOG_OBJECT (pinossink, "add buffer");

  if (!(b = pinos_stream_peek_buffer (pinossink->stream, id))) {
    g_warning ("failed to peek buffer");
    return;
  }

  buf = gst_buffer_new ();

  data.sink = gst_object_ref (pinossink);
  data.id = id;
  data.buf = b;
  data.header = NULL;

  for (i = 0; i < b->n_metas; i++) {
    SpaMeta *m = &b->metas[i];

    switch (m->type) {
      case SPA_META_TYPE_HEADER:
        data.header = m->data;
        break;
      default:
        break;
    }
  }
  for (i = 0; i < b->n_datas; i++) {
    SpaData *d = &b->datas[i];
    GstMemory *gmem = NULL;

    switch (d->type) {
      case SPA_DATA_TYPE_MEMFD:
      case SPA_DATA_TYPE_DMABUF:
      {
        gmem = gst_fd_allocator_alloc (pinossink->allocator, dup (d->fd),
                  d->mapoffset + d->maxsize, GST_FD_MEMORY_FLAG_NONE);
        gst_memory_resize (gmem, d->chunk->offset + d->mapoffset, d->chunk->size);
        data.offset = d->mapoffset;
        break;
      }
      case SPA_DATA_TYPE_MEMPTR:
        gmem = gst_memory_new_wrapped (0, d->data, d->maxsize, d->chunk->offset,
                                       d->chunk->size, NULL, NULL);
        data.offset = 0;
        break;
      default:
        break;
    }
    if (gmem)
      gst_buffer_append_memory (buf, gmem);
  }
  data.flags = GST_BUFFER_FLAGS (buf);
  gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (buf),
                             process_mem_data_quark,
                             g_slice_dup (ProcessMemData, &data),
                             process_mem_data_destroy);

  gst_pinos_pool_add_buffer (pinossink->pool, buf);
  g_hash_table_insert (pinossink->buf_ids, GINT_TO_POINTER (id), buf);

  pinos_thread_main_loop_signal (pinossink->main_loop, FALSE);
}

static void
on_remove_buffer (PinosListener *listener,
                  PinosStream   *stream,
                  uint32_t       id)
{
  GstPinosSink *pinossink = SPA_CONTAINER_OF (listener, GstPinosSink, stream_remove_buffer);
  GstBuffer *buf;

  GST_LOG_OBJECT (pinossink, "remove buffer");
  buf = g_hash_table_lookup (pinossink->buf_ids, GINT_TO_POINTER (id));
  GST_MINI_OBJECT_CAST (buf)->dispose = NULL;

  gst_pinos_pool_remove_buffer (pinossink->pool, buf);
  g_hash_table_remove (pinossink->buf_ids, GINT_TO_POINTER (id));
}

static void
on_new_buffer (PinosListener *listener,
               PinosStream   *stream,
               uint32_t       id)
{
  GstPinosSink *pinossink = SPA_CONTAINER_OF (listener, GstPinosSink, stream_new_buffer);
  GstBuffer *buf;

  GST_LOG_OBJECT (pinossink, "got new buffer %u", id);
  if (pinossink->stream == NULL) {
    GST_LOG_OBJECT (pinossink, "no stream");
    return;
  }
  buf = g_hash_table_lookup (pinossink->buf_ids, GINT_TO_POINTER (id));

  if (buf) {
    gst_buffer_unref (buf);
    pinos_thread_main_loop_signal (pinossink->main_loop, FALSE);
  }
}

static void
do_send_buffer (GstPinosSink *pinossink)
{
  GstBuffer *buffer;
  ProcessMemData *data;
  gboolean res;
  guint i;

  buffer = g_queue_pop_head (&pinossink->queue);
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
    SpaData *d = &data->buf->datas[i];
    GstMemory *mem = gst_buffer_peek_memory (buffer, i);
    d->chunk->offset = mem->offset - data->offset;
    d->chunk->size = mem->size;
  }

  if (!(res = pinos_stream_send_buffer (pinossink->stream, data->id))) {
    g_warning ("can't send buffer");
    gst_buffer_unref (buffer);
    pinos_thread_main_loop_signal (pinossink->main_loop, FALSE);
  } else
    pinossink->need_ready--;
}


static void
on_need_buffer (PinosListener *listener,
                PinosStream   *stream)
{
  GstPinosSink *pinossink = SPA_CONTAINER_OF (listener, GstPinosSink, stream_need_buffer);

  pinossink->need_ready++;
  GST_DEBUG ("need buffer %u", pinossink->need_ready);
  do_send_buffer (pinossink);
}

static void
on_state_changed (PinosListener *listener,
                  PinosStream   *stream)
{
  GstPinosSink *pinossink = SPA_CONTAINER_OF (listener, GstPinosSink, stream_state_changed);
  PinosStreamState state;

  state = stream->state;
  GST_DEBUG ("got stream state %d", state);

  switch (state) {
    case PINOS_STREAM_STATE_UNCONNECTED:
    case PINOS_STREAM_STATE_CONNECTING:
    case PINOS_STREAM_STATE_CONFIGURE:
    case PINOS_STREAM_STATE_READY:
    case PINOS_STREAM_STATE_PAUSED:
    case PINOS_STREAM_STATE_STREAMING:
      break;
    case PINOS_STREAM_STATE_ERROR:
      GST_ELEMENT_ERROR (pinossink, RESOURCE, FAILED,
          ("stream error: %s", stream->error), (NULL));
      break;
  }
  pinos_thread_main_loop_signal (pinossink->main_loop, FALSE);
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
on_format_changed (PinosListener *listener,
                   PinosStream   *stream,
                   SpaFormat     *format)
{
  GstPinosSink *pinossink = SPA_CONTAINER_OF (listener, GstPinosSink, stream_format_changed);
  PinosContext *ctx = stream->context;
  GstStructure *config;
  GstCaps *caps;
  guint size;
  guint min_buffers;
  guint max_buffers;
  SpaAllocParam *port_params[3];
  SpaPODBuilder b = { NULL };
  uint8_t buffer[1024];
  SpaPODFrame f[2];

  config = gst_buffer_pool_get_config (GST_BUFFER_POOL (pinossink->pool));
  gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers, &max_buffers);

  spa_pod_builder_init (&b, buffer, sizeof (buffer));
  spa_pod_builder_object (&b, &f[0], 0, ctx->type.alloc_param_buffers.Buffers,
      PROP    (&f[1], ctx->type.alloc_param_buffers.size,    SPA_POD_TYPE_INT, size),
      PROP    (&f[1], ctx->type.alloc_param_buffers.stride,  SPA_POD_TYPE_INT, 0),
      PROP_MM (&f[1], ctx->type.alloc_param_buffers.buffers, SPA_POD_TYPE_INT, min_buffers, min_buffers, max_buffers),
      PROP    (&f[1], ctx->type.alloc_param_buffers.align,   SPA_POD_TYPE_INT, 16));
  port_params[0] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

  spa_pod_builder_object (&b, &f[0], 0, ctx->type.alloc_param_meta_enable.MetaEnable,
      PROP    (&f[1], ctx->type.alloc_param_meta_enable.type, SPA_POD_TYPE_INT, SPA_META_TYPE_HEADER));
  port_params[1] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

  spa_pod_builder_object (&b, &f[0], 0, ctx->type.alloc_param_meta_enable.MetaEnable,
      PROP    (&f[1], ctx->type.alloc_param_meta_enable.type,             SPA_POD_TYPE_INT, SPA_META_TYPE_RINGBUFFER),
      PROP    (&f[1], ctx->type.alloc_param_meta_enable.ringbufferSize,   SPA_POD_TYPE_INT,
                                                                         size * SPA_MAX (4,
                                                                                SPA_MAX (min_buffers, max_buffers))),
      PROP    (&f[1], ctx->type.alloc_param_meta_enable.ringbufferStride, SPA_POD_TYPE_INT, 0),
      PROP    (&f[1], ctx->type.alloc_param_meta_enable.ringbufferBlocks, SPA_POD_TYPE_INT, 1),
      PROP    (&f[1], ctx->type.alloc_param_meta_enable.ringbufferAlign,  SPA_POD_TYPE_INT, 16));
  port_params[2] = SPA_POD_BUILDER_DEREF (&b, f[0].ref, SpaAllocParam);

  pinos_stream_finish_format (pinossink->stream, SPA_RESULT_OK, port_params, 2);
}

static gboolean
gst_pinos_sink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstPinosSink *pinossink;
  GPtrArray *possible;
  PinosStreamState state;
  gboolean res = FALSE;

  pinossink = GST_PINOS_SINK (bsink);

  possible = gst_caps_to_format_all (caps);

  pinos_thread_main_loop_lock (pinossink->main_loop);
  state = pinossink->stream->state;

  if (state == PINOS_STREAM_STATE_ERROR)
    goto start_error;

  if (!gst_buffer_pool_is_active (GST_BUFFER_POOL_CAST (pinossink->pool))) {
    GstStructure *config = gst_buffer_pool_get_config (GST_BUFFER_POOL_CAST (pinossink->pool));
    gst_buffer_pool_config_set_params (config, caps, 8192, 16, 32);
    gst_buffer_pool_set_config (GST_BUFFER_POOL_CAST (pinossink->pool), config);
    gst_buffer_pool_set_active (GST_BUFFER_POOL_CAST (pinossink->pool), TRUE);
  }

  if (state == PINOS_STREAM_STATE_UNCONNECTED) {
    PinosStreamFlags flags = 0;

    if (pinossink->mode != GST_PINOS_SINK_MODE_PROVIDE)
      flags |= PINOS_STREAM_FLAG_AUTOCONNECT;

    pinos_stream_connect (pinossink->stream,
                          PINOS_DIRECTION_OUTPUT,
                          PINOS_STREAM_MODE_BUFFER,
                          pinossink->path,
                          flags,
                          possible->len,
                          (SpaFormat **) possible->pdata);

    while (TRUE) {
      state = pinossink->stream->state;

      if (state == PINOS_STREAM_STATE_READY)
        break;

      if (state == PINOS_STREAM_STATE_ERROR)
        goto start_error;

      pinos_thread_main_loop_wait (pinossink->main_loop);
    }
  }
  res = TRUE;

  pinos_thread_main_loop_unlock (pinossink->main_loop);

  pinossink->negotiated = res;

  return res;

start_error:
  {
    GST_ERROR ("could not start stream");
    pinos_thread_main_loop_unlock (pinossink->main_loop);
    g_ptr_array_unref (possible);
    return FALSE;
  }
}

static GstFlowReturn
gst_pinos_sink_render (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstPinosSink *pinossink;
  GstFlowReturn res = GST_FLOW_OK;

  pinossink = GST_PINOS_SINK (bsink);

  if (!pinossink->negotiated)
    goto not_negotiated;

  pinos_thread_main_loop_lock (pinossink->main_loop);
  if (pinossink->stream->state != PINOS_STREAM_STATE_STREAMING)
    goto done;

  if (buffer->pool != GST_BUFFER_POOL_CAST (pinossink->pool)) {
    GstBuffer *b = NULL;
    GstMapInfo info = { 0, };

    if ((res = gst_buffer_pool_acquire_buffer (GST_BUFFER_POOL_CAST (pinossink->pool), &b, NULL)) != GST_FLOW_OK)
      goto done;

    gst_buffer_map (b, &info, GST_MAP_WRITE);
    gst_buffer_extract (buffer, 0, info.data, info.size);
    gst_buffer_unmap (b, &info);
    gst_buffer_resize (b, 0, gst_buffer_get_size (buffer));
    buffer = b;
  } else
    gst_buffer_ref (buffer);

  GST_DEBUG ("push buffer in queue");
  g_queue_push_tail (&pinossink->queue, buffer);

//  if (pinossink->need_ready)
//    do_send_buffer (pinossink);

done:
  pinos_thread_main_loop_unlock (pinossink->main_loop);

  return res;

not_negotiated:
  {
    return GST_FLOW_NOT_NEGOTIATED;
  }
//streaming_error:
//  {
//    pinos_thread_main_loop_unlock (pinossink->main_loop);
//    return GST_FLOW_ERROR;
//  }
}

static gboolean
copy_properties (GQuark field_id,
                 const GValue *value,
                 gpointer user_data)
{
  PinosProperties *properties = user_data;

  if (G_VALUE_HOLDS_STRING (value))
    pinos_properties_set (properties,
                          g_quark_to_string (field_id),
                          g_value_get_string (value));
  return TRUE;
}

static gboolean
gst_pinos_sink_start (GstBaseSink * basesink)
{
  GstPinosSink *pinossink = GST_PINOS_SINK (basesink);
  PinosProperties *props;

  pinossink->negotiated = FALSE;

  if (pinossink->properties) {
    props = pinos_properties_new (NULL, NULL);
    gst_structure_foreach (pinossink->properties, copy_properties, props);
  } else {
    props = NULL;
  }

  pinos_thread_main_loop_lock (pinossink->main_loop);
  pinossink->stream = pinos_stream_new (pinossink->ctx, pinossink->client_name, props);
  pinossink->pool->stream = pinossink->stream;

  pinos_signal_add (&pinossink->stream->state_changed, &pinossink->stream_state_changed, on_state_changed);
  pinos_signal_add (&pinossink->stream->format_changed, &pinossink->stream_format_changed, on_format_changed);
  pinos_signal_add (&pinossink->stream->add_buffer, &pinossink->stream_add_buffer, on_add_buffer);
  pinos_signal_add (&pinossink->stream->remove_buffer, &pinossink->stream_remove_buffer, on_remove_buffer);
  pinos_signal_add (&pinossink->stream->new_buffer, &pinossink->stream_new_buffer, on_new_buffer);
  pinos_signal_add (&pinossink->stream->need_buffer, &pinossink->stream_need_buffer, on_need_buffer);
  pinos_thread_main_loop_unlock (pinossink->main_loop);

  return TRUE;
}

static gboolean
gst_pinos_sink_stop (GstBaseSink * basesink)
{
  GstPinosSink *pinossink = GST_PINOS_SINK (basesink);

  pinos_thread_main_loop_lock (pinossink->main_loop);
  if (pinossink->stream) {
    pinos_stream_stop (pinossink->stream);
    pinos_stream_disconnect (pinossink->stream);
    pinos_stream_destroy (pinossink->stream);
    pinossink->stream = NULL;
    pinossink->pool->stream = NULL;
  }
  pinos_thread_main_loop_unlock (pinossink->main_loop);

  pinossink->negotiated = FALSE;

  return TRUE;
}

static void
on_ctx_state_changed (PinosListener *listener,
                      PinosContext  *ctx)
{
  GstPinosSink *pinossink = SPA_CONTAINER_OF (listener, GstPinosSink, ctx_state_changed);
  PinosContextState state;

  state = ctx->state;
  GST_DEBUG ("got context state %d", state);

  switch (state) {
    case PINOS_CONTEXT_STATE_UNCONNECTED:
    case PINOS_CONTEXT_STATE_CONNECTING:
    case PINOS_CONTEXT_STATE_CONNECTED:
      break;
    case PINOS_CONTEXT_STATE_ERROR:
      GST_ELEMENT_ERROR (pinossink, RESOURCE, FAILED,
          ("context error: %s", ctx->error), (NULL));
      break;
  }
  pinos_thread_main_loop_signal (pinossink->main_loop, FALSE);
}

static gboolean
gst_pinos_sink_open (GstPinosSink * pinossink)
{
  if (pinos_thread_main_loop_start (pinossink->main_loop) != SPA_RESULT_OK)
    goto mainloop_error;

  pinos_thread_main_loop_lock (pinossink->main_loop);
  pinossink->ctx = pinos_context_new (pinossink->loop, g_get_application_name (), NULL);

  pinos_signal_add (&pinossink->ctx->state_changed, &pinossink->ctx_state_changed, on_ctx_state_changed);

  pinos_context_connect (pinossink->ctx);

  while (TRUE) {
    PinosContextState state = pinossink->ctx->state;

    if (state == PINOS_CONTEXT_STATE_CONNECTED)
      break;

    if (state == PINOS_CONTEXT_STATE_ERROR)
      goto connect_error;

    pinos_thread_main_loop_wait (pinossink->main_loop);
  }
  pinos_thread_main_loop_unlock (pinossink->main_loop);

  return TRUE;

  /* ERRORS */
mainloop_error:
  {
    GST_ELEMENT_ERROR (pinossink, RESOURCE, FAILED,
        ("Failed to start mainloop"), (NULL));
    return FALSE;
  }
connect_error:
  {
    pinos_thread_main_loop_unlock (pinossink->main_loop);
    return FALSE;
  }
}

static gboolean
gst_pinos_sink_close (GstPinosSink * pinossink)
{
  pinos_thread_main_loop_lock (pinossink->main_loop);
  if (pinossink->stream) {
    pinos_stream_disconnect (pinossink->stream);
  }
  if (pinossink->ctx) {
    pinos_context_disconnect (pinossink->ctx);

    while (TRUE) {
      PinosContextState state = pinossink->ctx->state;

      if (state == PINOS_CONTEXT_STATE_UNCONNECTED)
        break;

      if (state == PINOS_CONTEXT_STATE_ERROR)
        break;

      pinos_thread_main_loop_wait (pinossink->main_loop);
    }
  }
  pinos_thread_main_loop_unlock (pinossink->main_loop);

  pinos_thread_main_loop_stop (pinossink->main_loop);

  if (pinossink->stream) {
    pinos_stream_destroy (pinossink->stream);
    pinossink->stream = NULL;
  }

  if (pinossink->ctx) {
    pinos_context_destroy (pinossink->ctx);
    pinossink->ctx = NULL;
  }

  return TRUE;
}

static GstStateChangeReturn
gst_pinos_sink_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstPinosSink *this = GST_PINOS_SINK_CAST (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_pinos_sink_open (this))
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
      gst_pinos_sink_close (this);
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
