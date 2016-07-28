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
 * SECTION:element-pinossrc
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v pinossrc ! videoconvert ! ximagesink
 * ]| Shows pinos output in an X window.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "gstpinossrc.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gunixfdmessage.h>
#include <gst/net/gstnetclientclock.h>
#include <gst/allocators/gstfdmemory.h>
#include <gst/video/video.h>

#include <spa/include/spa/buffer.h>


static GQuark process_mem_data_quark;

GST_DEBUG_CATEGORY_STATIC (pinos_src_debug);
#define GST_CAT_DEFAULT pinos_src_debug

enum
{
  PROP_0,
  PROP_PATH,
  PROP_CLIENT_NAME,
  PROP_STREAM_PROPERTIES,
};


#define PINOSS_VIDEO_CAPS GST_VIDEO_CAPS_MAKE (GST_VIDEO_FORMATS_ALL)

static GstStaticPadTemplate gst_pinos_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );

#define gst_pinos_src_parent_class parent_class
G_DEFINE_TYPE (GstPinosSrc, gst_pinos_src, GST_TYPE_PUSH_SRC);

static GstStateChangeReturn
gst_pinos_src_change_state (GstElement * element, GstStateChange transition);

static gboolean gst_pinos_src_negotiate (GstBaseSrc * basesrc);
static GstCaps *gst_pinos_src_src_fixate (GstBaseSrc * bsrc,
    GstCaps * caps);

static GstFlowReturn gst_pinos_src_create (GstPushSrc * psrc,
    GstBuffer ** buffer);
static gboolean gst_pinos_src_unlock (GstBaseSrc * basesrc);
static gboolean gst_pinos_src_unlock_stop (GstBaseSrc * basesrc);
static gboolean gst_pinos_src_start (GstBaseSrc * basesrc);
static gboolean gst_pinos_src_stop (GstBaseSrc * basesrc);
static gboolean gst_pinos_src_event (GstBaseSrc * src, GstEvent * event);
static gboolean gst_pinos_src_query (GstBaseSrc * src, GstQuery * query);

