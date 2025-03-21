/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

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

#define PW_ENABLE_DEPRECATED

#include "config.h"
#include "gstpipewiresink.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/socket.h>

#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#include <spa/utils/dll.h>

#include <gst/video/video.h>

#include "gstpipewireclock.h"
#include "gstpipewireformat.h"

GST_DEBUG_CATEGORY_STATIC (pipewire_sink_debug);
#define GST_CAT_DEFAULT pipewire_sink_debug

#define DEFAULT_PROP_MODE GST_PIPEWIRE_SINK_MODE_DEFAULT
#define DEFAULT_PROP_SLAVE_METHOD GST_PIPEWIRE_SINK_SLAVE_METHOD_NONE
#define DEFAULT_PROP_USE_BUFFERPOOL USE_BUFFERPOOL_AUTO

enum
{
  PROP_0,
  PROP_PATH,
  PROP_TARGET_OBJECT,
  PROP_CLIENT_NAME,
  PROP_CLIENT_PROPERTIES,
  PROP_STREAM_PROPERTIES,
  PROP_MODE,
  PROP_FD,
  PROP_SLAVE_METHOD,
  PROP_USE_BUFFERPOOL,
};

GType
gst_pipewire_sink_mode_get_type (void)
{
  static gsize mode_type = 0;
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

GType
gst_pipewire_sink_slave_method_get_type (void)
{
  static gsize method_type = 0;
  static const GEnumValue method[] = {
    {GST_PIPEWIRE_SINK_SLAVE_METHOD_NONE, "GST_PIPEWIRE_SINK_SLAVE_METHOD_NONE", "none"},
    {GST_PIPEWIRE_SINK_SLAVE_METHOD_RESAMPLE, "GST_PIPEWIRE_SINK_SLAVE_METHOD_RESAMPLE", "resample"},
    {0, NULL, NULL},
  };

  if (g_once_init_enter (&method_type)) {
    GType tmp =
        g_enum_register_static ("GstPipeWireSinkSlaveMethod", method);
    g_once_init_leave (&method_type, tmp);
  }

  return (GType) method_type;
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

static  gboolean gst_pipewire_sink_event (GstBaseSink *sink, GstEvent *event);

static GstClock *
gst_pipewire_sink_provide_clock (GstElement * elem)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (elem);
  GstClock *clock;

  GST_OBJECT_LOCK (pwsink);
  if (!GST_OBJECT_FLAG_IS_SET (pwsink, GST_ELEMENT_FLAG_PROVIDE_CLOCK))
    goto clock_disabled;

  if (pwsink->stream->clock)
    clock = GST_CLOCK_CAST (gst_object_ref (pwsink->stream->clock));
  else
    clock = NULL;
  GST_OBJECT_UNLOCK (pwsink);

  return clock;

  /* ERRORS */
clock_disabled:
  {
    GST_DEBUG_OBJECT (pwsink, "clock provide disabled");
    GST_OBJECT_UNLOCK (pwsink);
    return NULL;
  }
}

static void
gst_pipewire_sink_finalize (GObject * object)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (object);

  gst_clear_object (&pwsink->stream);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_pipewire_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK (bsink);

  if (pwsink->use_bufferpool != USE_BUFFERPOOL_NO)
    gst_query_add_allocation_pool (query, GST_BUFFER_POOL_CAST (pwsink->stream->pool), 0,
        PIPEWIRE_POOL_MIN_BUFFERS, PIPEWIRE_POOL_MAX_BUFFERS);

  gst_query_add_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL);
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
                                                        G_PARAM_STATIC_STRINGS |
                                                        G_PARAM_DEPRECATED));

  g_object_class_install_property (gobject_class,
                                   PROP_TARGET_OBJECT,
                                   g_param_spec_string ("target-object",
                                                        "Target object",
                                                        "The sink name/serial to connect to (NULL = default)",
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
                                    PROP_CLIENT_PROPERTIES,
                                    g_param_spec_boxed ("client-properties",
                                                        "Client properties",
                                                        "List of PipeWire client properties",
                                                        GST_TYPE_STRUCTURE,
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
                                                        G_PARAM_STATIC_STRINGS |
                                                        GST_PARAM_MUTABLE_READY));

   g_object_class_install_property (gobject_class,
                                    PROP_FD,
                                    g_param_spec_int ("fd",
                                                      "Fd",
                                                      "The fd to connect with",
                                                      -1, G_MAXINT, -1,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_SLAVE_METHOD,
                                   g_param_spec_enum ("slave-method",
                                                      "Slave Method",
                                                      "Algorithm used to match the rate of the masterclock",
                                                      GST_TYPE_PIPEWIRE_SINK_SLAVE_METHOD,
                                                      DEFAULT_PROP_SLAVE_METHOD,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_USE_BUFFERPOOL,
                                   g_param_spec_boolean ("use-bufferpool",
                                                      "Use bufferpool",
                                                      "Use bufferpool (default: true for video, false for audio)",
                                                      DEFAULT_PROP_USE_BUFFERPOOL,
                                                      G_PARAM_READWRITE |
                                                      G_PARAM_STATIC_STRINGS));

  gstelement_class->provide_clock = gst_pipewire_sink_provide_clock;
  gstelement_class->change_state = gst_pipewire_sink_change_state;

  gst_element_class_set_static_metadata (gstelement_class,
      "PipeWire sink", "Sink/Audio/Video",
      "Send audio/video to PipeWire", "Wim Taymans <wim.taymans@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pipewire_sink_template));

  gstbasesink_class->set_caps = gst_pipewire_sink_setcaps;
  gstbasesink_class->fixate = gst_pipewire_sink_sink_fixate;
  gstbasesink_class->propose_allocation = gst_pipewire_sink_propose_allocation;
  gstbasesink_class->render = gst_pipewire_sink_render;
  gstbasesink_class->event = gst_pipewire_sink_event;

  GST_DEBUG_CATEGORY_INIT (pipewire_sink_debug, "pipewiresink", 0,
      "PipeWire Sink");
}

static void
gst_pipewire_sink_update_params (GstPipeWireSink *sink)
{
  GstPipeWirePool *pool = sink->stream->pool;
  GstStructure *config;
  GstCaps *caps;
  guint size;
  guint min_buffers;
  guint max_buffers;
  const struct spa_pod *port_params[3];
  struct spa_pod_builder b = { NULL };
  uint8_t buffer[1024];
  struct spa_pod_frame f;
  guint n_params = 0;

  config = gst_buffer_pool_get_config (GST_BUFFER_POOL (pool));
  gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers, &max_buffers);

  /* We cannot dynamically grow the pool */
  if (max_buffers == 0) {
    GST_WARNING_OBJECT (sink, "cannot support unlimited buffers in pool");
    max_buffers = PIPEWIRE_POOL_MAX_BUFFERS;
  }

  spa_pod_builder_init (&b, buffer, sizeof (buffer));
  spa_pod_builder_push_object (&b, &f, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
  spa_pod_builder_add (&b,
      SPA_PARAM_BUFFERS_size, SPA_POD_CHOICE_RANGE_Int(size, size, INT32_MAX),
      0);
  if (sink->is_video) {
    /* MUST have n_datas == n_planes */
    spa_pod_builder_add (&b,
        SPA_PARAM_BUFFERS_blocks,
        SPA_POD_Int(GST_VIDEO_INFO_N_PLANES (&pool->video_info)), 0);
  }

  spa_pod_builder_add (&b,
      SPA_PARAM_BUFFERS_stride,  SPA_POD_CHOICE_RANGE_Int(0, 0, INT32_MAX),
      /* At this stage, we will request as many buffers as we _might_ need as
       * the default, since we can't grow the pool once this is set */
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(
              max_buffers, min_buffers, max_buffers),
      SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1<<SPA_DATA_MemFd),
      0);
  port_params[n_params++] = spa_pod_builder_pop (&b, &f);

  port_params[n_params++] = spa_pod_builder_add_object (&b,
      SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
      SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
      SPA_PARAM_META_size, SPA_POD_Int(sizeof (struct spa_meta_header)));

  if (sink->is_video) {
    port_params[n_params++] = spa_pod_builder_add_object (&b,
        SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof (struct spa_meta_region)));
  }

  pw_thread_loop_lock (sink->stream->core->loop);
  pw_stream_update_params (sink->stream->pwstream, port_params, n_params);
  pw_thread_loop_unlock (sink->stream->core->loop);
}

static void
pool_activated (GstPipeWirePool *pool, GstPipeWireSink *sink)
{
  GST_DEBUG_OBJECT (pool, "activated");
  g_cond_signal (&sink->stream->pool->cond);
}

static void
gst_pipewire_sink_init (GstPipeWireSink * sink)
{
  sink->stream =  gst_pipewire_stream_new (GST_ELEMENT (sink));

  sink->mode = DEFAULT_PROP_MODE;
  sink->use_bufferpool = DEFAULT_PROP_USE_BUFFERPOOL;
  sink->is_video = false;

  GST_OBJECT_FLAG_SET (sink, GST_ELEMENT_FLAG_PROVIDE_CLOCK);

  g_signal_connect (sink->stream->pool, "activated", G_CALLBACK (pool_activated), sink);
}

static GstCaps *
gst_pipewire_sink_sink_fixate (GstBaseSink * bsink, GstCaps * caps)
{
  GstStructure *structure;
  GstPipeWireSink *pwsink = GST_PIPEWIRE_SINK(bsink);

  caps = gst_caps_make_writable (caps);

  structure = gst_caps_get_structure (caps, 0);

  if (gst_structure_has_name (structure, "video/x-raw")) {
    pwsink->is_video = true;
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
  } else if (gst_structure_has_name (structure, "audio/mpeg")) {
    gst_structure_fixate_field_string (structure, "format", "Encoded");
    gst_structure_fixate_field_nearest_int (structure, "channels", 2);
    gst_structure_fixate_field_nearest_int (structure, "rate", 44100);
  } else if (gst_structure_has_name (structure, "audio/x-flac")) {
    gst_structure_fixate_field_string (structure, "format", "Encoded");
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
      g_free (pwsink->stream->path);
      pwsink->stream->path = g_value_dup_string (value);
      break;

    case PROP_TARGET_OBJECT:
      g_free (pwsink->stream->target_object);
      pwsink->stream->target_object = g_value_dup_string (value);
      break;

    case PROP_CLIENT_NAME:
      g_free (pwsink->stream->client_name);
      pwsink->stream->client_name = g_value_dup_string (value);
      break;

    case PROP_CLIENT_PROPERTIES:
      if (pwsink->stream->client_properties)
        gst_structure_free (pwsink->stream->client_properties);
      pwsink->stream->client_properties =
          gst_structure_copy (gst_value_get_structure (value));
      break;

    case PROP_STREAM_PROPERTIES:
      if (pwsink->stream->stream_properties)
        gst_structure_free (pwsink->stream->stream_properties);
      pwsink->stream->stream_properties =
          gst_structure_copy (gst_value_get_structure (value));
      break;

    case PROP_MODE:
      pwsink->mode = g_value_get_enum (value);
      break;

    case PROP_FD:
      pwsink->stream->fd = g_value_get_int (value);
      break;

    case PROP_SLAVE_METHOD:
      pwsink->slave_method = g_value_get_enum (value);
      break;

    case PROP_USE_BUFFERPOOL:
      if(g_value_get_boolean (value))
        pwsink->use_bufferpool = USE_BUFFERPOOL_YES;
      else
        pwsink->use_bufferpool = USE_BUFFERPOOL_NO;
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
      g_value_set_string (value, pwsink->stream->path);
      break;

    case PROP_TARGET_OBJECT:
      g_value_set_string (value, pwsink->stream->target_object);
      break;

    case PROP_CLIENT_NAME:
      g_value_set_string (value, pwsink->stream->client_name);
      break;

    case PROP_CLIENT_PROPERTIES:
      gst_value_set_structure (value, pwsink->stream->client_properties);
      break;

    case PROP_STREAM_PROPERTIES:
      gst_value_set_structure (value, pwsink->stream->stream_properties);
      break;

    case PROP_MODE:
      g_value_set_enum (value, pwsink->mode);
      break;

    case PROP_FD:
      g_value_set_int (value, pwsink->stream->fd);
      break;

    case PROP_SLAVE_METHOD:
      g_value_set_enum (value, pwsink->slave_method);
      break;

    case PROP_USE_BUFFERPOOL:
      g_value_set_boolean (value, !!pwsink->use_bufferpool);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void rate_match_resample(GstPipeWireSink *pwsink)
{
  GstPipeWireStream *stream = pwsink->stream;
  double err, corr;
  struct pw_time ts;
  guint64 queued, now, elapsed, target;

  if (!pwsink->rate_match)
    return;

  pw_stream_get_time_n(stream->pwstream, &ts, sizeof(ts));
  now = pw_stream_get_nsec(stream->pwstream);
  if (ts.now != 0)
    elapsed = gst_util_uint64_scale_int (now - ts.now, ts.rate.denom, GST_SECOND * ts.rate.num);
  else
    elapsed = 0;

  queued = ts.queued - ts.size;
  target = elapsed;
  err = ((gint64)queued - ((gint64)target));

  corr = spa_dll_update(&stream->dll, SPA_CLAMPD(err, -128.0, 128.0));

  stream->err_wdw = (double)ts.rate.denom/ts.size;

  double avg = (stream->err_avg * stream->err_wdw + (err - stream->err_avg)) / (stream->err_wdw + 1.0);
  stream->err_var = (stream->err_var * stream->err_wdw +
                    (err - stream->err_avg) * (err - avg)) / (stream->err_wdw + 1.0);
  stream->err_avg = avg;

  if (stream->last_ts == 0 || stream->last_ts + SPA_NSEC_PER_SEC < now) {
    double bw;

    stream->last_ts = now;

    if (stream->err_var == 0.0)
      bw = 0.0;
    else
      bw = fabs(stream->err_avg) / sqrt(fabs(stream->err_var));

    spa_dll_set_bw(&stream->dll, SPA_CLAMPD(bw, 0.001, SPA_DLL_BW_MAX), ts.size, ts.rate.denom);

    GST_INFO_OBJECT (pwsink, "q:%"PRIi64"/%"PRIi64" e:%"PRIu64" err:%+03f corr:%f %f %f %f",
                    ts.queued, ts.size, elapsed, err, corr,
		    stream->err_avg, stream->err_var, stream->dll.bw);
  }

  pw_stream_set_rate (stream->pwstream, corr);
}

static void
on_add_buffer (void *_data, struct pw_buffer *b)
{
  GstPipeWireSink *pwsink = _data;
  GST_DEBUG_OBJECT (pwsink, "add pw_buffer %p", b);
  gst_pipewire_pool_wrap_buffer (pwsink->stream->pool, b);
}

static void
on_remove_buffer (void *_data, struct pw_buffer *b)
{
  GstPipeWireSink *pwsink = _data;

  GST_DEBUG_OBJECT (pwsink, "remove pw_buffer %p", b);
  gst_pipewire_pool_remove_buffer (pwsink->stream->pool, b);

  if (!gst_pipewire_pool_has_buffers (pwsink->stream->pool) &&
      !GST_BUFFER_POOL_IS_FLUSHING (GST_BUFFER_POOL_CAST (pwsink->stream->pool))) {
      if (pwsink->mode != GST_PIPEWIRE_SINK_MODE_PROVIDE) {
        GST_ELEMENT_ERROR (pwsink, RESOURCE, NOT_FOUND,
          ("all buffers have been removed"),
          ("PipeWire link to remote node was destroyed"));
      }
  }
}

static void
do_send_buffer (GstPipeWireSink *pwsink, GstBuffer *buffer)
{
  GstPipeWirePoolData *data;
  GstPipeWireStream *stream = pwsink->stream;
  gboolean res;
  guint i;
  struct spa_buffer *b;

  data = gst_pipewire_pool_get_data(buffer);

  b = data->b->buffer;

  if (data->header) {
    data->header->seq = GST_BUFFER_OFFSET (buffer);
    data->header->pts = GST_BUFFER_PTS (buffer);
    if (GST_BUFFER_DTS(buffer) != GST_CLOCK_TIME_NONE)
      data->header->dts_offset = GST_BUFFER_DTS (buffer) - GST_BUFFER_PTS (buffer);
    else
      data->header->dts_offset = 0;
  }
  if (data->crop) {
    GstVideoCropMeta *meta = gst_buffer_get_video_crop_meta (buffer);
    if (meta) {
      data->crop->region.position.x = meta->x;
      data->crop->region.position.y = meta->y;
      data->crop->region.size.width = meta->width;
      data->crop->region.size.height = meta->width;
    }
  }
  data->b->size = 0;

  spa_assert(b->n_datas == gst_buffer_n_memory(buffer));

  for (i = 0; i < b->n_datas; i++) {
    struct spa_data *d = &b->datas[i];
    GstMemory *mem = gst_buffer_peek_memory (buffer, i);
    d->chunk->offset = mem->offset;
    d->chunk->size = mem->size;
    d->chunk->stride = stream->pool->video_info.stride[i];

    data->b->size += mem->size / 4;
  }

  GstVideoMeta *meta = gst_buffer_get_video_meta (buffer);
  if (meta) {
    if (meta->n_planes == b->n_datas) {
      uint32_t n_planes = GST_VIDEO_INFO_N_PLANES (&data->pool->video_info);
      gsize video_size = 0;

      for (i = 0; i < n_planes; i++) {
        struct spa_data *d = &b->datas[i];

        d->chunk->stride = meta->stride[i];
        d->chunk->offset = meta->offset[i] - video_size;

        video_size += d->chunk->size;
      }
    } else {
      GST_ERROR_OBJECT (pwsink, "plane num not matching, meta:%u buffer:%u",
          meta->n_planes, b->n_datas);
    }
  }

  if ((res = pw_stream_queue_buffer (stream->pwstream, data->b)) < 0) {
    GST_WARNING_OBJECT (pwsink, "can't send buffer %s", spa_strerror(res));
  } else {
    data->queued = TRUE;
    GST_LOG_OBJECT(pwsink, "queued pwbuffer: %p; gstbuffer %p ",data->b, buffer);
  }

  switch (pwsink->slave_method) {
    case GST_PIPEWIRE_SINK_SLAVE_METHOD_NONE:
      break;
    case GST_PIPEWIRE_SINK_SLAVE_METHOD_RESAMPLE:
      rate_match_resample(pwsink);
      break;
  }
}


static void
on_process (void *data)
{
  GstPipeWireSink *pwsink = data;
  GST_LOG_OBJECT (pwsink, "signal");
  g_cond_signal (&pwsink->stream->pool->cond);
}

static void
on_state_changed (void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error)
{
  GstPipeWireSink *pwsink = data;

  GST_DEBUG_OBJECT (pwsink, "got stream state \"%s\" (%d)",
      pw_stream_state_as_string(state), state);

  switch (state) {
    case PW_STREAM_STATE_UNCONNECTED:
    case PW_STREAM_STATE_CONNECTING:
    case PW_STREAM_STATE_PAUSED:
      break;
    case PW_STREAM_STATE_STREAMING:
      if (pw_stream_is_driving (pwsink->stream->pwstream))
        pw_stream_trigger_process (pwsink->stream->pwstream);
      break;
    case PW_STREAM_STATE_ERROR:
      /* make the error permanent, if it is not already;
         pw_stream_set_error() will recursively call us again */
      if (pw_stream_get_state (pwsink->stream->pwstream, NULL) != PW_STREAM_STATE_ERROR)
        pw_stream_set_error (pwsink->stream->pwstream, -EPIPE, "%s", error);
      else
        GST_ELEMENT_ERROR (pwsink, RESOURCE, FAILED,
            ("stream error: %s", error), (NULL));
      break;
  }
  pw_thread_loop_signal (pwsink->stream->core->loop, FALSE);
}

static void
on_param_changed (void *data, uint32_t id, const struct spa_pod *param)
{
  GstPipeWireSink *pwsink = data;
  GstPipeWirePool *pool = pwsink->stream->pool;

  if (param == NULL || id != SPA_PARAM_Format)
    return;

  GST_OBJECT_LOCK (pool);
  while (!gst_buffer_pool_is_active (GST_BUFFER_POOL (pool))) {
    GST_DEBUG_OBJECT (pool, "waiting for pool to become active");
    g_cond_wait(&pool->cond, GST_OBJECT_GET_LOCK (pool));
  }
  GST_OBJECT_UNLOCK (pool);

  gst_pipewire_sink_update_params (pwsink);
}

static gboolean
gst_pipewire_sink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstPipeWireSink *pwsink;
  g_autoptr(GPtrArray) possible = NULL;
  enum pw_stream_state state;
  const char *error = NULL;
  GstStructure *config, *s;
  guint size;
  guint min_buffers;
  guint max_buffers;
  struct timespec abstime;
  gint rate;

  pwsink = GST_PIPEWIRE_SINK (bsink);

  s = gst_caps_get_structure (caps, 0);
  if (gst_structure_has_name (s, "audio/x-raw")) {
    gst_structure_get_int (s, "rate", &rate);
    pwsink->rate = rate;
    pwsink->rate_match = true;

    /* Don't provide bufferpool for audio if not requested by the application/user */
    if (pwsink->use_bufferpool != USE_BUFFERPOOL_YES)
      pwsink->use_bufferpool = USE_BUFFERPOOL_NO;
  } else {
    pwsink->rate = rate = 0;
    pwsink->rate_match = false;
    pwsink->is_video = true;
  }

  spa_dll_set_bw(&pwsink->stream->dll, SPA_DLL_BW_MIN, 4096, rate);

  possible = gst_caps_to_format_all (caps);

  pw_thread_loop_lock (pwsink->stream->core->loop);
  state = pw_stream_get_state (pwsink->stream->pwstream, &error);

  if (state == PW_STREAM_STATE_ERROR)
    goto start_error;

  if (state == PW_STREAM_STATE_UNCONNECTED) {
    enum pw_stream_flags flags;
    uint32_t target_id;
    struct spa_dict_item items[3];
    uint32_t n_items = 0;
    char buf[64];

    flags = PW_STREAM_FLAG_ASYNC;
    flags |= PW_STREAM_FLAG_EARLY_PROCESS;
    if (pwsink->mode != GST_PIPEWIRE_SINK_MODE_PROVIDE)
      flags |= PW_STREAM_FLAG_AUTOCONNECT;
    else
      flags |= PW_STREAM_FLAG_DRIVER;

#ifdef HAVE_GSTREAMER_SHM_ALLOCATOR
    flags |= PW_STREAM_FLAG_ALLOC_BUFFERS;
    pwsink->stream->pool->allocate_memory = true;
#endif

    target_id = pwsink->stream->path ? (uint32_t)atoi(pwsink->stream->path) : PW_ID_ANY;

    if (pwsink->stream->target_object) {
      uint64_t serial;

      items[n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_TARGET_OBJECT, pwsink->stream->target_object);

      /* If target.object is a name, set it also to node.target */
      if (!spa_atou64(pwsink->stream->target_object, &serial, 0)) {
        target_id = PW_ID_ANY;
        /* XXX deprecated but the portal and some example apps only
         * provide the object id */
        items[n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_TARGET, pwsink->stream->target_object);
      }
    }
    if (rate != 0) {
      snprintf(buf, sizeof(buf), "1/%u", rate);
      items[n_items++] = SPA_DICT_ITEM_INIT(PW_KEY_NODE_RATE, buf);
    }
    if (n_items > 0)
	    pw_stream_update_properties (pwsink->stream->pwstream, &SPA_DICT_INIT(items, n_items));

    pw_stream_connect (pwsink->stream->pwstream,
                          PW_DIRECTION_OUTPUT,
                          target_id,
                          flags,
                          (const struct spa_pod **) possible->pdata,
                          possible->len);

    pw_thread_loop_get_time (pwsink->stream->core->loop, &abstime,
              GST_PIPEWIRE_DEFAULT_TIMEOUT * SPA_NSEC_PER_SEC);

    while (TRUE) {
      state = pw_stream_get_state (pwsink->stream->pwstream, &error);

      if (state >= PW_STREAM_STATE_PAUSED)
        break;

      if (state == PW_STREAM_STATE_ERROR)
        goto start_error;

      if (pw_thread_loop_timed_wait_full (pwsink->stream->core->loop, &abstime) < 0) {
        error = "timeout";
        goto start_error;
      }
    }
  }

  gst_pipewire_clock_reset (GST_PIPEWIRE_CLOCK (pwsink->stream->clock), 0);

  config = gst_buffer_pool_get_config (GST_BUFFER_POOL_CAST (pwsink->stream->pool));
  gst_buffer_pool_config_get_params (config, NULL, &size, &min_buffers, &max_buffers);
  gst_buffer_pool_config_set_params (config, caps, size, min_buffers, max_buffers);
  if (pwsink->is_video) {
    gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_META);
#ifdef HAVE_GSTREAMER_SHM_ALLOCATOR
    gst_buffer_pool_config_add_option (config, GST_BUFFER_POOL_OPTION_VIDEO_ALIGNMENT);
#endif
  }
  gst_buffer_pool_set_config (GST_BUFFER_POOL_CAST (pwsink->stream->pool), config);

  pw_thread_loop_unlock (pwsink->stream->core->loop);

  pwsink->negotiated = TRUE;

  return TRUE;

start_error:
  {
    GST_ERROR_OBJECT (pwsink, "could not start stream: %s", error);
    pw_thread_loop_unlock (pwsink->stream->core->loop);
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

  if (buffer->pool != GST_BUFFER_POOL_CAST (pwsink->stream->pool) &&
      !gst_buffer_pool_is_active (GST_BUFFER_POOL_CAST (pwsink->stream->pool))) {
    GstStructure *config;
    GstCaps *caps;
    guint size, min_buffers, max_buffers;

    config = gst_buffer_pool_get_config (GST_BUFFER_POOL_CAST (pwsink->stream->pool));
    gst_buffer_pool_config_get_params (config, &caps, &size, &min_buffers, &max_buffers);

    if (size == 0) {
      gsize maxsize;
      gst_buffer_get_sizes (buffer, NULL, &maxsize);
      size = maxsize;
    }

    gst_buffer_pool_config_set_params (config, caps, size, min_buffers, max_buffers);
    gst_buffer_pool_set_config (GST_BUFFER_POOL_CAST (pwsink->stream->pool), config);

    gst_buffer_pool_set_active (GST_BUFFER_POOL_CAST (pwsink->stream->pool), TRUE);
  }

  pw_thread_loop_lock (pwsink->stream->core->loop);
  if (pw_stream_get_state (pwsink->stream->pwstream, &error) != PW_STREAM_STATE_STREAMING)
    goto done_unlock;

  if (buffer->pool != GST_BUFFER_POOL_CAST (pwsink->stream->pool)) {
    gsize offset = 0;
    gsize buf_size = gst_buffer_get_size (buffer);

    GST_TRACE_OBJECT(pwsink, "Buffer is not from pipewirepool, copying into our pool");

    /* For some streams, the buffer size is changed and may exceed the acquired
     * buffer size which is acquired from the pool of pipewiresink. Need split
     * the buffer and send them in turn for this case */
    while (buf_size) {
      GstBuffer *b = NULL;
      GstMapInfo info = { 0, };
      GstBufferPoolAcquireParams params = { 0, };

      pw_thread_loop_unlock (pwsink->stream->core->loop);

      params.flags = GST_BUFFER_POOL_ACQUIRE_FLAG_LAST;
      res = gst_buffer_pool_acquire_buffer (GST_BUFFER_POOL_CAST (pwsink->stream->pool),
          &b, &params);
      if (res == GST_FLOW_CUSTOM_ERROR_1) {
        res = gst_base_sink_wait_preroll (bsink);
        if (res != GST_FLOW_OK)
          goto done;
        continue;
      }
      if (res != GST_FLOW_OK)
        goto done;

      if (pwsink->is_video) {
        GstVideoFrame src, dst;
        gboolean copied = FALSE;
        buf_size = 0; // to break from the loop

        /* splitting of buffers in the case of video might break the frame layout
         * and that seems to be causing issues while retrieving the buffers on the receiver
         * side. Hence use the video_frame_map to copy the buffer of bigger size into the
         * pipewirepool's buffer */

        if (!gst_video_frame_map (&dst, &pwsink->stream->pool->video_info, b, GST_MAP_WRITE)) {
          GST_ERROR_OBJECT(pwsink, "Failed to map dest buffer");
          return GST_FLOW_ERROR;
        }

        if (!gst_video_frame_map (&src, &pwsink->stream->pool->video_info, buffer, GST_MAP_READ)) {
          gst_video_frame_unmap (&dst);
          GST_ERROR_OBJECT(pwsink, "Failed to map src buffer");
          return GST_FLOW_ERROR;
        }

        copied = gst_video_frame_copy (&dst, &src);

        gst_video_frame_unmap (&src);
        gst_video_frame_unmap (&dst);

        if (!copied) {
          GST_ERROR_OBJECT(pwsink, "Failed to copy the frame");
          return GST_FLOW_ERROR;
        }
      } else {
        gst_buffer_map (b, &info, GST_MAP_WRITE);
        gsize extract_size = (buf_size <= info.maxsize) ? buf_size: info.maxsize;
        gst_buffer_extract (buffer, offset, info.data, info.maxsize);
        gst_buffer_unmap (b, &info);
        gst_buffer_resize (b, 0, extract_size);
        gst_buffer_copy_into(b, buffer, GST_BUFFER_COPY_METADATA, 0, -1);
        buf_size -= extract_size;
        offset += extract_size;
      }

      pw_thread_loop_lock (pwsink->stream->core->loop);
      if (pw_stream_get_state (pwsink->stream->pwstream, &error) != PW_STREAM_STATE_STREAMING) {
        gst_buffer_unref (b);
        goto done_unlock;
      }

      do_send_buffer (pwsink, b);
      gst_buffer_unref (b);

      if (pw_stream_is_driving (pwsink->stream->pwstream))
        pw_stream_trigger_process (pwsink->stream->pwstream);
    }
  } else {
    GST_TRACE_OBJECT(pwsink, "Buffer is from pipewirepool");

    do_send_buffer (pwsink, buffer);

    if (pw_stream_is_driving (pwsink->stream->pwstream))
      pw_stream_trigger_process (pwsink->stream->pwstream);
  }

done_unlock:
  pw_thread_loop_unlock (pwsink->stream->core->loop);
done:
  return res;

not_negotiated:
  {
    return GST_FLOW_NOT_NEGOTIATED;
  }
}

static const struct pw_stream_events stream_events = {
        PW_VERSION_STREAM_EVENTS,
        .state_changed = on_state_changed,
        .param_changed = on_param_changed,
        .add_buffer = on_add_buffer,
        .remove_buffer = on_remove_buffer,
        .process = on_process,
};

static GstStateChangeReturn
gst_pipewire_sink_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstPipeWireSink *this = GST_PIPEWIRE_SINK_CAST (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_pipewire_stream_open (this->stream, &stream_events))
        goto open_failed;
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      /* If we are a driver, we shouldn't try to also provide the clock, as we
       * _are_ the clock for the graph. For that case, we rely on the pipeline
       * clock to drive the pipeline (and thus the graph). */
      if (this->mode == GST_PIPEWIRE_SINK_MODE_PROVIDE)
        GST_OBJECT_FLAG_UNSET (this, GST_ELEMENT_FLAG_PROVIDE_CLOCK);

      /* the initial stream state is active, which is needed for linking and
       * negotiation to happen and the bufferpool to be set up. We don't know
       * if we'll go to plaing, so we deactivate the stream until that
       * transition happens. This is janky, but because of how bins propagate
       * state changes one transition at a time, there may not be a better way
       * to do this. PAUSED -> READY -> PAUSED transitions, this is a noop */
      pw_thread_loop_lock (this->stream->core->loop);
      pw_stream_set_active(this->stream->pwstream, false);
      pw_thread_loop_unlock (this->stream->core->loop);
      gst_pipewire_pool_set_paused(this->stream->pool, TRUE);
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      /* stop play ASAP by corking */
      gst_pipewire_pool_set_paused(this->stream->pool, TRUE);
      pw_thread_loop_lock (this->stream->core->loop);
      pw_stream_set_active(this->stream->pwstream, false);
      pw_thread_loop_unlock (this->stream->core->loop);
      break;
    default:
      break;
  }

  ret = GST_ELEMENT_CLASS (parent_class)->change_state (element, transition);

  switch (transition) {
    case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
      /* For some cases, the param_changed event is earlier than the state switch
       * from paused state to playing state which will wait until buffer pool is ready.
       * Guarantee to finish preoll if needed to active buffer pool before uncorking and
       * starting play */
      gst_pipewire_pool_set_paused(this->stream->pool, FALSE);
      pw_thread_loop_lock (this->stream->core->loop);
      pw_stream_set_active(this->stream->pwstream, true);
      pw_thread_loop_unlock (this->stream->core->loop);
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      gst_buffer_pool_set_active(GST_BUFFER_POOL_CAST(this->stream->pool), FALSE);
      this->negotiated = FALSE;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      gst_pipewire_stream_close (this->stream);
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

static  gboolean gst_pipewire_sink_event (GstBaseSink *sink, GstEvent *event) {
  GstPipeWireSink *pw_sink = GST_PIPEWIRE_SINK(sink);
  GstState current_state = GST_ELEMENT(sink)->current_state;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_FLUSH_START:
    {
      GST_DEBUG_OBJECT (pw_sink, "flush-start");
      pw_thread_loop_lock (pw_sink->stream->core->loop);

      /* The stream would be already inactive if the sink is not PLAYING */
      if (current_state == GST_STATE_PLAYING)
        pw_stream_set_active(pw_sink->stream->pwstream, false);

      gst_buffer_pool_set_flushing(GST_BUFFER_POOL_CAST(pw_sink->stream->pool), TRUE);
      pw_stream_flush(pw_sink->stream->pwstream, false);
      pw_thread_loop_unlock (pw_sink->stream->core->loop);
      break;
    }
    case GST_EVENT_FLUSH_STOP:
    {
      GST_DEBUG_OBJECT (pw_sink, "flush-stop");
      pw_thread_loop_lock (pw_sink->stream->core->loop);

      /* The stream needs to remain inactive if the sink is not PLAYING */
      if (current_state == GST_STATE_PLAYING)
        pw_stream_set_active(pw_sink->stream->pwstream, true);

      gst_buffer_pool_set_flushing(GST_BUFFER_POOL_CAST(pw_sink->stream->pool), FALSE);
      pw_thread_loop_unlock (pw_sink->stream->core->loop);
      break;
    }
    default:
      break;
  }

  return GST_BASE_SINK_CLASS (parent_class)->event (sink, event);
}
