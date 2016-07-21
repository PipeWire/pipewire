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

#include "gsttmpfileallocator.h"


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

  if (pinossink->properties)
    gst_structure_free (pinossink->properties);
  g_hash_table_unref (pinossink->mem_ids);
  g_object_unref (pinossink->allocator);
  g_free (pinossink->path);
  g_free (pinossink->client_name);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static gboolean
gst_pinos_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstPinosSink *pinossink = GST_PINOS_SINK (bsink);

  gst_query_add_allocation_param (query, pinossink->allocator, NULL);
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
}

static void
gst_pinos_sink_init (GstPinosSink * sink)
{
  sink->allocator = gst_tmpfile_allocator_new ();
  sink->fdmanager = pinos_fd_manager_get (PINOS_FD_MANAGER_DEFAULT);
  sink->client_name = pinos_client_name();
  sink->mode = DEFAULT_PROP_MODE;

  sink->mem_ids = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL,
      (GDestroyNotify) gst_memory_unref);
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

static void
on_new_buffer (GObject    *gobject,
               gpointer    user_data)
{
  GstPinosSink *pinossink = user_data;
  PinosBuffer *pbuf;
  PinosBufferIter it;

  GST_LOG_OBJECT (pinossink, "got new buffer");
  if (pinossink->stream == NULL) {
    GST_LOG_OBJECT (pinossink, "no stream");
    return;
  }

  if (!(pbuf = pinos_stream_peek_buffer (pinossink->stream))) {
    g_warning ("failed to capture buffer");
    return;
  }

  pinos_buffer_iter_init (&it, pbuf);
  while (pinos_buffer_iter_next (&it)) {
    switch (pinos_buffer_iter_get_type (&it)) {
      case PINOS_PACKET_TYPE_REUSE_MEM:
      {
        PinosPacketReuseMem p;

        if (!pinos_buffer_iter_parse_reuse_mem (&it, &p))
          continue;

        GST_LOG ("mem index %d is reused", p.id);
        g_hash_table_remove (pinossink->mem_ids, GINT_TO_POINTER (p.id));
        break;
      }
      case PINOS_PACKET_TYPE_REFRESH_REQUEST:
      {
        PinosPacketRefreshRequest p;

        if (!pinos_buffer_iter_parse_refresh_request (&it, &p))
          continue;

        GST_LOG ("refresh request");
        gst_pad_push_event (GST_BASE_SINK_PAD (pinossink),
            gst_video_event_new_upstream_force_key_unit (p.pts,
            p.request_type == 1, 0));
        break;
      }
      default:
        break;
    }
  }
  pinos_buffer_iter_end (&it);
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
  GST_DEBUG ("got stream state %d", state);

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

static gboolean
gst_pinos_sink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstPinosSink *pinossink;
  gchar *str;
  GBytes *format;
  PinosStreamState state;
  gboolean res = FALSE;

  pinossink = GST_PINOS_SINK (bsink);

  str = gst_caps_to_string (caps);
  format = g_bytes_new_take (str, strlen (str) + 1);

  pinos_main_loop_lock (pinossink->loop);
  state = pinos_stream_get_state (pinossink->stream);

  if (state == PINOS_STREAM_STATE_ERROR)
    goto start_error;

  if (state == PINOS_STREAM_STATE_UNCONNECTED) {
    PinosStreamFlags flags = 0;

    if (pinossink->mode != GST_PINOS_SINK_MODE_PROVIDE)
      flags |= PINOS_STREAM_FLAG_AUTOCONNECT;

    pinos_stream_connect (pinossink->stream,
                          PINOS_DIRECTION_OUTPUT,
                          pinossink->path,
                          flags,
                          g_bytes_ref (format));

    while (TRUE) {
      state = pinos_stream_get_state (pinossink->stream);

      if (state == PINOS_STREAM_STATE_READY)
        break;

      if (state == PINOS_STREAM_STATE_ERROR)
        goto start_error;

      pinos_main_loop_wait (pinossink->loop);
    }
  }

  if (state != PINOS_STREAM_STATE_STREAMING) {
    res = pinos_stream_start (pinossink->stream,
                             g_bytes_ref (format),
                             PINOS_STREAM_MODE_BUFFER);

    while (TRUE) {
      state = pinos_stream_get_state (pinossink->stream);

      if (state == PINOS_STREAM_STATE_STREAMING)
        break;

      if (state == PINOS_STREAM_STATE_ERROR)
        goto start_error;

      pinos_main_loop_wait (pinossink->loop);
    }
  }
  {
    PinosBufferBuilder builder;
    PinosPacketFormatChange change;
    PinosBuffer pbuf;

    pinos_stream_buffer_builder_init (pinossink->stream, &builder);

    change.id = 1;
    change.format = g_bytes_get_data (format, NULL);
    pinos_buffer_builder_add_format_change (&builder, &change);
    pinos_buffer_builder_end (&builder, &pbuf);

    g_debug ("sending format");
    res = pinos_stream_send_buffer (pinossink->stream, &pbuf);
    pinos_buffer_unref (&pbuf);
  }

  pinos_main_loop_unlock (pinossink->loop);
  g_bytes_unref (format);

  pinossink->negotiated = res;

  return res;

start_error:
  {
    GST_ERROR ("could not start stream");
    pinos_main_loop_unlock (pinossink->loop);
    g_bytes_unref (format);
    return FALSE;
  }
}