static void
gst_pinos_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPinosSrc *pinossrc = GST_PINOS_SRC (object);

  switch (prop_id) {
    case PROP_PATH:
      g_free (pinossrc->path);
      pinossrc->path = g_value_dup_string (value);
      break;

    case PROP_CLIENT_NAME:
      g_free (pinossrc->client_name);
      pinossrc->client_name = g_value_dup_string (value);
      break;

    case PROP_STREAM_PROPERTIES:
      if (pinossrc->properties)
        gst_structure_free (pinossrc->properties);
      pinossrc->properties =
          gst_structure_copy (gst_value_get_structure (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pinos_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPinosSrc *pinossrc = GST_PINOS_SRC (object);

  switch (prop_id) {
    case PROP_PATH:
      g_value_set_string (value, pinossrc->path);
      break;

    case PROP_CLIENT_NAME:
      g_value_set_string (value, pinossrc->client_name);
      break;

    case PROP_STREAM_PROPERTIES:
      gst_value_set_structure (value, pinossrc->properties);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstClock *
gst_pinos_src_provide_clock (GstElement * elem)
{
  GstPinosSrc *pinossrc = GST_PINOS_SRC (elem);
  GstClock *clock;

  GST_OBJECT_LOCK (pinossrc);
  if (!GST_OBJECT_FLAG_IS_SET (pinossrc, GST_ELEMENT_FLAG_PROVIDE_CLOCK))
    goto clock_disabled;

  if (pinossrc->clock)
    clock = GST_CLOCK_CAST (gst_object_ref (pinossrc->clock));
  else
    clock = NULL;
  GST_OBJECT_UNLOCK (pinossrc);

  return clock;

  /* ERRORS */
clock_disabled:
  {
    GST_DEBUG_OBJECT (pinossrc, "clock provide disabled");
    GST_OBJECT_UNLOCK (pinossrc);
    return NULL;
  }
}

static void
gst_pinos_src_finalize (GObject * object)
{
  GstPinosSrc *pinossrc = GST_PINOS_SRC (object);

  g_queue_foreach (&pinossrc->queue, (GFunc) gst_mini_object_unref, NULL);
  g_queue_clear (&pinossrc->queue);
  if (pinossrc->properties)
    gst_structure_free (pinossrc->properties);
  g_object_unref (pinossrc->fd_allocator);
  if (pinossrc->clock)
    gst_object_unref (pinossrc->clock);
  g_free (pinossrc->path);
  g_free (pinossrc->client_name);
  g_hash_table_unref (pinossrc->mem_ids);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_pinos_src_class_init (GstPinosSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSrcClass *gstbasesrc_class;
  GstPushSrcClass *gstpushsrc_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesrc_class = (GstBaseSrcClass *) klass;
  gstpushsrc_class = (GstPushSrcClass *) klass;

  gobject_class->finalize = gst_pinos_src_finalize;
  gobject_class->set_property = gst_pinos_src_set_property;
  gobject_class->get_property = gst_pinos_src_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_PATH,
                                   g_param_spec_string ("path",
                                                        "Path",
                                                        "The source path to connect to (NULL = default)",
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
                                                       "stream properties",
                                                       "list of pinos stream properties",
                                                       GST_TYPE_STRUCTURE,
                                                       G_PARAM_READWRITE |
                                                       G_PARAM_STATIC_STRINGS));

  gstelement_class->provide_clock = gst_pinos_src_provide_clock;
  gstelement_class->change_state = gst_pinos_src_change_state;

  gst_element_class_set_static_metadata (gstelement_class,
      "Pinos source", "Source/Video",
      "Uses pinos to create video", "Wim Taymans <wim.taymans@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pinos_src_template));

  gstbasesrc_class->negotiate = gst_pinos_src_negotiate;
  gstbasesrc_class->fixate = gst_pinos_src_src_fixate;
  gstbasesrc_class->unlock = gst_pinos_src_unlock;
  gstbasesrc_class->unlock_stop = gst_pinos_src_unlock_stop;
  gstbasesrc_class->start = gst_pinos_src_start;
  gstbasesrc_class->stop = gst_pinos_src_stop;
  gstbasesrc_class->event = gst_pinos_src_event;
  gstbasesrc_class->query = gst_pinos_src_query;
  gstpushsrc_class->create = gst_pinos_src_create;

  GST_DEBUG_CATEGORY_INIT (pinos_src_debug, "pinossrc", 0,
      "Pinos Source");

  process_mem_data_quark = g_quark_from_static_string ("GstPinosSrcProcessMemQuark");
}

static void
gst_pinos_src_init (GstPinosSrc * src)
{
  /* we operate in time */
  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);

  GST_OBJECT_FLAG_SET (src, GST_ELEMENT_FLAG_PROVIDE_CLOCK);

  g_queue_init (&src->queue);

  src->fd_allocator = gst_fd_allocator_new ();
  src->client_name = pinos_client_name ();
  src->mem_ids = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) gst_memory_unref);
}

static GstCaps *
gst_pinos_src_src_fixate (GstBaseSrc * bsrc, GstCaps * caps)
{
  GstStructure *structure;
  const gchar *name;

  caps = gst_caps_make_writable (caps);

  structure = gst_caps_get_structure (caps, 0);
  name = gst_structure_get_name (structure);

  if (g_str_has_prefix (name, "video/") || g_str_has_prefix (name, "image/")) {
    gst_structure_fixate_field_nearest_int (structure, "width", 320);
    gst_structure_fixate_field_nearest_int (structure, "height", 240);
    gst_structure_fixate_field_nearest_fraction (structure, "framerate", 30, 1);


    if (strcmp (name, "video/x-raw") == 0) {
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
    }
  } else if (gst_structure_has_name (structure, "audio/x-raw")) {
    gst_structure_fixate_field_string (structure, "format", "S16LE");
    gst_structure_fixate_field_nearest_int (structure, "channels", 2);
    gst_structure_fixate_field_nearest_int (structure, "rate", 44100);
  }

  caps = GST_BASE_SRC_CLASS (parent_class)->fixate (bsrc, caps);

  return caps;
}

typedef struct {
  GstPinosSrc *src;
  SpaBuffer *buffer;
} ProcessMemData;

static void
process_mem_data_destroy (gpointer user_data)
{
  ProcessMemData *data = user_data;

  spa_buffer_unref (data->buffer);
  gst_object_unref (data->src);
  g_slice_free (ProcessMemData, data);
}

static void
on_new_buffer (GObject    *gobject,
               gpointer    user_data)
{
  GstPinosSrc *pinossrc = user_data;
  SpaBuffer *b;
  GstBuffer *buf;
  unsigned int i;
  ProcessMemData data;

  GST_LOG_OBJECT (pinossrc, "got new buffer");
  if (!(b = pinos_stream_peek_buffer (pinossrc->stream))) {
    g_warning ("failed to capture buffer");
    return;
  }

  buf = gst_buffer_new ();

  data.src = gst_object_ref (pinossrc);
  data.buffer = b;
  gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (buf),
                             process_mem_data_quark,
                             g_slice_dup (ProcessMemData, &data),
                             process_mem_data_destroy);

  for (i = 0; i < b->n_metas; i++) {
    SpaMeta *m = &b->metas[i];

    switch (m->type) {
      case SPA_META_TYPE_HEADER:
      {
        SpaMetaHeader *h = m->data;

        GST_INFO ("pts %" G_GUINT64_FORMAT ", dts_offset %"G_GUINT64_FORMAT, h->pts, h->dts_offset);

        if (GST_CLOCK_TIME_IS_VALID (h->pts)) {
          GST_BUFFER_PTS (buf) = h->pts;
          if (GST_BUFFER_PTS (buf) + h->dts_offset > 0)
            GST_BUFFER_DTS (buf) = GST_BUFFER_PTS (buf) + h->dts_offset;
        }
        GST_BUFFER_OFFSET (buf) = h->seq;
        break;
      }
      default:
        break;
    }
  }
  for (i = 0; i < b->n_datas; i++) {
    SpaData *d = &b->datas[i];

    switch (d->type) {
      case SPA_DATA_TYPE_MEMPTR:
      {
        gst_buffer_append_memory (buf,
                 gst_memory_new_wrapped (0, d->ptr, d->offset + d->size, d->offset,
                                         d->size, NULL, NULL));
        break;
      }
      case SPA_DATA_TYPE_FD:
      {
        GstMemory *fdmem = NULL;
        int fd = *((int *) d->ptr);

        fdmem = gst_fd_allocator_alloc (pinossrc->fd_allocator, dup (fd),
                  d->offset + d->size, GST_FD_MEMORY_FLAG_NONE);
        gst_memory_resize (fdmem, d->offset, d->size);
        gst_buffer_append_memory (buf, fdmem);
        break;
      }
      default:
        break;
    }
  }

  if (buf) {
    g_queue_push_tail (&pinossrc->queue, buf);

    pinos_main_loop_signal (pinossrc->loop, FALSE);
  }
  return;
}

static void
on_stream_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  GstPinosSrc *pinossrc = user_data;
  PinosStreamState state = pinos_stream_get_state (pinossrc->stream);

  GST_DEBUG ("got stream state %d", state);

  GST_OBJECT_LOCK (pinossrc);
  pinossrc->stream_state = state;
  GST_OBJECT_UNLOCK (pinossrc);

  switch (state) {
    case PINOS_STREAM_STATE_UNCONNECTED:
    case PINOS_STREAM_STATE_CONNECTING:
    case PINOS_STREAM_STATE_STARTING:
    case PINOS_STREAM_STATE_STREAMING:
    case PINOS_STREAM_STATE_READY:
      break;
    case PINOS_STREAM_STATE_ERROR:
      GST_ELEMENT_ERROR (pinossrc, RESOURCE, FAILED,
          ("stream error: %s",
            pinos_stream_get_error (pinossrc->stream)->message), (NULL));
      break;
  }
  pinos_main_loop_signal (pinossrc->loop, FALSE);
}

