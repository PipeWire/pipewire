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
 * SECTION:element-pulsevideosink
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v pulsevideosink ! ximagesink
 * ]| Shows pulsevideo output in an X window.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "gstpvsink.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gunixfdmessage.h>
#include <gst/allocators/gstfdmemory.h>

#include "gsttmpfileallocator.h"


GST_DEBUG_CATEGORY_STATIC (pulsevideo_sink_debug);
#define GST_CAT_DEFAULT pulsevideo_sink_debug

enum
{
  PROP_0,
  PROP_LAST
};


#define PVS_VIDEO_CAPS GST_VIDEO_CAPS_MAKE (GST_VIDEO_FORMATS_ALL)

static GstStaticPadTemplate gst_pulsevideo_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS (PVS_VIDEO_CAPS)
    );

#define gst_pulsevideo_sink_parent_class parent_class
G_DEFINE_TYPE (GstPulsevideoSink, gst_pulsevideo_sink, GST_TYPE_BASE_SINK);

static void gst_pulsevideo_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_pulsevideo_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static GstStateChangeReturn
gst_pulsevideo_sink_change_state (GstElement * element, GstStateChange transition);

static GstCaps *gst_pulsevideo_sink_getcaps (GstBaseSink * bsink, GstCaps * filter);
static gboolean gst_pulsevideo_sink_setcaps (GstBaseSink * bsink, GstCaps * caps);
static GstCaps *gst_pulsevideo_sink_sink_fixate (GstBaseSink * bsink,
    GstCaps * caps);

static gboolean gst_pulsevideo_sink_propose_allocation (GstBaseSink * bsink,
    GstQuery * query);
static GstFlowReturn gst_pulsevideo_sink_render (GstBaseSink * psink,
    GstBuffer * buffer);
static gboolean gst_pulsevideo_sink_start (GstBaseSink * basesink);
static gboolean gst_pulsevideo_sink_stop (GstBaseSink * basesink);

static void
gst_pulsevideo_sink_class_init (GstPulsevideoSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;

  gobject_class->set_property = gst_pulsevideo_sink_set_property;
  gobject_class->get_property = gst_pulsevideo_sink_get_property;

  gstelement_class->change_state = gst_pulsevideo_sink_change_state;

  gst_element_class_set_static_metadata (gstelement_class,
      "Pulsevideo sink", "Sink/Video",
      "Send video to pulsevideo", "Wim Taymans <wim.taymans@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pulsevideo_sink_template));

  gstbasesink_class->get_caps = gst_pulsevideo_sink_getcaps;
  gstbasesink_class->set_caps = gst_pulsevideo_sink_setcaps;
  gstbasesink_class->fixate = gst_pulsevideo_sink_sink_fixate;
  gstbasesink_class->start = gst_pulsevideo_sink_start;
  gstbasesink_class->stop = gst_pulsevideo_sink_stop;
  gstbasesink_class->render = gst_pulsevideo_sink_render;
  gstbasesink_class->propose_allocation = gst_pulsevideo_sink_propose_allocation;

  GST_DEBUG_CATEGORY_INIT (pulsevideo_sink_debug, "pulsevideosink", 0,
      "Pulsevideo Sink");
}

static void
gst_pulsevideo_sink_init (GstPulsevideoSink * sink)
{
  sink->allocator = gst_tmpfile_allocator_new ();
  g_mutex_init (&sink->lock);
  g_cond_init (&sink->cond);
}

static GstCaps *
gst_pulsevideo_sink_sink_fixate (GstBaseSink * bsink, GstCaps * caps)
{
  GstStructure *structure;

  caps = gst_caps_make_writable (caps);

  structure = gst_caps_get_structure (caps, 0);

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

  caps = GST_BASE_SINK_CLASS (parent_class)->fixate (bsink, caps);

  return caps;
}

static void
gst_pulsevideo_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pulsevideo_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_pulsevideo_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstPulsevideoSink *pvsink;
  GstBufferPool *pool;
  gboolean update;
  guint size, min, max;
  GstStructure *config;
  GstCaps *caps = NULL;

  pvsink = GST_PULSEVIDEO_SINK (bsink);

  if (gst_query_get_n_allocation_pools (query) > 0) {
    gst_query_parse_nth_allocation_pool (query, 0, &pool, &size, &min, &max);

    /* adjust size */
    size = MAX (size, pvsink->info.size);
    update = TRUE;
  } else {
    pool = NULL;
    size = pvsink->info.size;
    min = max = 0;
    update = FALSE;
  }

  /* no downstream pool, make our own */
  if (pool == NULL) {
    pool = gst_video_buffer_pool_new ();
  }

  config = gst_buffer_pool_get_config (pool);

  gst_query_parse_allocation (query, &caps, NULL);
  if (caps)
    gst_buffer_pool_config_set_params (config, caps, size, min, max);

  if (gst_query_find_allocation_meta (query, GST_VIDEO_META_API_TYPE, NULL)) {
    gst_buffer_pool_config_add_option (config,
        GST_BUFFER_POOL_OPTION_VIDEO_META);
  }
  gst_buffer_pool_set_config (pool, config);

  if (update)
    gst_query_set_nth_allocation_pool (query, 0, pool, size, min, max);
  else
    gst_query_add_allocation_pool (query, pool, size, min, max);

  if (pool)
    gst_object_unref (pool);

  return GST_BASE_SINK_CLASS (parent_class)->propose_allocation (bsink, query);
}

