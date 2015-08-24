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

#include "gsttmpfileallocator.h"


GST_DEBUG_CATEGORY_STATIC (pinos_sink_debug);
#define GST_CAT_DEFAULT pinos_sink_debug

enum
{
  PROP_0,
  PROP_CLIENT_NAME
};


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

static GstCaps *gst_pinos_sink_getcaps (GstBaseSink * bsink, GstCaps * filter);
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

  g_object_unref (pinossink->allocator);
  g_free (pinossink->client_name);

  G_OBJECT_CLASS (parent_class)->finalize (object);
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
                                   PROP_CLIENT_NAME,
                                   g_param_spec_string ("client-name",
                                                        "Client Name",
                                                        "The client name to use (NULL = default)",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_STATIC_STRINGS));

  gstelement_class->change_state = gst_pinos_sink_change_state;

  gst_element_class_set_static_metadata (gstelement_class,
      "Pinos sink", "Sink/Video",
      "Send video to pinos", "Wim Taymans <wim.taymans@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pinos_sink_template));

  gstbasesink_class->get_caps = gst_pinos_sink_getcaps;
  gstbasesink_class->set_caps = gst_pinos_sink_setcaps;
  gstbasesink_class->fixate = gst_pinos_sink_sink_fixate;
  gstbasesink_class->start = gst_pinos_sink_start;
  gstbasesink_class->stop = gst_pinos_sink_stop;
  gstbasesink_class->render = gst_pinos_sink_render;

  GST_DEBUG_CATEGORY_INIT (pinos_sink_debug, "pinossink", 0,
      "Pinos Sink");
}

static void
gst_pinos_sink_init (GstPinosSink * sink)
{
  sink->allocator = gst_tmpfile_allocator_new ();
  sink->client_name = pinos_client_name();
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
    case PROP_CLIENT_NAME:
      g_free (pinossink->client_name);
      pinossink->client_name = g_value_dup_string (value);
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
    case PROP_CLIENT_NAME:
      g_value_set_string (value, pinossink->client_name);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
on_new_buffer (GObject    *gobject,
               gpointer    user_data)
{
  GstPinosSink *pinossink = user_data;

  pinos_main_loop_signal (pinossink->loop, FALSE);
}

static void
on_stream_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  PinosStreamState state;
  PinosStream *stream = PINOS_STREAM (gobject);
  GstPinosSink *pinossink = user_data;

  state = pinos_stream_get_state (stream);
  GST_DEBUG ("got stream state %d\n", state);

  switch (state) {
    case PINOS_STREAM_STATE_UNCONNECTED:
    case PINOS_STREAM_STATE_CONNECTING:
    case PINOS_STREAM_STATE_STARTING:
    case PINOS_STREAM_STATE_STREAMING:
    case PINOS_STREAM_STATE_READY:
      break;
    case PINOS_STREAM_STATE_ERROR:
      GST_ELEMENT_ERROR (pinossink, RESOURCE, FAILED,
          ("stream error: %s",
            pinos_stream_get_error (stream)->message), (NULL));
      break;
  }
  pinos_main_loop_signal (pinossink->loop, FALSE);
}

static GstCaps *
gst_pinos_sink_getcaps (GstBaseSink * bsink, GstCaps * filter)
{
  return GST_BASE_SINK_CLASS (parent_class)->get_caps (bsink, filter);
}