static void
parse_stream_properties (GstPinosSrc *pinossrc, PinosProperties *props)
{
  const gchar *var;

  var = pinos_properties_get (props, "pinos.latency.is-live");
  pinossrc->is_live = var ? (atoi (var) == 1) : FALSE;
  gst_base_src_set_live (GST_BASE_SRC (pinossrc), pinossrc->is_live);

  var = pinos_properties_get (props, "pinos.latency.min");
  pinossrc->min_latency = var ? (GstClockTime) atoi (var) : 0;

  var = pinos_properties_get (props, "pinos.latency.max");
  pinossrc->max_latency = var ? (GstClockTime) atoi (var) : GST_CLOCK_TIME_NONE;

  var = pinos_properties_get (props, "pinos.clock.type");
  if (var != NULL) {
    GST_DEBUG_OBJECT (pinossrc, "got clock type %s", var);
    if (strcmp (var, "gst.net.time.provider") == 0) {
      const gchar *address;
      gint port;
      GstClockTime base_time;

      address = pinos_properties_get (props, "pinos.clock.address");
      port = atoi (pinos_properties_get (props, "pinos.clock.port"));
      base_time = atoll (pinos_properties_get (props, "pinos.clock.base-time"));

      GST_DEBUG_OBJECT (pinossrc, "making net clock for %s:%d %" G_GUINT64_FORMAT, address, port, base_time);
      if (pinossrc->clock)
        gst_object_unref (pinossrc->clock);
      pinossrc->clock = gst_net_client_clock_new ("pinosclock", address, port, base_time);

      gst_element_post_message (GST_ELEMENT_CAST (pinossrc),
          gst_message_new_clock_provide (GST_OBJECT_CAST (pinossrc),
            pinossrc->clock, TRUE));
    }
  }
}


