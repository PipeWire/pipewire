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
 * gst-launch -v videotestsrc ! pulsevideosink
 * ]| Sends a test video source to pulsevideo
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
    GST_STATIC_CAPS_ANY
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
  GstPulsevideoSink *pvsink;
  gchar *str;
  GBytes *format;

  pvsink = GST_PULSEVIDEO_SINK (bsink);

  pvsink->stream = pv_stream_new (pvsink->ctx, "test", NULL);
  g_signal_connect (pvsink->stream, "notify::state", (GCallback) on_stream_notify, pvsink);
  g_signal_connect (pvsink->stream, "new-buffer", (GCallback) on_new_buffer, pvsink);

  str = gst_caps_to_string (caps);
  format = g_bytes_new_take (str, strlen (str) + 1);

  pv_stream_connect_provide (pvsink->stream, 0, format);

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

  pv_stream_start (pvsink->stream, format, PV_STREAM_MODE_BUFFER);

  g_mutex_lock (&pvsink->lock);
  while (TRUE) {
    PvStreamState state = pv_stream_get_state (pvsink->stream);

    if (state == PV_STREAM_STATE_STREAMING)
      break;

    if (state == PV_STREAM_STATE_ERROR)
      goto connect_error;

    g_cond_wait (&pvsink->cond, &pvsink->lock);
  }
  g_mutex_unlock (&pvsink->lock);

  pvsink->negotiated = TRUE;

  return TRUE;

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

  if (!pvsink->negotiated)
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
  gst_memory_unref (mem);
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

  sink->negotiated = FALSE;

  return TRUE;
}

static gboolean
gst_pulsevideo_sink_stop (GstBaseSink * basesink)
{
  GstPulsevideoSink *sink = GST_PULSEVIDEO_SINK (basesink);

  sink->negotiated = FALSE;

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

static gboolean
gst_pulsevideo_sink_close (GstPulsevideoSink * pvsink)
{
  if (pvsink->stream) {
    pv_stream_disconnect (pvsink->stream);
  }
  if (pvsink->ctx) {
    pv_context_disconnect(pvsink->ctx);

    g_mutex_lock (&pvsink->lock);
    while (TRUE) {
      PvContextState state = pv_context_get_state (pvsink->ctx);

      if (state == PV_CONTEXT_STATE_UNCONNECTED)
        break;

      if (state == PV_CONTEXT_STATE_ERROR)
        break;

      g_cond_wait (&pvsink->cond, &pvsink->lock);
    }
    g_mutex_unlock (&pvsink->lock);
  }

  return TRUE;
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
      if (!gst_pulsevideo_sink_open (this)) {
        ret = GST_STATE_CHANGE_FAILURE;
        goto exit;
      }
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
      gst_pulsevideo_sink_close (this);
      g_main_loop_quit (this->loop);
      g_thread_join (this->thread);
      g_main_loop_unref (this->loop);
      g_clear_object (&this->stream);
      g_clear_object (&this->ctx);
      g_main_context_unref (this->context);
      break;
    default:
      break;
  }

exit:
  return ret;
}
