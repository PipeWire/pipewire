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
 * SECTION:element-pinosportsrc
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v pinosportsrc ! videoconvert ! ximagesink
 * ]| Shows pinos output in an X window.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif
#include "gstpinosportsrc.h"

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gunixfdmessage.h>
#include <gst/net/gstnetclientclock.h>
#include <gst/allocators/gstfdmemory.h>
#include <gst/video/video.h>


static GQuark fdpayload_data_quark;

GST_DEBUG_CATEGORY_STATIC (pinos_port_src_debug);
#define GST_CAT_DEFAULT pinos_port_src_debug

enum
{
  PROP_0,
  PROP_PORT,
};


#define PINOSS_VIDEO_CAPS GST_VIDEO_CAPS_MAKE (GST_VIDEO_FORMATS_ALL)

static GstStaticPadTemplate gst_pinos_port_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );

#define gst_pinos_port_src_parent_class parent_class
G_DEFINE_TYPE (GstPinosPortSrc, gst_pinos_port_src, GST_TYPE_PUSH_SRC);

static GstStateChangeReturn
gst_pinos_port_src_change_state (GstElement * element, GstStateChange transition);

static GstCaps *gst_pinos_port_src_getcaps (GstBaseSrc * bsrc, GstCaps * filter);

static GstFlowReturn gst_pinos_port_src_create (GstPushSrc * psrc,
    GstBuffer ** buffer);
static gboolean gst_pinos_port_src_start (GstBaseSrc * basesrc);
static gboolean gst_pinos_port_src_stop (GstBaseSrc * basesrc);
static gboolean gst_pinos_port_src_event (GstBaseSrc * src, GstEvent * event);
static gboolean gst_pinos_port_src_query (GstBaseSrc * src, GstQuery * query);

typedef struct {
  GstPinosPortSrc *src;
  PinosPacketFDPayload p;
} FDPayloadData;

static void
fdpayload_data_destroy (gpointer user_data)
{
  FDPayloadData *data = user_data;
  GstPinosPortSrc *this = data->src;
  PinosBufferBuilder b;
  PinosPacketReleaseFDPayload r;
  PinosBuffer pbuf;

  r.id = data->p.id;

  GST_DEBUG_OBJECT (this, "destroy %d", r.id);

  pinos_port_buffer_builder_init (this->port, &b);
  pinos_buffer_builder_add_release_fd_payload (&b, &r);
  pinos_buffer_builder_end (&b, &pbuf);

  pinos_port_send_buffer (this->port, &pbuf, NULL);
  pinos_buffer_unref (&pbuf);

  gst_object_unref (this);
  g_slice_free (FDPayloadData, data);
}

static void
on_received_buffer (PinosPort  *port,
                    gpointer    user_data)
{
  GstPinosPortSrc *this = user_data;
  PinosBuffer *pbuf;
  PinosBufferIter it;
  GstBuffer *buf = NULL;

  GST_LOG_OBJECT (this, "got new buffer");
  pbuf = pinos_port_peek_buffer (port);

  pinos_buffer_iter_init (&it, pbuf);
  while (pinos_buffer_iter_next (&it)) {
    switch (pinos_buffer_iter_get_type (&it)) {
      case PINOS_PACKET_TYPE_HEADER:
      {
        PinosPacketHeader hdr;

        if (!pinos_buffer_iter_parse_header  (&it, &hdr))
          break;

        if (buf == NULL)
          buf = gst_buffer_new ();

        GST_INFO ("pts %" G_GUINT64_FORMAT ", dts_offset %"G_GUINT64_FORMAT, hdr.pts, hdr.dts_offset);

        if (GST_CLOCK_TIME_IS_VALID (hdr.pts)) {
          GST_BUFFER_PTS (buf) = hdr.pts;
          if (GST_BUFFER_PTS (buf) + hdr.dts_offset > 0)
            GST_BUFFER_DTS (buf) = GST_BUFFER_PTS (buf) + hdr.dts_offset;
        }
        GST_BUFFER_OFFSET (buf) = hdr.seq;
        break;
      }
      case PINOS_PACKET_TYPE_FD_PAYLOAD:
      {
        GstMemory *fdmem = NULL;
        FDPayloadData data;
        int fd;

        if (!pinos_buffer_iter_parse_fd_payload  (&it, &data.p))
          break;

        GST_DEBUG ("got fd payload id %d", data.p.id);
        fd = pinos_buffer_get_fd (pbuf, data.p.fd_index);
        if (fd == -1)
          break;

        if (buf == NULL)
          buf = gst_buffer_new ();

        fdmem = gst_fd_allocator_alloc (this->fd_allocator, dup (fd),
                  data.p.offset + data.p.size, GST_FD_MEMORY_FLAG_NONE);
        gst_memory_resize (fdmem, data.p.offset, data.p.size);
        gst_buffer_append_memory (buf, fdmem);

        data.src = gst_object_ref (this);
        gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (fdmem),
                                   fdpayload_data_quark,
                                   g_slice_dup (FDPayloadData, &data),
                                   fdpayload_data_destroy);
        break;
      }
      case PINOS_PACKET_TYPE_FORMAT_CHANGE:
      {
        PinosPacketFormatChange change;
        GstCaps *caps;

        if (!pinos_buffer_iter_parse_format_change  (&it, &change))
          break;
        GST_DEBUG ("got format change %d %s", change.id, change.format);

        caps = gst_caps_from_string (change.format);
        gst_base_src_set_caps (GST_BASE_SRC (this), caps);
        gst_caps_unref (caps);
        break;
      }
      default:
        break;
    }
  }
  pinos_buffer_iter_end (&it);

  if (buf) {
    g_queue_push_tail (&this->queue, buf);
    g_cond_signal (&this->cond);
  }

  return;
}