static gboolean
gst_pinos_src_stream_start (GstPinosSrc *pinossrc, GstCaps * caps)
{
  gchar *str;
  GBytes *format = NULL;
  gboolean res;
  PinosProperties *props;

  if (caps) {
    str = gst_caps_to_string (caps);
    format = g_bytes_new_take (str, strlen (str) + 1);
  }

  pinos_main_loop_lock (pinossrc->loop);
  res = pinos_stream_start (pinossrc->stream, format, PINOS_STREAM_MODE_BUFFER);
  while (TRUE) {
    PinosStreamState state = pinos_stream_get_state (pinossrc->stream);

    if (state == PINOS_STREAM_STATE_STREAMING)
      break;

    if (state == PINOS_STREAM_STATE_ERROR)
      goto start_error;

    pinos_main_loop_wait (pinossrc->loop);
  }

  g_object_get (pinossrc->stream, "properties", &props, NULL);
  g_object_get (pinossrc->stream, "format", &format, NULL);

  pinos_main_loop_unlock (pinossrc->loop);

  if (format) {
    caps = gst_caps_from_string (g_bytes_get_data (format, NULL));
    gst_base_src_set_caps (GST_BASE_SRC (pinossrc), caps);
    g_bytes_unref (format);
  }

  parse_stream_properties (pinossrc, props);
  pinos_properties_free (props);

  pinos_main_loop_lock (pinossrc->loop);
  pinossrc->started = TRUE;
  pinos_main_loop_signal (pinossrc->loop, FALSE);
  pinos_main_loop_unlock (pinossrc->loop);

  return res;

start_error:
  {
    GST_DEBUG_OBJECT (pinossrc, "error starting stream");
    return FALSE;
  }
}

static void
wait_negotiated (GstPinosSrc *this)
{
  pinos_main_loop_lock (this->loop);
  while (!this->started) {
    pinos_main_loop_wait (this->loop);
  }
  pinos_main_loop_unlock (this->loop);
}

