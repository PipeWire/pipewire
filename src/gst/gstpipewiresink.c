/* GStreamer
 *
 * Copyright © 2018 Wim Taymans
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
#include <unistd.h>
#include <sys/socket.h>

#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#include "gstpipewireformat.h"

GST_DEBUG_CATEGORY_STATIC (pipewire_sink_debug);
#define GST_CAT_DEFAULT pipewire_sink_debug

#define DEFAULT_PROP_MODE GST_PIPEWIRE_SINK_MODE_DEFAULT

enum
{
  PROP_0,
  PROP_PATH,
  PROP_CLIENT_NAME,
  PROP_STREAM_PROPERTIES,
  PROP_MODE,
  PROP_FD
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

  pw_thread_loop_destroy (pwsink->loop);
  pwsink->loop = NULL;

  if (pwsink->properties)
    gst_structure_free (pwsink->properties);
  g_free (pwsink->path);
  g_free (pwsink->client_name);

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

   g_object_class_install_property (gobject_class,
                                    PROP_FD,
                                    g_param_spec_int ("fd",
                                                      "Fd",
                                                      "The fd to connect with",
                                                      -1, G_MAXINT, -1,
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
}

static void
pool_activated (GstPipeWirePool *pool, GstPipeWireSink *sink)
{
  GstStructure *config;
  GstCaps *caps;
  guint size;
  guint min_buffers;
  guint max_buffers;
  const struct spa_pod *port_params[2];
  struct spa_pod_builder b = { NULL };
  uint8_t buffer[1024];
  struct spa_pod_frame f;

  config = gst_buffer_pool_get_config (GST_BUFFER_POOL (pool));
  gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers, &max_buffers);

  spa_pod_builder_init (&b, buffer, sizeof (buffer));
  spa_pod_builder_push_object (&b, &f, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
  if (size == 0)
    spa_pod_builder_add (&b,
        SPA_PARAM_BUFFERS_size, SPA_POD_CHOICE_RANGE_Int(0, 0, INT32_MAX),
        0);
  else
    spa_pod_builder_add (&b,
        SPA_PARAM_BUFFERS_size, SPA_POD_CHOICE_RANGE_Int(size, size, INT32_MAX),
        0);

  spa_pod_builder_add (&b,
      SPA_PARAM_BUFFERS_stride,  SPA_POD_CHOICE_RANGE_Int(0, 0, INT32_MAX),
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(min_buffers, min_buffers,
                                               max_buffers ? max_buffers : INT32_MAX),
      SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16),
      0);
  port_params[0] = spa_pod_builder_pop (&b, &f);

  port_params[1] = spa_pod_builder_add_object (&b,
      SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
      SPA_PARAM_META_type, SPA_POD_Int(SPA_META_Header),
      SPA_PARAM_META_size, SPA_POD_Int(sizeof (struct spa_meta_header)));


  pw_thread_loop_lock (sink->loop);
  pw_stream_update_params (sink->stream, port_params, 2);
  pw_thread_loop_unlock (sink->loop);
}

static void
gst_pipewire_sink_init (GstPipeWireSink * sink)
{
  sink->pool =  gst_pipewire_pool_new ();
  sink->client_name = g_strdup(pw_get_client_name());
  sink->mode = DEFAULT_PROP_MODE;
  sink->fd = -1;

  g_signal_connect (sink->pool, "activated", G_CALLBACK (pool_activated), sink);

  g_queue_init (&sink->queue);

  sink->loop = pw_thread_loop_new ("pipewire-sink-loop", NULL);
  sink->context = pw_context_new (pw_thread_loop_get_loop(sink->loop), NULL, 0);
  GST_DEBUG ("loop %p context %p", sink->loop, sink->context);
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

    case PROP_FD:
      pwsink->fd = g_value_get_int (value);
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

    case PROP_FD:
      g_value_set_int (value, pwsink->fd);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
on_add_buffer (void *_data, struct pw_buffer *b)
{
  GstPipeWireSink *pwsink = _data;
  gst_pipewire_pool_wrap_buffer (pwsink->pool, b);
  pw_thread_loop_signal (pwsink->loop, FALSE);
}

static void
on_remove_buffer (void *_data, struct pw_buffer *b)
{
  GstPipeWireSink *pwsink = _data;
  GstPipeWirePoolData *data = b->user_data;

  GST_LOG_OBJECT (pwsink, "remove buffer");

  if (g_queue_remove (&pwsink->queue, data->buf))
    gst_buffer_unref (data->buf);
  gst_buffer_unref (data->buf);
}

static void
do_send_buffer (GstPipeWireSink *pwsink)
{
  GstBuffer *buffer;
  GstPipeWirePoolData *data;
  gboolean res;
  guint i;
  struct spa_buffer *b;

  buffer = g_queue_pop_head (&pwsink->queue);
  if (buffer == NULL) {
    GST_WARNING ("out of buffers");
    return;
  }

  data = gst_pipewire_pool_get_data(buffer);

  b = data->b->buffer;

  if (data->header) {
    data->header->seq = GST_BUFFER_OFFSET (buffer);
    data->header->pts = GST_BUFFER_PTS (buffer);
    data->header->dts_offset = GST_BUFFER_DTS (buffer);
  }
  for (i = 0; i < b->n_datas; i++) {
    struct spa_data *d = &b->datas[i];
    GstMemory *mem = gst_buffer_peek_memory (buffer, i);
    d->chunk->offset = mem->offset - data->offset;
    d->chunk->size = mem->size;
  }

  if ((res = pw_stream_queue_buffer (pwsink->stream, data->b)) < 0) {
    g_warning ("can't send buffer %s", spa_strerror(res));
    pw_thread_loop_signal (pwsink->loop, FALSE);
  } else
    pwsink->need_ready--;
}


static void
on_process (void *data)
{
  GstPipeWireSink *pwsink = data;

  if (pwsink->stream == NULL) {
    GST_LOG_OBJECT (pwsink, "no stream");
    return;
  }

  g_cond_signal (&pwsink->pool->cond);

  pwsink->need_ready++;
  GST_DEBUG ("need buffer %u", pwsink->need_ready);
  do_send_buffer (pwsink);
}

static void
on_state_changed (void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
  GstPipeWireSink *pwsink = data;

  GST_DEBUG ("got stream state %d", state);

  switch (state) {
    case PW_STREAM_STATE_UNCONNECTED:
    case PW_STREAM_STATE_CONNECTING:
    case PW_STREAM_STATE_PAUSED:
    case PW_STREAM_STATE_STREAMING:
      break;
    case PW_STREAM_STATE_ERROR:
      GST_ELEMENT_ERROR (pwsink, RESOURCE, FAILED,
          ("stream error: %s", error), (NULL));
      break;
  }
  pw_thread_loop_signal (pwsink->loop, FALSE);
}

static void
on_param_changed (void *data, uint32_t id, const struct spa_pod *param)
{
  GstPipeWireSink *pwsink = data;

  if (param == NULL || id != SPA_PARAM_Format)
          return;

  if (gst_buffer_pool_is_active (GST_BUFFER_POOL_CAST (pwsink->pool)))
    pool_activated (pwsink->pool, pwsink);
}

static gboolean
gst_pipewire_sink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstPipeWireSink *pwsink;
  GPtrArray *possible;
  enum pw_stream_state state;
  const char *error = NULL;
  gboolean res = FALSE;

  pwsink = GST_PIPEWIRE_SINK (bsink);

  possible = gst_caps_to_format_all (caps, SPA_PARAM_EnumFormat);

  pw_thread_loop_lock (pwsink->loop);
  state = pw_stream_get_state (pwsink->stream, &error);

  if (state == PW_STREAM_STATE_ERROR)
    goto start_error;

  if (state == PW_STREAM_STATE_UNCONNECTED) {
    enum pw_stream_flags flags = 0;

    if (pwsink->mode != GST_PIPEWIRE_SINK_MODE_PROVIDE)
      flags |= PW_STREAM_FLAG_AUTOCONNECT;
    else
      flags |= PW_STREAM_FLAG_DRIVER;

    pw_stream_connect (pwsink->stream,
                          PW_DIRECTION_OUTPUT,
                          pwsink->path ? (uint32_t)atoi(pwsink->path) : PW_ID_ANY,
                          flags,
                          (const struct spa_pod **) possible->pdata,
                          possible->len);

    while (TRUE) {
      state = pw_stream_get_state (pwsink->stream, &error);

      if (state == PW_STREAM_STATE_PAUSED)
        break;

      if (state == PW_STREAM_STATE_ERROR)
        goto start_error;

      pw_thread_loop_wait (pwsink->loop);
    }
  }
  res = TRUE;

  pw_thread_loop_unlock (pwsink->loop);

  pwsink->negotiated = res;

  return res;

start_error:
  {
    GST_ERROR ("could not start stream: %s", error);
    pw_thread_loop_unlock (pwsink->loop);
    g_ptr_array_unref (possible);
    return FALSE;
  }
}

static GstFlowReturn
gst_pipewire_sink_render (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstPipeWireSink *pwsink;
  GstFlowReturn res = GST_FLOW_OK;
  const char *error = NULL;

  pwsink = GST_PIPEWIRE_SINK (bsink);

  if (!pwsink->negotiated)
    goto not_negotiated;

  pw_thread_loop_lock (pwsink->loop);
  if (pw_stream_get_state (pwsink->stream, &error) != PW_STREAM_STATE_STREAMING)
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
  } else {
    gst_buffer_ref (buffer);
  }

  GST_DEBUG ("push buffer in queue");
  g_queue_push_tail (&pwsink->queue, buffer);

  if (pwsink->mode == GST_PIPEWIRE_SINK_MODE_PROVIDE)
    do_send_buffer (pwsink);

done:
  pw_thread_loop_unlock (pwsink->loop);

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

static const struct pw_stream_events stream_events = {
        PW_VERSION_STREAM_EVENTS,
        .state_changed = on_state_changed,
        .param_changed = on_param_changed,
        .add_buffer = on_add_buffer,
        .remove_buffer = on_remove_buffer,
        .process = on_process,
};

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

  pw_thread_loop_lock (pwsink->loop);
  pwsink->stream = pw_stream_new (pwsink->core, pwsink->client_name, props);
  pwsink->pool->stream = pwsink->stream;

  pw_stream_add_listener(pwsink->stream,
                         &pwsink->stream_listener,
                         &stream_events,
                         pwsink);

  pw_thread_loop_unlock (pwsink->loop);

  return TRUE;
}

static gboolean
gst_pipewire_sink_stop (GstBaseSink * basesink)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (basesink);

  pw_thread_loop_lock (pwsink->loop);
  if (pwsink->stream) {
    pw_stream_disconnect (pwsink->stream);
    pw_stream_destroy (pwsink->stream);
    pwsink->stream = NULL;
    pwsink->pool->stream = NULL;
  }
  pw_thread_loop_unlock (pwsink->loop);

  pwsink->negotiated = FALSE;

  return TRUE;
}

static gboolean
gst_pipewire_sink_open (GstPipeWireSink * pwsink)
{
  if (pw_thread_loop_start (pwsink->loop) < 0)
    goto mainloop_error;

  pw_thread_loop_lock (pwsink->loop);

  if (pwsink->fd == -1)
    pwsink->core = pw_context_connect (pwsink->context, NULL, 0);
  else
    pwsink->core = pw_context_connect_fd (pwsink->context, dup(pwsink->fd), NULL, 0);

  if (pwsink->core == NULL)
    goto connect_error;

  pw_thread_loop_unlock (pwsink->loop);

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
    GST_ELEMENT_ERROR (pwsink, RESOURCE, FAILED,
        ("Failed to connect"), (NULL));
    pw_thread_loop_unlock (pwsink->loop);
    return FALSE;
  }
}

static gboolean
gst_pipewire_sink_close (GstPipeWireSink * pwsink)
{
  pw_thread_loop_lock (pwsink->loop);
  if (pwsink->stream) {
    pw_stream_disconnect (pwsink->stream);
  }
  if (pwsink->core) {
    pw_core_disconnect (pwsink->core);
    pwsink->core = NULL;
  }
  pw_thread_loop_unlock (pwsink->loop);

  pw_thread_loop_stop (pwsink->loop);

  if (pwsink->stream) {
    pw_stream_destroy (pwsink->stream);
    pwsink->stream = NULL;
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