static gboolean
gst_pinos_sink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstPinosSink *pinossink;
  gchar *str;
  GBytes *format;

  pinossink = GST_PINOS_SINK (bsink);

  str = gst_caps_to_string (caps);
  format = g_bytes_new_take (str, strlen (str) + 1);

  pinos_main_loop_lock (pinossink->loop);
  pinossink->stream = pinos_stream_new (pinossink->ctx, "test", NULL);
  g_signal_connect (pinossink->stream, "notify::state", (GCallback) on_stream_notify, pinossink);
  g_signal_connect (pinossink->stream, "new-buffer", (GCallback) on_new_buffer, pinossink);

  pinos_stream_connect_provide (pinossink->stream, 0, format);

  while (TRUE) {
    PinosStreamState state = pinos_stream_get_state (pinossink->stream);

    if (state == PINOS_STREAM_STATE_READY)
      break;

    if (state == PINOS_STREAM_STATE_ERROR)
      goto connect_error;

    pinos_main_loop_wait (pinossink->loop);
  }

  pinos_stream_start (pinossink->stream, format, PINOS_STREAM_MODE_BUFFER);

  while (TRUE) {
    PinosStreamState state = pinos_stream_get_state (pinossink->stream);

    if (state == PINOS_STREAM_STATE_STREAMING)
      break;

    if (state == PINOS_STREAM_STATE_ERROR)
      goto connect_error;

    pinos_main_loop_wait (pinossink->loop);
  }
  pinos_main_loop_unlock (pinossink->loop);

  pinossink->negotiated = TRUE;

  return TRUE;

connect_error:
  {
    pinos_main_loop_unlock (pinossink->loop);
    return FALSE;
  }
}

static GstFlowReturn
gst_pinos_sink_render (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstPinosSink *pinossink;
  PinosBuffer pbuf;
  PinosPacketBuilder builder;
  GstMemory *mem = NULL;
  GstClockTime pts, dts, base;
  PinosBufferHeader hdr;
  gsize size;

  pinossink = GST_PINOS_SINK (bsink);

  if (!pinossink->negotiated)
    goto not_negotiated;

  base = GST_ELEMENT_CAST (bsink)->base_time;

  pts = GST_BUFFER_PTS (buffer);
  dts = GST_BUFFER_DTS (buffer);
  if (!GST_CLOCK_TIME_IS_VALID (pts))
    pts = dts;
  else if (!GST_CLOCK_TIME_IS_VALID (dts))
    dts = pts;

  hdr.flags = 0;
  hdr.seq = GST_BUFFER_OFFSET (buffer);
  hdr.pts = GST_CLOCK_TIME_IS_VALID (pts) ? pts + base : base;
  hdr.dts_offset = GST_CLOCK_TIME_IS_VALID (dts) && GST_CLOCK_TIME_IS_VALID (pts) ? pts - dts : 0;

  size = gst_buffer_get_size (buffer);

  pinos_packet_builder_init (&builder, &hdr);

  if (gst_buffer_n_memory (buffer) == 1
      && gst_is_fd_memory (gst_buffer_peek_memory (buffer, 0))) {
    mem = gst_buffer_get_memory (buffer, 0);
  } else {
    GstMapInfo minfo;
    GstAllocationParams params = {0, 0, 0, 0, { NULL, }};

    GST_INFO_OBJECT (bsink, "Buffer cannot be payloaded without copying");

    mem = gst_allocator_alloc (pinossink->allocator, size, &params);
    if (!gst_memory_map (mem, &minfo, GST_MAP_WRITE))
      goto map_error;
    gst_buffer_extract (buffer, 0, minfo.data, size);
    gst_memory_unmap (mem, &minfo);
  }

  pinos_packet_builder_add_fd_payload (&builder, 0, size, gst_fd_memory_get_fd (mem), NULL);
  gst_memory_unref (mem);

  pinos_packet_builder_end (&builder, &pbuf);

  pinos_main_loop_lock (pinossink->loop);
  if (pinos_stream_get_state (pinossink->stream) != PINOS_STREAM_STATE_STREAMING)
    goto streaming_error;
  pinos_stream_provide_buffer (pinossink->stream, &pbuf);
  pinos_main_loop_unlock (pinossink->loop);

  return GST_FLOW_OK;

not_negotiated:
  {
    return GST_FLOW_NOT_NEGOTIATED;
  }
map_error:
  {
    return GST_FLOW_ERROR;
  }
streaming_error:
  {
    pinos_main_loop_unlock (pinossink->loop);
    return GST_FLOW_ERROR;
  }
}