static gboolean
gst_pinos_src_negotiate (GstBaseSrc * basesrc)
{
  GstPinosSrc *pinossrc = GST_PINOS_SRC (basesrc);
  GstCaps *thiscaps;
  GstCaps *caps = NULL;
  GstCaps *peercaps = NULL;
  gboolean result = FALSE;
  GBytes *possible;
  gchar *str;

  /* first see what is possible on our source pad */
  thiscaps = gst_pad_query_caps (GST_BASE_SRC_PAD (basesrc), NULL);
  GST_DEBUG_OBJECT (basesrc, "caps of src: %" GST_PTR_FORMAT, thiscaps);
  /* nothing or anything is allowed, we're done */
  if (thiscaps == NULL)
    goto no_nego_needed;

  if (G_UNLIKELY (gst_caps_is_empty (thiscaps)))
    goto no_caps;

  /* get the peer caps */
  peercaps = gst_pad_peer_query_caps (GST_BASE_SRC_PAD (basesrc), thiscaps);
  GST_DEBUG_OBJECT (basesrc, "caps of peer: %" GST_PTR_FORMAT, peercaps);
  if (peercaps) {
    /* The result is already a subset of our caps */
    caps = peercaps;
    gst_caps_unref (thiscaps);
  } else {
    /* no peer, work with our own caps then */
    caps = thiscaps;
  }
  if (caps == NULL || gst_caps_is_empty (caps))
    goto no_common_caps;

  GST_DEBUG_OBJECT (basesrc, "have common caps: %" GST_PTR_FORMAT, caps);

  /* open a connection with these caps */
  str = gst_caps_to_string (caps);
  possible = g_bytes_new_take (str, strlen (str) + 1);

  /* first disconnect */
  pinos_main_loop_lock (pinossrc->loop);
  if (pinos_stream_get_state (pinossrc->stream) != PINOS_STREAM_STATE_UNCONNECTED) {
    GST_DEBUG_OBJECT (basesrc, "disconnect capture");
    pinos_stream_disconnect (pinossrc->stream);
    while (TRUE) {
      PinosStreamState state = pinos_stream_get_state (pinossrc->stream);

      if (state == PINOS_STREAM_STATE_UNCONNECTED)
        break;

      if (state == PINOS_STREAM_STATE_ERROR) {
        g_bytes_unref (possible);
        goto connect_error;
      }

      pinos_main_loop_wait (pinossrc->loop);
    }
  }

  GST_DEBUG_OBJECT (basesrc, "connect capture with path %s", pinossrc->path);
  pinos_stream_connect (pinossrc->stream,
                        PINOS_DIRECTION_INPUT,
                        pinossrc->path,
                        PINOS_STREAM_FLAG_AUTOCONNECT,
                        possible);

  while (TRUE) {
    PinosStreamState state = pinos_stream_get_state (pinossrc->stream);

    if (state == PINOS_STREAM_STATE_READY)
      break;

    if (state == PINOS_STREAM_STATE_ERROR)
      goto connect_error;

    pinos_main_loop_wait (pinossrc->loop);
  }
  pinos_main_loop_unlock (pinossrc->loop);

  result = gst_pinos_src_stream_start (pinossrc, NULL);

  pinossrc->negotiated = result;

  return result;

no_nego_needed:
  {
    GST_DEBUG_OBJECT (basesrc, "no negotiation needed");
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return TRUE;
  }
no_caps:
  {
    GST_ELEMENT_ERROR (basesrc, STREAM, FORMAT,
        ("No supported formats found"),
        ("This element did not produce valid caps"));
    if (thiscaps)
      gst_caps_unref (thiscaps);
    return FALSE;
  }
no_common_caps:
  {
    GST_ELEMENT_ERROR (basesrc, STREAM, FORMAT,
        ("No supported formats found"),
        ("This element does not have formats in common with the peer"));
    if (caps)
      gst_caps_unref (caps);
    return FALSE;
  }
connect_error:
  {
    pinos_main_loop_unlock (pinossrc->loop);
    return FALSE;
  }
}

static void
on_format_notify (GObject    *gobject,
                  GParamSpec *pspec,
                  gpointer    user_data)
{
  GstPinosSrc *pinossrc = user_data;
  GBytes *format;
  GstCaps *caps;

  g_object_get (gobject, "format", &format, NULL);

  caps = gst_caps_from_string (g_bytes_get_data (format, NULL));
  gst_base_src_set_caps (GST_BASE_SRC (pinossrc), caps);
  gst_caps_unref (caps);
}

static gboolean
gst_pinos_src_unlock (GstBaseSrc * basesrc)
{
  GstPinosSrc *pinossrc = GST_PINOS_SRC (basesrc);

  pinos_main_loop_lock (pinossrc->loop);
  GST_DEBUG_OBJECT (pinossrc, "setting flushing");
  pinossrc->flushing = TRUE;
  pinos_main_loop_signal (pinossrc->loop, FALSE);
  pinos_main_loop_unlock (pinossrc->loop);

  return TRUE;
}

static gboolean
gst_pinos_src_unlock_stop (GstBaseSrc * basesrc)
{
  GstPinosSrc *pinossrc = GST_PINOS_SRC (basesrc);

  pinos_main_loop_lock (pinossrc->loop);
  GST_DEBUG_OBJECT (pinossrc, "unsetting flushing");
  pinossrc->flushing = FALSE;
  pinos_main_loop_unlock (pinossrc->loop);

  return TRUE;
}