static void
on_new_buffer (GObject    *gobject,
               gpointer    user_data)
{
  GstPulsevideoSink *pvsink = user_data;

  g_mutex_lock (&pvsink->lock);
  g_cond_signal (&pvsink->cond);
  g_mutex_unlock (&pvsink->lock);
}

static void
on_stream_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  PvStreamState state;
  GstPulsevideoSink *pvsink = user_data;

  g_mutex_lock (&pvsink->lock);
  state = pv_stream_get_state (pvsink->stream);
  g_print ("got stream state %d\n", state);
  if (state == PV_STREAM_STATE_ERROR) {
    GST_ELEMENT_ERROR (pvsink, RESOURCE, FAILED,
        ("Failed to connect stream: %s",
          pv_stream_get_error (pvsink->stream)->message), (NULL));
  }
  g_cond_broadcast (&pvsink->cond);
  g_mutex_unlock (&pvsink->lock);
}

static GstCaps *
gst_pulsevideo_sink_getcaps (GstBaseSink * bsink, GstCaps * filter)
{
  return GST_BASE_SINK_CLASS (parent_class)->get_caps (bsink, filter);

}

static gboolean
gst_pulsevideo_sink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  const GstStructure *structure;
  GstPulsevideoSink *pvsink;
  GstVideoInfo info;
  GVariantBuilder builder;

  pvsink = GST_PULSEVIDEO_SINK (bsink);

  structure = gst_caps_get_structure (caps, 0);

  if (gst_structure_has_name (structure, "video/x-raw")) {
    /* we can use the parsing code */
    if (!gst_video_info_from_caps (&info, caps))
      goto parse_failed;

  } else {
    goto unsupported_caps;
  }

  /* looks ok here */
  pvsink->info = info;

  pvsink->stream = pv_stream_new (pvsink->ctx, "test", NULL);
  g_signal_connect (pvsink->stream, "notify::state", (GCallback) on_stream_notify, pvsink);
  g_signal_connect (pvsink->stream, "new-buffer", (GCallback) on_new_buffer, pvsink);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&builder, "{sv}", "format.encoding", g_variant_new_string ("video/x-raw"));
  g_variant_builder_add (&builder, "{sv}", "format.format",
      g_variant_new_string (gst_video_format_to_string (info.finfo->format)));
  g_variant_builder_add (&builder, "{sv}", "format.width", g_variant_new_int32 (info.width));
  g_variant_builder_add (&builder, "{sv}", "format.height", g_variant_new_int32 (info.height));
  g_variant_builder_add (&builder, "{sv}", "format.views", g_variant_new_int32 (info.views));
//  g_variant_builder_add (&builder, "{sv}", "format.chroma-site",
//      g_variant_new_string (gst_video_chroma_to_string (info.chroma_site)));
//  g_variant_builder_add (&builder, "{sv}", "format.colorimetry",
//      g_variant_new_take_string (gst_video_colorimetry_to_string (&info.colorimetry)));
//  g_variant_builder_add (&builder, "{sv}", "format.interlace-mode",
//      g_variant_new_string (gst_video_interlace_mode_to_string (info.interlace_mode)));

  pv_stream_connect_provide (pvsink->stream, 0, g_variant_builder_end (&builder));

  g_mutex_lock (&pvsink->lock);
  while (TRUE) {
    PvStreamState state = pv_stream_get_state (pvsink->stream);

    if (state == PV_STREAM_STATE_READY)
      break;

    if (state == PV_STREAM_STATE_ERROR)
      goto connect_error;

    g_cond_wait (&pvsink->cond, &pvsink->lock);
  }
  g_mutex_unlock (&pvsink->lock);

  pv_stream_start (pvsink->stream, PV_STREAM_MODE_BUFFER);

  GST_DEBUG_OBJECT (pvsink, "size %dx%d, %d/%d fps",
      info.width, info.height, info.fps_n, info.fps_d);

  return TRUE;

  /* ERRORS */
parse_failed:
  {
    GST_DEBUG_OBJECT (bsink, "failed to parse caps");
    return FALSE;
  }
unsupported_caps:
  {
    GST_DEBUG_OBJECT (bsink, "unsupported caps: %" GST_PTR_FORMAT, caps);
    return FALSE;
  }
connect_error:
  {
    g_mutex_unlock (&pvsink->lock);
    return FALSE;
  }
}