static void
set_port (GstPinosPortSrc *this, PinosPort *port)
{
  g_debug ("set port %p", port);

  if (this->port)
    g_object_unref (this->port);
  this->port = port;

  pinos_port_set_received_buffer_cb (port, on_received_buffer, this, NULL);
}

static void
gst_pinos_port_src_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPinosPortSrc *this = GST_PINOS_PORT_SRC (object);

  switch (prop_id) {
    case PROP_PORT:
      set_port (this, g_value_dup_object (value));
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pinos_port_src_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPinosPortSrc *this = GST_PINOS_PORT_SRC (object);

  switch (prop_id) {
    case PROP_PORT:
      g_value_set_object (value, this->port);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstClock *
gst_pinos_port_src_provide_clock (GstElement * elem)
{
  GstPinosPortSrc *this = GST_PINOS_PORT_SRC (elem);
  GstClock *clock;

  GST_OBJECT_LOCK (this);
  if (!GST_OBJECT_FLAG_IS_SET (this, GST_ELEMENT_FLAG_PROVIDE_CLOCK))
    goto clock_disabled;

  if (this->clock)
    clock = GST_CLOCK_CAST (gst_object_ref (this->clock));
  else
    clock = NULL;
  GST_OBJECT_UNLOCK (this);

  return clock;

  /* ERRORS */
clock_disabled:
  {
    GST_DEBUG_OBJECT (this, "clock provide disabled");
    GST_OBJECT_UNLOCK (this);
    return NULL;
  }
}

static gboolean
gst_pinos_port_src_unlock (GstBaseSrc * basesrc)
{
  GstPinosPortSrc *this = GST_PINOS_PORT_SRC (basesrc);

  GST_DEBUG_OBJECT (this, "setting flushing");

  GST_OBJECT_LOCK (this);
  this->flushing = TRUE;
  g_cond_signal (&this->cond);
  GST_OBJECT_UNLOCK (this);

  return TRUE;
}

static gboolean
gst_pinos_port_src_unlock_stop (GstBaseSrc * basesrc)
{
  GstPinosPortSrc *this = GST_PINOS_PORT_SRC (basesrc);

  GST_DEBUG_OBJECT (this, "unsetting flushing");
  this->flushing = FALSE;

  return TRUE;
}

static void
gst_pinos_port_src_finalize (GObject * object)
{
  GstPinosPortSrc *this = GST_PINOS_PORT_SRC (object);

  g_queue_foreach (&this->queue, (GFunc) gst_mini_object_unref, NULL);
  g_queue_clear (&this->queue);
  g_cond_clear (&this->cond);
  g_object_unref (this->fd_allocator);
  g_object_unref (this->port);
  if (this->clock)
    gst_object_unref (this->clock);

  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_pinos_port_src_class_init (GstPinosPortSrcClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSrcClass *gstbasesrc_class;
  GstPushSrcClass *gstpushsrc_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesrc_class = (GstBaseSrcClass *) klass;
  gstpushsrc_class = (GstPushSrcClass *) klass;

  gobject_class->finalize = gst_pinos_port_src_finalize;
  gobject_class->set_property = gst_pinos_port_src_set_property;
  gobject_class->get_property = gst_pinos_port_src_get_property;

  g_object_class_install_property (gobject_class, PROP_PORT,
              g_param_spec_object ("port", "Port",
                                   "The pinos port object",
                                   PINOS_TYPE_PORT,
                                   G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));


  gstelement_class->provide_clock = gst_pinos_port_src_provide_clock;
  gstelement_class->change_state = gst_pinos_port_src_change_state;

  gst_element_class_set_static_metadata (gstelement_class,
      "Pinos source", "Source/Video",
      "Uses pinos to create video", "Wim Taymans <wim.taymans@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pinos_port_src_template));

  gstbasesrc_class->get_caps = gst_pinos_port_src_getcaps;
  gstbasesrc_class->unlock = gst_pinos_port_src_unlock;
  gstbasesrc_class->unlock_stop = gst_pinos_port_src_unlock_stop;
  gstbasesrc_class->start = gst_pinos_port_src_start;
  gstbasesrc_class->stop = gst_pinos_port_src_stop;
  gstbasesrc_class->event = gst_pinos_port_src_event;
  gstbasesrc_class->query = gst_pinos_port_src_query;
  gstpushsrc_class->create = gst_pinos_port_src_create;

  GST_DEBUG_CATEGORY_INIT (pinos_port_src_debug, "pinosportsrc", 0,
      "Pinos Source");

  fdpayload_data_quark = g_quark_from_static_string ("GstPinosPortSrcFDPayloadQuark");
}

static void
gst_pinos_port_src_init (GstPinosPortSrc * src)
{
  /* we operate in time */
  gst_base_src_set_format (GST_BASE_SRC (src), GST_FORMAT_TIME);

  GST_OBJECT_FLAG_SET (src, GST_ELEMENT_FLAG_PROVIDE_CLOCK);

  g_cond_init (&src->cond);
  g_queue_init (&src->queue);

  src->fd_allocator = gst_fd_allocator_new ();
}

static void
parse_stream_properties (GstPinosPortSrc *this, PinosProperties *props)
{
  const gchar *var;

  var = pinos_properties_get (props, "pinos.latency.is-live");
  this->is_live = var ? (atoi (var) == 1) : FALSE;
  gst_base_src_set_live (GST_BASE_SRC (this), this->is_live);

  var = pinos_properties_get (props, "pinos.latency.min");
  this->min_latency = var ? (GstClockTime) atoi (var) : 0;

  var = pinos_properties_get (props, "pinos.latency.max");
  this->max_latency = var ? (GstClockTime) atoi (var) : GST_CLOCK_TIME_NONE;

  var = pinos_properties_get (props, "pinos.clock.type");
  if (var != NULL) {
    GST_DEBUG_OBJECT (this, "got clock type %s", var);
    if (strcmp (var, "gst.net.time.provider") == 0) {
      const gchar *address;
      gint port;
      GstClockTime base_time;

      address = pinos_properties_get (props, "pinos.clock.address");
      port = atoi (pinos_properties_get (props, "pinos.clock.port"));
      base_time = atoll (pinos_properties_get (props, "pinos.clock.base-time"));

      GST_DEBUG_OBJECT (this, "making net clock for %s:%d %" G_GUINT64_FORMAT, address, port, base_time);
      if (this->clock)
        gst_object_unref (this->clock);
      this->clock = gst_net_client_clock_new ("pinosclock", address, port, base_time);

      gst_element_post_message (GST_ELEMENT_CAST (this),
          gst_message_new_clock_provide (GST_OBJECT_CAST (this),
            this->clock, TRUE));
    }
  }
}

static GstCaps *
gst_pinos_port_src_getcaps (GstBaseSrc * bsrc, GstCaps * filter)
{
  GstPinosPortSrc *this = GST_PINOS_PORT_SRC (bsrc);
  GBytes *format;
  GstCaps *caps = NULL;

  GST_DEBUG ("getting caps");

  g_object_get (this->port, "format", &format, NULL);
  if (format) {
    GST_DEBUG ("have format %s", (gchar *)g_bytes_get_data (format, NULL));
    caps = gst_caps_from_string (g_bytes_get_data (format, NULL));
    g_bytes_unref (format);
  }
  return caps;
}

static gboolean
gst_pinos_port_src_event (GstBaseSrc * src, GstEvent * event)
{
  gboolean res = FALSE;
  GstPinosPortSrc *this;

  this = GST_PINOS_PORT_SRC (src);

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CUSTOM_UPSTREAM:
      if (gst_video_event_is_force_key_unit (event)) {
        GstClockTime running_time;
        gboolean all_headers;
        guint count;
        PinosPacketRefreshRequest refresh;
        PinosBufferBuilder b;
        PinosBuffer pbuf;

        gst_video_event_parse_upstream_force_key_unit (event,
                &running_time, &all_headers, &count);

        refresh.last_id = 0;
        refresh.request_type = all_headers ? 1 : 0;
        refresh.pts = running_time;

        pinos_port_buffer_builder_init (this->port, &b);
        pinos_buffer_builder_add_refresh_request (&b, &refresh);
        pinos_buffer_builder_end (&b, &pbuf);

        GST_DEBUG_OBJECT (this, "send refresh request");
        pinos_port_send_buffer (this->port, &pbuf, NULL);
        pinos_buffer_unref (&pbuf);
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
gst_pinos_port_src_query (GstBaseSrc * src, GstQuery * query)
{
  gboolean res = FALSE;
  GstPinosPortSrc *this;

  this = GST_PINOS_PORT_SRC (src);

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_LATENCY:
      gst_query_set_latency (query, this->is_live, this->min_latency, this->max_latency);
      res = TRUE;
      break;
    default:
      res = GST_BASE_SRC_CLASS (parent_class)->query (src, query);
      break;
  }
  return res;
}

static GstFlowReturn
gst_pinos_port_src_create (GstPushSrc * psrc, GstBuffer ** buffer)
{
  GstPinosPortSrc *this;
  GstClockTime pts, dts, base_time;

  this = GST_PINOS_PORT_SRC (psrc);

  GST_OBJECT_LOCK (this);
  while (TRUE) {
    if (this->flushing)
      goto streaming_stopped;

    *buffer = g_queue_pop_head (&this->queue);
    if (*buffer != NULL)
      break;

    g_cond_wait (&this->cond, GST_OBJECT_GET_LOCK (this));
  }
  GST_OBJECT_UNLOCK (this);

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

streaming_stopped:
  {
    GST_OBJECT_UNLOCK (this);
    return GST_FLOW_FLUSHING;
  }
}

static gboolean
gst_pinos_port_src_start (GstBaseSrc * basesrc)
{
  PinosProperties *props;
  GstPinosPortSrc *this;

  this = GST_PINOS_PORT_SRC (basesrc);

  props = pinos_port_get_properties (this->port);
  if (props)
    parse_stream_properties (this, props);

  return TRUE;
}

static void
clear_queue (GstPinosPortSrc *this)
{
  g_queue_foreach (&this->queue, (GFunc) gst_mini_object_unref, NULL);
  g_queue_clear (&this->queue);
}

static gboolean
gst_pinos_port_src_stop (GstBaseSrc * basesrc)
{
  GstPinosPortSrc *this;

  this = GST_PINOS_PORT_SRC (basesrc);

  clear_queue (this);

  return TRUE;
}

static GstStateChangeReturn
gst_pinos_port_src_change_state (GstElement * element, GstStateChange transition)
{
  GstStateChangeReturn ret;

  switch (transition) {
    case GST_STATE_CHANGE_NULL_TO_READY:
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
      if (gst_base_src_is_live (GST_BASE_SRC (element)))
        ret = GST_STATE_CHANGE_NO_PREROLL;
      break;
    case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
      break;
    case GST_STATE_CHANGE_PAUSED_TO_READY:
      break;
    case GST_STATE_CHANGE_READY_TO_NULL:
      break;
    default:
      break;
  }
  return ret;
}