static gboolean
gst_pinos_src_event (GstBaseSrc * src, GstEvent * event)
{
  gboolean res = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_UPSTREAM:
      if (gst_video_event_is_force_key_unit (event)) {
        GstClockTime running_time;
        gboolean all_headers;
        guint count;

        gst_video_event_parse_upstream_force_key_unit (event,
                &running_time, &all_headers, &count);

#if 0
        pinos_buffer_builder_init (&b);

        refresh.last_id = 0;
        refresh.request_type = all_headers ? 1 : 0;
        refresh.pts = running_time;
        pinos_buffer_builder_add_refresh_request (&b, &refresh);

        pinos_buffer_builder_end (&b, &pbuf);

        GST_OBJECT_LOCK (pinossrc);
        if (pinossrc->stream_state == PINOS_STREAM_STATE_STREAMING) {
          GST_DEBUG_OBJECT (pinossrc, "send refresh request");
          pinos_stream_send_buffer (pinossrc->stream, &pbuf);
        }
        GST_OBJECT_UNLOCK (pinossrc);

        pinos_buffer_unref (&pbuf);
#endif
        res = TRUE;
      } else {
        res = GST_BASE_SRC_CLASS (parent_class)->event (src, event);
      }
      break;
    default:
      res = GST_BASE_SRC_CLASS (parent_class)->event (src, event);
      break;
  }
  return res;
}

static gboolean
gst_pinos_src_query (GstBaseSrc * src, GstQuery * query)
{
  gboolean res = FALSE;
  GstPinosSrc *pinossrc;

  pinossrc = GST_PINOS_SRC (src);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
      gst_query_set_latency (query, pinossrc->is_live, pinossrc->min_latency, pinossrc->max_latency);
      res = TRUE;
      break;
    default:
      res = GST_BASE_SRC_CLASS (parent_class)->query (src, query);
      break;
  }
  return res;
}

static GstFlowReturn
gst_pinos_src_create (GstPushSrc * psrc, GstBuffer ** buffer)
{
  GstPinosSrc *pinossrc;
  GstClockTime pts, dts, base_time;

  pinossrc = GST_PINOS_SRC (psrc);

  if (!pinossrc->negotiated)
    goto not_negotiated;

  pinos_main_loop_lock (pinossrc->loop);
  while (TRUE) {
    PinosStreamState state;

    if (pinossrc->flushing)
      goto streaming_stopped;

    state = pinos_stream_get_state (pinossrc->stream);
    if (state == PINOS_STREAM_STATE_ERROR)
      goto streaming_error;

    if (state != PINOS_STREAM_STATE_STREAMING)
      goto streaming_stopped;

    *buffer = g_queue_pop_head (&pinossrc->queue);
    if (*buffer != NULL)
      break;

    pinos_main_loop_wait (pinossrc->loop);
  }
  pinos_main_loop_unlock (pinossrc->loop);

  base_time = GST_ELEMENT_CAST (psrc)->base_time;
  pts = GST_BUFFER_PTS (*buffer);
  dts = GST_BUFFER_DTS (*buffer);

  if (GST_CLOCK_TIME_IS_VALID (pts))
    pts = (pts >= base_time ? pts - base_time : 0);
  if (GST_CLOCK_TIME_IS_VALID (dts))
    dts = (dts >= base_time ? dts - base_time : 0);

  GST_INFO ("pts %" G_GUINT64_FORMAT ", dts %"G_GUINT64_FORMAT
      ", base-time %"GST_TIME_FORMAT" -> %"GST_TIME_FORMAT", %"GST_TIME_FORMAT,
      GST_BUFFER_PTS (*buffer), GST_BUFFER_DTS (*buffer), GST_TIME_ARGS (base_time),
      GST_TIME_ARGS (pts), GST_TIME_ARGS (dts));

  GST_BUFFER_PTS (*buffer) = pts;
  GST_BUFFER_DTS (*buffer) = dts;

  return GST_FLOW_OK;

not_negotiated:
  {
    return GST_FLOW_NOT_NEGOTIATED;
  }
streaming_error:
  {
    pinos_main_loop_unlock (pinossrc->loop);
    return GST_FLOW_ERROR;
  }
streaming_stopped:
  {
    pinos_main_loop_unlock (pinossrc->loop);
    return GST_FLOW_FLUSHING;
  }
}

static gboolean
gst_pinos_src_start (GstBaseSrc * basesrc)
{
  return TRUE;
}

static void
clear_queue (GstPinosSrc *pinossrc)
{
  g_queue_foreach (&pinossrc->queue, (GFunc) gst_mini_object_unref, NULL);
  g_queue_clear (&pinossrc->queue);
}

static gboolean
gst_pinos_src_stop (GstBaseSrc * basesrc)
{
  GstPinosSrc *pinossrc;

  pinossrc = GST_PINOS_SRC (basesrc);

  pinos_main_loop_lock (pinossrc->loop);
  clear_queue (pinossrc);
  pinos_main_loop_unlock (pinossrc->loop);

  return TRUE;
}

