/* GStreamer
 * Copyright (C) <2016> Wim Taymans <wim.taymans@gmail.com>
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
 * SECTION:element-pinosportsink
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <gio/gunixfdmessage.h>

#include <gst/allocators/gstfdmemory.h>
#include <gst/net/gstnetcontrolmessagemeta.h>
#include <gst/video/video.h>

#include "gstpinosportsink.h"
#include "gsttmpfileallocator.h"

GST_DEBUG_CATEGORY_STATIC (pinos_port_sink_debug);
#define GST_CAT_DEFAULT pinos_port_sink_debug

static GstStaticPadTemplate gst_pinos_port_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY
    );

enum
{
  PROP_0,
  PROP_PORT,
};

#define gst_pinos_port_sink_parent_class parent_class
G_DEFINE_TYPE (GstPinosPortSink, gst_pinos_port_sink, GST_TYPE_BASE_SINK);

static gboolean
gst_pinos_port_sink_propose_allocation (GstBaseSink * bsink, GstQuery * query)
{
  GstPinosPortSink *this = GST_PINOS_PORT_SINK (bsink);

  gst_query_add_allocation_param (query, this->allocator, NULL);
  gst_query_add_allocation_meta (query, GST_NET_CONTROL_MESSAGE_META_API_TYPE,
            NULL);

  return TRUE;
}

static gboolean
on_received_buffer (PinosPort *port, PinosBuffer *pbuf, GError **error, gpointer user_data)
{
  GstPinosPortSink *this = user_data;
  GstEvent *ev;
  PinosBufferIter it;
  PinosBufferBuilder b;
  gboolean have_out = FALSE;
  guint8 buffer[1024];
  gint fds[8];

  if (this->pinos_input) {
    pinos_buffer_builder_init_into (&b, buffer, 1024, fds, 8);
  }

  pinos_buffer_iter_init (&it, pbuf);
  while (pinos_buffer_iter_next (&it)) {
    switch (pinos_buffer_iter_get_type (&it)) {
      case PINOS_PACKET_TYPE_REFRESH_REQUEST:
      {
        PinosPacketRefreshRequest p;

        if (!pinos_buffer_iter_parse_refresh_request (&it, &p))
          continue;

        GST_LOG ("refresh request");
        if (!this->pinos_input) {
          gst_pad_push_event (GST_BASE_SINK_PAD (this),
              gst_video_event_new_upstream_force_key_unit (p.pts,
              p.request_type == 1, 0));
        } else {
          pinos_buffer_builder_add_refresh_request (&b, &p);
          have_out = TRUE;
        }
        break;
      }
      default:
        break;
    }
  }
  pinos_buffer_iter_end (&it);

  if (this->pinos_input) {
    GstBuffer *outbuf;
    gsize size;
    gpointer data;

    if (have_out) {
      pinos_buffer_builder_end (&b, pbuf);

      data = pinos_buffer_steal_data (pbuf, &size);

      outbuf = gst_buffer_new_wrapped (data, size);
      ev = gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM,
              gst_structure_new ("GstNetworkMessage",
                  "object", G_TYPE_OBJECT, this,
                  "buffer", GST_TYPE_BUFFER, outbuf, NULL));
      gst_buffer_unref (outbuf);

      gst_pad_push_event (GST_BASE_SINK_PAD (this), ev);
    } else {
      pinos_buffer_builder_clear (&b);
    }
  }
  return TRUE;
}

static void
set_port (GstPinosPortSink *this, PinosPort *port)
{
  if (this->port)
    g_object_unref (this->port);
  this->port = port;

  pinos_port_set_received_buffer_cb (port, on_received_buffer, this, NULL);
}

static void
gst_pinos_port_sink_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPinosPortSink *this = GST_PINOS_PORT_SINK (object);

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
gst_pinos_port_sink_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPinosPortSink *this = GST_PINOS_PORT_SINK (object);

  switch (prop_id) {
    case PROP_PORT:
      g_value_set_object (value, this->port);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static GstCaps *
gst_pinos_port_sink_getcaps (GstBaseSink * bsink, GstCaps * filter)
{
  GstPinosPortSink *this = GST_PINOS_PORT_SINK (bsink);
  GBytes *filt, *formats;
  gchar *cstr;
  GstCaps *result;

  if (filter) {
    cstr = gst_caps_to_string (filter);
    filt = g_bytes_new_take (cstr, strlen (cstr) + 1);
  } else {
    filt = NULL;
  }

  formats = pinos_port_filter_formats (this->port, filt, NULL);

  if (filt)
    g_bytes_unref (filt);

  if (formats) {
    result = gst_caps_from_string (g_bytes_get_data (formats, NULL));
    g_bytes_unref (formats);
  } else {
    result = gst_caps_new_empty ();
  }

  return result;
}

static gboolean
gst_pinos_port_sink_setcaps (GstBaseSink * bsink, GstCaps * caps)
{
  GstPinosPortSink *this = GST_PINOS_PORT_SINK (bsink);
  GstStructure *str;
  gchar *cstr;

  str = gst_caps_get_structure (caps, 0);
  this->pinos_input = gst_structure_has_name (str, "application/x-pinos");
  if (!this->pinos_input) {
    GError *error = NULL;
    PinosBufferBuilder builder;
    PinosBuffer pbuf;
    PinosPacketFormatChange fc;
    guint8 buffer[1024];

    pinos_buffer_builder_init_into (&builder, buffer, 1024, NULL, 0);
    fc.id = 0;
    fc.format = cstr = gst_caps_to_string (caps);
    pinos_buffer_builder_add_format_change (&builder, &fc);
    pinos_buffer_builder_end (&builder, &pbuf);
    g_free (cstr);

    if (!pinos_port_send_buffer (this->port, &pbuf, &error)) {
      GST_WARNING ("format update failed: %s", error->message);
      g_clear_error (&error);
    }
    pinos_buffer_unref (&pbuf);
  }

  return GST_BASE_SINK_CLASS (parent_class)->set_caps (bsink, caps);
}

static GstFlowReturn
gst_pinos_port_sink_render_pinos (GstPinosPortSink * this, GstBuffer * buffer)
{
  GstMapInfo info;
  PinosBuffer pbuf;
  GError *error = NULL;

  gst_buffer_map (buffer, &info, GST_MAP_READ);
  pinos_buffer_init_data (&pbuf, info.data, info.size, NULL, 0);

  if (!pinos_port_send_buffer (this->port, &pbuf, &error)) {
    GST_WARNING ("send failed: %s", error->message);
    g_clear_error (&error);
  }
  gst_buffer_unmap (buffer, &info);
  pinos_buffer_unref (&pbuf);

  return GST_FLOW_OK;
}

static GstMemory *
gst_pinos_port_sink_get_fd_memory (GstPinosPortSink * this, GstBuffer * buffer, gboolean *tmpfile)
{
  GstMemory *mem = NULL;

  if (gst_buffer_n_memory (buffer) == 1
      && gst_is_fd_memory (gst_buffer_peek_memory (buffer, 0))) {
    mem = gst_buffer_get_memory (buffer, 0);
    *tmpfile = gst_is_tmpfile_memory (mem);
  } else {
    GstMapInfo info;
    GstAllocationParams params = {0, 0, 0, 0, { NULL, }};
    gsize size = gst_buffer_get_size (buffer);
    GST_INFO_OBJECT (this, "Buffer cannot be sent without copying");
    mem = gst_allocator_alloc (this->allocator, size, &params);
    if (!gst_memory_map (mem, &info, GST_MAP_WRITE))
      return NULL;
    gst_buffer_extract (buffer, 0, info.data, size);
    gst_memory_unmap (mem, &info);
    *tmpfile = TRUE;
  }
  return mem;
}

static GstFlowReturn
gst_pinos_port_sink_render_other (GstPinosPortSink * this, GstBuffer * buffer)
{
  GstMemory *fdmem = NULL;
  GError *error = NULL;
  PinosBuffer pbuf;
  PinosBufferBuilder builder;
  PinosPacketHeader hdr;
  PinosPacketAddMem am;
  PinosPacketProcessMem p;
  PinosPacketRemoveMem rm;
  gboolean tmpfile = TRUE;
  guint8 send_buffer[1024];
  gint send_fds[8];

  hdr.flags = 0;
  hdr.seq = GST_BUFFER_OFFSET (buffer);
  hdr.pts = GST_BUFFER_PTS (buffer) + GST_ELEMENT_CAST (this)->base_time;
  hdr.dts_offset = 0;

  pinos_buffer_builder_init_into (&builder, send_buffer, 1024, send_fds, 8);
  pinos_buffer_builder_add_header (&builder, &hdr);

  fdmem = gst_pinos_port_sink_get_fd_memory (this, buffer, &tmpfile);
  am.id = pinos_fd_manager_get_id (this->fdmanager);
  am.fd_index = pinos_buffer_builder_add_fd (&builder, gst_fd_memory_get_fd (fdmem));
  am.offset = 0;
  am.size = fdmem->size + fdmem->offset;
  p.id = am.id;
  p.offset = fdmem->offset;
  p.size = fdmem->size;
  rm.id = am.id;
  pinos_buffer_builder_add_add_mem (&builder, &am);
  pinos_buffer_builder_add_process_mem (&builder, &p);
  pinos_buffer_builder_add_remove_mem (&builder, &rm);
  pinos_buffer_builder_end (&builder, &pbuf);

  GST_LOG ("send %d %"G_GUINT64_FORMAT" %"G_GUINT64_FORMAT" %"G_GUINT64_FORMAT,
      p.id, hdr.pts, GST_BUFFER_PTS (buffer), GST_ELEMENT_CAST (this)->base_time);

  if (!pinos_port_send_buffer (this->port, &pbuf, &error)) {
    GST_WARNING ("send failed: %s", error->message);
    g_clear_error (&error);
  }
  pinos_buffer_steal_fds (&pbuf, NULL);
  pinos_buffer_unref (&pbuf);

  gst_memory_unref(fdmem);

  return GST_FLOW_OK;
}

static GstFlowReturn
gst_pinos_port_sink_render (GstBaseSink * bsink, GstBuffer * buffer)
{
  GstPinosPortSink *this = GST_PINOS_PORT_SINK (bsink);

  if (this->pinos_input)
    return gst_pinos_port_sink_render_pinos (this, buffer);
  else
    return gst_pinos_port_sink_render_other (this, buffer);
}

static void
gst_pinos_port_sink_finalize (GObject * object)
{
  G_OBJECT_CLASS (parent_class)->finalize (object);
}

static void
gst_pinos_port_sink_class_init (GstPinosPortSinkClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *gstelement_class;
  GstBaseSinkClass *gstbasesink_class;

  gobject_class = (GObjectClass *) klass;
  gstelement_class = (GstElementClass *) klass;
  gstbasesink_class = (GstBaseSinkClass *) klass;

  gobject_class->finalize = gst_pinos_port_sink_finalize;
  gobject_class->set_property = gst_pinos_port_sink_set_property;
  gobject_class->get_property = gst_pinos_port_sink_get_property;

  g_object_class_install_property (gobject_class, PROP_PORT,
      g_param_spec_object ("port", "Port",
          "The pinos port object", PINOS_TYPE_PORT,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  gst_element_class_set_static_metadata (gstelement_class,
      "Pinos Port sink", "Sink/Video",
      "Send data to pinos port", "Wim Taymans <wim.taymans@gmail.com>");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_pinos_port_sink_template));

  gstbasesink_class->get_caps = gst_pinos_port_sink_getcaps;
  gstbasesink_class->set_caps = gst_pinos_port_sink_setcaps;
  gstbasesink_class->propose_allocation = gst_pinos_port_sink_propose_allocation;
  gstbasesink_class->render = gst_pinos_port_sink_render;

  GST_DEBUG_CATEGORY_INIT (pinos_port_sink_debug, "pinosportsink", 0,
      "Pinos Socket Sink");
}

static void
gst_pinos_port_sink_init (GstPinosPortSink * this)
{
  this->allocator = gst_tmpfile_allocator_new ();
  this->fdmanager = pinos_fd_manager_get (PINOS_FD_MANAGER_DEFAULT);
}