static GstFlowReturn
gst_pinos_sink_render (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstPinosSink *pinossink;
  PinosBuffer pbuf;
  PinosBufferBuilder builder;
  GstMemory *mem = NULL;
  GstClockTime pts, dts, base;
  PinosPacketHeader hdr;
  PinosPacketAddMem am;
  PinosPacketProcessMem p;
  PinosPacketRemoveMem rm;
  gsize size;
  gboolean tmpfile, res;

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

  if (gst_buffer_n_memory (buffer) == 1
      && gst_is_fd_memory (gst_buffer_peek_memory (buffer, 0))) {
    mem = gst_buffer_get_memory (buffer, 0);
    tmpfile = gst_is_tmpfile_memory (mem);
  } else {
    GstMapInfo minfo;
    GstAllocationParams params = {0, 0, 0, 0, { NULL, }};

    GST_INFO_OBJECT (bsink, "Buffer cannot be payloaded without copying");

    mem = gst_allocator_alloc (pinossink->allocator, size, &params);
    if (!gst_memory_map (mem, &minfo, GST_MAP_WRITE))
      goto map_error;
    gst_buffer_extract (buffer, 0, minfo.data, size);
    gst_memory_unmap (mem, &minfo);
    tmpfile = TRUE;
  }

  pinos_main_loop_lock (pinossink->loop);
  if (pinos_stream_get_state (pinossink->stream) != PINOS_STREAM_STATE_STREAMING)
    goto streaming_error;

  pinos_stream_buffer_builder_init (pinossink->stream, &builder);
  pinos_buffer_builder_add_header (&builder, &hdr);

  am.id = pinos_fd_manager_get_id (pinossink->fdmanager);
  am.fd_index = pinos_buffer_builder_add_fd (&builder, gst_fd_memory_get_fd (mem));
  am.offset = 0;
  am.size = mem->size;
  p.id = am.id;
  p.offset = mem->offset;
  p.size = mem->size;
  rm.id = am.id;
  pinos_buffer_builder_add_add_mem (&builder, &am);
  pinos_buffer_builder_add_process_mem (&builder, &p);
  pinos_buffer_builder_add_remove_mem (&builder, &rm);
  pinos_buffer_builder_end (&builder, &pbuf);

  GST_LOG ("sending fd mem %d %d", am.id, am.fd_index);

  res = pinos_stream_send_buffer (pinossink->stream, &pbuf);
  pinos_buffer_steal_fds (&pbuf, NULL);
  pinos_buffer_unref (&pbuf);
  pinos_main_loop_unlock (pinossink->loop);

  if (res && !tmpfile) {
    /* keep the memory around until we get the reuse mem message */
    g_hash_table_insert (pinossink->mem_ids, GINT_TO_POINTER (p.id), mem);
  } else
    gst_memory_unref (mem);

  return GST_FLOW_OK;

not_negotiated:
  {
    return GST_FLOW_NOT_NEGOTIATED;
  }
map_error:
  {
    GST_ELEMENT_ERROR (pinossink, RESOURCE, FAILED,
        ("failed to map buffer"), (NULL));
    gst_memory_unref (mem);
    return GST_FLOW_ERROR;
  }
streaming_error:
  {
    pinos_main_loop_unlock (pinossink->loop);
    gst_memory_unref (mem);
    return GST_FLOW_ERROR;
  }
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

  pinos_main_loop_lock (pinossink->loop);
  pinossink->stream = pinos_stream_new (pinossink->ctx, pinossink->client_name, props);
  g_signal_connect (pinossink->stream, "notify::state", (GCallback) on_stream_notify, pinossink);
  g_signal_connect (pinossink->stream, "new-buffer", (GCallback) on_new_buffer, pinossink);
  pinos_main_loop_unlock (pinossink->loop);

  return TRUE;
}

static gboolean
gst_pinos_sink_stop (GstBaseSink * basesink)
{
  GstPinosSink *pinossink = GST_PINOS_SINK (basesink);

  pinos_main_loop_lock (pinossink->loop);
  if (pinossink->stream) {
    pinos_stream_stop (pinossink->stream);
    pinos_stream_disconnect (pinossink->stream);
    g_clear_object (&pinossink->stream);
  }
  pinos_main_loop_unlock (pinossink->loop);

  pinossink->negotiated = FALSE;

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
  GST_DEBUG ("got context state %d", state);

  switch (state) {
    case PINOS_CONTEXT_STATE_UNCONNECTED:
    case PINOS_CONTEXT_STATE_CONNECTING:
    case PINOS_CONTEXT_STATE_CONNECTED:
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
  GST_DEBUG ("context %p", pinossink->context);

  pinossink->loop = pinos_main_loop_new (pinossink->context, "pinos-sink-loop");
  if (!pinos_main_loop_start (pinossink->loop, &error))
    goto mainloop_error;

  pinos_main_loop_lock (pinossink->loop);
  pinossink->ctx = pinos_context_new (pinossink->context, g_get_application_name (), NULL);
  g_signal_connect (pinossink->ctx, "notify::state", (GCallback) on_context_notify, pinossink);

  pinos_context_connect(pinossink->ctx, PINOS_CONTEXT_FLAGS_NONE);

  while (TRUE) {
    PinosContextState state = pinos_context_get_state (pinossink->ctx);

    if (state == PINOS_CONTEXT_STATE_CONNECTED)
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
      g_hash_table_remove_all (this->mem_ids);
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      g_hash_table_remove_all (this->mem_ids);
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