static void
on_context_notify (GObject    *gobject,
                   GParamSpec *pspec,
                   gpointer    user_data)
{
  GstPinosSrc *pinossrc = user_data;
  PinosContextState state = pinos_context_get_state (pinossrc->ctx);

  GST_DEBUG ("got context state %d", state);

  switch (state) {
    case PINOS_CONTEXT_STATE_UNCONNECTED:
    case PINOS_CONTEXT_STATE_CONNECTING:
    case PINOS_CONTEXT_STATE_CONNECTED:
      break;
    case PINOS_CONTEXT_STATE_ERROR:
      GST_ELEMENT_ERROR (pinossrc, RESOURCE, FAILED,
          ("context error: %s",
            pinos_context_get_error (pinossrc->ctx)->message), (NULL));
      break;
  }
  pinos_main_loop_signal (pinossrc->loop, FALSE);
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
gst_pinos_src_open (GstPinosSrc * pinossrc)
{
  GError *error = NULL;
  PinosProperties *props;

  pinossrc->context = g_main_context_new ();
  GST_DEBUG ("context %p", pinossrc->context);

  pinossrc->loop = pinos_main_loop_new (pinossrc->context, "pinos-main-loop");
  if (!pinos_main_loop_start (pinossrc->loop, &error))
    goto mainloop_failed;

  pinos_main_loop_lock (pinossrc->loop);
  pinossrc->ctx = pinos_context_new (pinossrc->context, g_get_application_name (), NULL);
  g_signal_connect (pinossrc->ctx, "notify::state", (GCallback) on_context_notify, pinossrc);

  pinos_context_connect (pinossrc->ctx, PINOS_CONTEXT_FLAGS_NONE);

  while (TRUE) {
    PinosContextState state = pinos_context_get_state (pinossrc->ctx);

    if (state == PINOS_CONTEXT_STATE_CONNECTED)
      break;

    if (state == PINOS_CONTEXT_STATE_ERROR)
      goto connect_error;

    pinos_main_loop_wait (pinossrc->loop);
  }

  if (pinossrc->properties) {
    props = pinos_properties_new (NULL, NULL);
    gst_structure_foreach (pinossrc->properties, copy_properties, props);
  } else {
    props = NULL;
  }

  pinossrc->stream = pinos_stream_new (pinossrc->ctx, pinossrc->client_name, props);
  g_signal_connect (pinossrc->stream, "notify::state", (GCallback) on_stream_notify, pinossrc);
  g_signal_connect (pinossrc->stream, "notify::format", (GCallback) on_format_notify, pinossrc);
  g_signal_connect (pinossrc->stream, "new-buffer", (GCallback) on_new_buffer, pinossrc);
  pinos_main_loop_unlock (pinossrc->loop);

  return TRUE;

  /* ERRORS */
mainloop_failed:
  {
    GST_ELEMENT_ERROR (pinossrc, RESOURCE, FAILED,
        ("mainloop error: %s", error->message), (NULL));
    return FALSE;
  }
connect_error:
  {
    pinos_main_loop_unlock (pinossrc->loop);
    return FALSE;
  }
}

static void
gst_pinos_src_close (GstPinosSrc * pinossrc)
{
  pinos_main_loop_stop (pinossrc->loop);
  g_clear_object (&pinossrc->loop);
  g_clear_object (&pinossrc->ctx);
  g_main_context_unref (pinossrc->context);
  GST_OBJECT_LOCK (pinossrc);
  pinossrc->stream_state = PINOS_STREAM_STATE_UNCONNECTED;
  g_clear_object (&pinossrc->stream);
  GST_OBJECT_UNLOCK (pinossrc);
  clear_queue (pinossrc);
  if (pinossrc->clock)
    gst_object_unref (pinossrc->clock);
  pinossrc->clock = NULL;
}

static GstStateChangeReturn
gst_pinos_src_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;
  GstPinosSrc *this = GST_PINOS_SRC_CAST (element);

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
      if (!gst_pinos_src_open (this))
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
    case GST_STATE_CHANGE_READY_TO_PAUSED:
      wait_negotiated (this);
      if (gst_base_src_is_live (GST_BASE_SRC (element)))
        ret = GST_STATE_CHANGE_NO_PREROLL;
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      this->negotiated = FALSE;
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      gst_pinos_src_close (this);
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