static GstFlowReturn
gst_pulsevideo_sink_render (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstPulsevideoSink *pvsink;
  PvBufferInfo info;
  GSocketControlMessage *mesg;
  GstMemory *mem = NULL;

  pvsink = GST_PULSEVIDEO_SINK (bsink);

  if (G_UNLIKELY (GST_VIDEO_INFO_FORMAT (&pvsink->info) ==
          GST_VIDEO_FORMAT_UNKNOWN))
    goto not_negotiated;

  info.flags = 0;
  info.seq = 0;
  info.pts = GST_BUFFER_TIMESTAMP (buffer);
  info.dts_offset = 0;
  info.offset = 0;
  info.size = gst_buffer_get_size (buffer);

  mesg = g_unix_fd_message_new ();
  if (gst_buffer_n_memory (buffer) == 1
      && gst_is_fd_memory (gst_buffer_peek_memory (buffer, 0))) {
    mem = gst_buffer_get_memory (buffer, 0);
  } else {
    GstMapInfo minfo;
    GstAllocationParams params = {0, 0, 0, 0, { NULL, }};

    GST_INFO_OBJECT (bsink, "Buffer cannot be payloaded without copying");

    mem = gst_allocator_alloc (pvsink->allocator, info.size, &params);
    if (!gst_memory_map (mem, &minfo, GST_MAP_WRITE))
      goto error;
    gst_buffer_extract (buffer, 0, minfo.data, info.size);
    gst_memory_unmap (mem, &minfo);
  }
  g_unix_fd_message_append_fd ((GUnixFDMessage*)mesg, gst_fd_memory_get_fd (mem), NULL);
  info.message = mesg;

  g_mutex_lock (&pvsink->lock);
  pv_stream_provide_buffer (pvsink->stream, &info);
  g_mutex_unlock (&pvsink->lock);

  return GST_FLOW_OK;

not_negotiated:
  {
    return GST_FLOW_NOT_NEGOTIATED;
  }
error:
  {
    return GST_FLOW_ERROR;
  }
}

static gboolean
gst_pulsevideo_sink_start (GstBaseSink * basesink)
{
  GstPulsevideoSink *sink = GST_PULSEVIDEO_SINK (basesink);

  gst_video_info_init (&sink->info);

  return TRUE;
}

static gboolean
gst_pulsevideo_sink_stop (GstBaseSink * basesink)
{
  return TRUE;
}

static gpointer
handle_mainloop (GstPulsevideoSink *this)
{
  g_main_context_push_thread_default (this->context);
  g_print ("run mainloop\n");
  g_main_loop_run (this->loop);
  g_print ("quit mainloop\n");
  g_main_context_pop_thread_default (this->context);

  return NULL;
}

static void
on_state_notify (GObject    *gobject,
                 GParamSpec *pspec,
                 gpointer    user_data)
{
  GstPulsevideoSink *pvsink = user_data;
  PvContextState state;

  g_mutex_lock (&pvsink->lock);
  state = pv_context_get_state (pvsink->ctx);
  g_print ("got context state %d\n", state);
  g_cond_broadcast (&pvsink->cond);
  g_mutex_unlock (&pvsink->lock);

  if (state == PV_CONTEXT_STATE_ERROR) {
    GST_ELEMENT_ERROR (pvsink, RESOURCE, FAILED,
        ("Failed to connect stream: %s",
          pv_context_get_error (pvsink->ctx)->message), (NULL));
  }
}

static gboolean
gst_pulsevideo_sink_open (GstPulsevideoSink * pvsink)
{

  pvsink->ctx = pv_context_new (pvsink->context, "test-client", NULL);
  g_signal_connect (pvsink->ctx, "notify::state", (GCallback) on_state_notify, pvsink);

  pv_context_connect(pvsink->ctx, PV_CONTEXT_FLAGS_NONE);

  g_mutex_lock (&pvsink->lock);
  while (TRUE) {
    PvContextState state = pv_context_get_state (pvsink->ctx);

    if (state == PV_CONTEXT_STATE_READY)
      break;

    if (state == PV_CONTEXT_STATE_ERROR)
      goto connect_error;

    g_cond_wait (&pvsink->cond, &pvsink->lock);
  }
  g_mutex_unlock (&pvsink->lock);

  return TRUE;

  /* ERRORS */
connect_error:
  {
    g_mutex_unlock (&pvsink->lock);
    return FALSE;
  }
}

static GstStateChangeReturn
gst_pulsevideo_sink_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstPulsevideoSink *this = GST_PULSEVIDEO_SINK_CAST (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      this->context = g_main_context_new ();
      g_print ("context %p\n", this->context);
      this->loop = g_main_loop_new (this->context, FALSE);
      this->thread = g_thread_new ("pulsevideo", (GThreadFunc) handle_mainloop, this);
      break;
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      if (!gst_pulsevideo_sink_open (this)) {
        ret = GST_STATE_CHANGE_FAILURE;
        goto exit;
      }
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
      g_main_loop_quit (this->loop);
      g_thread_join (this->thread);
      g_main_loop_unref (this->loop);
      g_main_context_unref (this->context);
      break;
    default:
      break;
  }

exit:
  return ret;
}