static gboolean
gst_pinos_sink_start (GstBaseSink * basesink)
{
  GstPinosSink *sink = GST_PINOS_SINK (basesink);

  sink->negotiated = FALSE;

  return TRUE;
}

static gboolean
gst_pinos_sink_stop (GstBaseSink * basesink)
{
  GstPinosSink *sink = GST_PINOS_SINK (basesink);

  sink->negotiated = FALSE;

  return TRUE;
}

static void
on_context_notify (GObject    *gobject,
                   GParamSpec *pspec,
                   gpointer    user_data)
{
  GstPinosSink *pinossink = user_data;
  PinosContext *ctx = PINOS_CONTEXT (gobject);
  PinosContextState state;

  state = pinos_context_get_state (ctx);
  GST_DEBUG ("got context state %d\n", state);

  switch (state) {
    case PINOS_CONTEXT_STATE_UNCONNECTED:
    case PINOS_CONTEXT_STATE_CONNECTING:
    case PINOS_CONTEXT_STATE_REGISTERING:
    case PINOS_CONTEXT_STATE_READY:
      break;
    case PINOS_CONTEXT_STATE_ERROR:
      GST_ELEMENT_ERROR (pinossink, RESOURCE, FAILED,
          ("context error: %s",
            pinos_context_get_error (pinossink->ctx)->message), (NULL));
      break;
  }
  pinos_main_loop_signal (pinossink->loop, FALSE);
}

static gboolean
gst_pinos_sink_open (GstPinosSink * pinossink)
{
  GError *error = NULL;

  pinossink->context = g_main_context_new ();
  GST_DEBUG ("context %p\n", pinossink->context);

  pinossink->loop = pinos_main_loop_new (pinossink->context, "pinos-sink-loop");
  if (!pinos_main_loop_start (pinossink->loop, &error))
    goto mainloop_error;

  pinos_main_loop_lock (pinossink->loop);
  pinossink->ctx = pinos_context_new (pinossink->context, "test-client", NULL);
  g_signal_connect (pinossink->ctx, "notify::state", (GCallback) on_context_notify, pinossink);

  pinos_context_connect(pinossink->ctx, PINOS_CONTEXT_FLAGS_NONE);

  while (TRUE) {
    PinosContextState state = pinos_context_get_state (pinossink->ctx);

    if (state == PINOS_CONTEXT_STATE_READY)
      break;

    if (state == PINOS_CONTEXT_STATE_ERROR)
      goto connect_error;

    pinos_main_loop_wait (pinossink->loop);
  }
  pinos_main_loop_unlock (pinossink->loop);

  return TRUE;

  /* ERRORS */
mainloop_error:
  {
    GST_ELEMENT_ERROR (pinossink, RESOURCE, FAILED,
        ("Failed to start mainloop: %s", error->message), (NULL));
    return FALSE;
  }
connect_error:
  {
    pinos_main_loop_unlock (pinossink->loop);
    return FALSE;
  }
}

static gboolean
gst_pinos_sink_close (GstPinosSink * pinossink)
{
  pinos_main_loop_lock (pinossink->loop);
  if (pinossink->stream) {
    pinos_stream_disconnect (pinossink->stream);
  }
  if (pinossink->ctx) {
    pinos_context_disconnect (pinossink->ctx);

    while (TRUE) {
      PinosContextState state = pinos_context_get_state (pinossink->ctx);

      if (state == PINOS_CONTEXT_STATE_UNCONNECTED)
        break;

      if (state == PINOS_CONTEXT_STATE_ERROR)
        break;

      pinos_main_loop_wait (pinossink->loop);
    }
  }
  pinos_main_loop_unlock (pinossink->loop);

  pinos_main_loop_stop (pinossink->loop);
  g_clear_object (&pinossink->loop);
  g_clear_object (&pinossink->stream);
  g_clear_object (&pinossink->ctx);
  g_main_context_unref (pinossink->context);

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
