/* GStreamer
 * Copyright (C) 2014 William Manley <will@williammanley.net>
 *           (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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
 * Free Software Foundation, Inc., 51 Franklin Street, Suite 500,
 * Boston, MA 02110-1335, USA.
 */
/**
 * SECTION:element-gstpinosdepay
 *
 * The pinosdepay element does FIXME stuff.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v fakesrc ! pinosdepay ! FIXME ! fakesink
 * ]|
 * FIXME Describe what the pipeline does.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstpinosdepay.h"
#include <gst/net/gstnetcontrolmessagemeta.h>

#include <gst/gst.h>
#include <gst/base/gstbasetransform.h>
#include <gst/allocators/gstfdmemory.h>
#include <gio/gunixfdmessage.h>

#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <client/pinos.h>

GST_DEBUG_CATEGORY_STATIC (gst_pinos_depay_debug_category);
#define GST_CAT_DEFAULT gst_pinos_depay_debug_category

static GQuark fdids_quark;

enum
{
  PROP_0,
  PROP_CAPS,
};

/* pad templates */
static GstStaticPadTemplate gst_pinos_depay_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate gst_pinos_depay_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-pinos"));


/* class initialization */
G_DEFINE_TYPE (GstPinosDepay, gst_pinos_depay, GST_TYPE_ELEMENT);

static gboolean
gst_pinos_depay_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstPinosDepay *depay = GST_PINOS_DEPAY (parent);
  gboolean res = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_SEGMENT:
    {
      GstSegment segment;

      gst_segment_init (&segment, GST_FORMAT_TIME);

      res = gst_pad_push_event (depay->srcpad, gst_event_new_segment (&segment));
      break;
    }
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;
      GstStructure *str;

      gst_event_parse_caps (event, &caps);
      str = gst_caps_get_structure (caps, 0);
      depay->pinos_input = gst_structure_has_name (str, "application/x-pinos");
      gst_event_unref (event);

      res = gst_pad_push_event (depay->srcpad, gst_event_new_caps (depay->caps));
      break;
    }
    default:
      res = gst_pad_event_default (pad, parent, event);
      break;
  }
  return res;
}

static void
reuse_fds (GstPinosDepay *this, GstBuffer *buffer)
{
  GArray *fdids;
  guint i;
  PinosBufferBuilder b;
  PinosPacketReuseMem r;
  PinosBuffer pbuf;
  gsize size;
  gpointer data;
  GstBuffer *outbuf;
  GstEvent *ev;

  fdids = gst_mini_object_steal_qdata (GST_MINI_OBJECT_CAST (buffer),
      fdids_quark);
  if (fdids == NULL)
    return;

  pinos_buffer_builder_init (&b);

  for (i = 0; i < fdids->len; i++) {
    r.id = g_array_index (fdids, guint32, i);
    GST_LOG ("reuse mem id %d", r.id);
    pinos_buffer_builder_add_reuse_mem (&b, &r);
  }
  pinos_buffer_builder_end (&b, &pbuf);
  g_array_unref (fdids);

  data = pinos_buffer_steal_data (&pbuf, &size);

  outbuf = gst_buffer_new_wrapped (data, size);
  ev = gst_event_new_custom (GST_EVENT_CUSTOM_UPSTREAM,
          gst_structure_new ("GstNetworkMessage",
              "object", G_TYPE_OBJECT, this,
              "buffer", GST_TYPE_BUFFER, outbuf, NULL));
  gst_buffer_unref (outbuf);

  gst_pad_push_event (this->sinkpad, ev);
  g_object_unref (this);
}

static GstFlowReturn
gst_pinos_depay_chain (GstPad *pad, GstObject * parent, GstBuffer * buffer)
{
  GstPinosDepay *depay = GST_PINOS_DEPAY (parent);
  GstBuffer *outbuf = NULL;
  GstMapInfo info;
  PinosBuffer pbuf;
  PinosBufferIter it;
  GstNetControlMessageMeta * meta;
  GError *err = NULL;
  GArray *fdids = NULL;
  GUnixFDList *fds = NULL;

  meta = ((GstNetControlMessageMeta*) gst_buffer_get_meta (
      buffer, GST_NET_CONTROL_MESSAGE_META_API_TYPE));
  if (meta) {
    if (G_IS_UNIX_FD_MESSAGE (meta->message)) {
      fds = g_unix_fd_message_get_fd_list (G_UNIX_FD_MESSAGE (meta->message));
    }
  }

  gst_buffer_map (buffer, &info, GST_MAP_READ);
  pinos_buffer_init_data (&pbuf, info.data, info.size, NULL, 0);

  pinos_buffer_iter_init (&it, &pbuf);
  while (pinos_buffer_iter_next (&it)) {
    switch (pinos_buffer_iter_get_type (&it)) {
      case PINOS_PACKET_TYPE_HEADER:
      {
        PinosPacketHeader hdr;

        if (!pinos_buffer_iter_parse_header  (&it, &hdr))
          goto error;

        if (outbuf == NULL)
          outbuf = gst_buffer_new ();

        GST_INFO ("pts %" G_GUINT64_FORMAT ", dts_offset %"G_GUINT64_FORMAT, hdr.pts, hdr.dts_offset);

#if 0
        if (GST_CLOCK_TIME_IS_VALID (hdr.pts)) {
          GST_BUFFER_PTS (outbuf) = hdr.pts;
          if (GST_BUFFER_PTS (outbuf) + hdr.dts_offset > 0)
            GST_BUFFER_DTS (outbuf) = GST_BUFFER_PTS (outbuf) + hdr.dts_offset;
        }
#endif
        GST_BUFFER_OFFSET (outbuf) = hdr.seq;
        break;
      }
      case PINOS_PACKET_TYPE_ADD_MEM:
      {
        GstMemory *fdmem = NULL;
        PinosPacketAddMem p;
        int fd;

        if (!pinos_buffer_iter_parse_add_mem (&it, &p))
          goto error;

        fd = g_unix_fd_list_get (fds, p.fd_index, &err);
        if (fd == -1)
          goto error;

        fdmem = gst_fd_allocator_alloc (depay->fd_allocator, fd,
                  p.offset + p.size, GST_FD_MEMORY_FLAG_NONE);
        gst_memory_resize (fdmem, p.offset, p.size);

        g_hash_table_insert (depay->mem_ids, GINT_TO_POINTER (p.id), fdmem);
        break;
      }
      case PINOS_PACKET_TYPE_REMOVE_MEM:
      {
        PinosPacketRemoveMem p;

        if (!pinos_buffer_iter_parse_remove_mem (&it, &p))
          goto error;

        g_hash_table_remove (depay->mem_ids, GINT_TO_POINTER (p.id));
        break;
      }
      case PINOS_PACKET_TYPE_PROCESS_MEM:
      {
        GstMemory *fdmem = NULL;
        PinosPacketProcessMem p;

        if (!pinos_buffer_iter_parse_process_mem (&it, &p))
          goto error;

        fdmem = g_hash_table_lookup (depay->mem_ids, GINT_TO_POINTER (p.id));
        if (fdmem == NULL)
          goto error;

        if (outbuf == NULL)
          outbuf = gst_buffer_new ();

        fdmem = gst_memory_share (fdmem, p.offset, p.size);
        gst_buffer_append_memory (outbuf, fdmem);

        if (fdids == NULL)
          fdids = g_array_new (FALSE, FALSE, sizeof (guint32));

        GST_LOG ("track fd index %d", p.id);
        g_array_append_val (fdids, p.id);
        break;
      }
      case PINOS_PACKET_TYPE_FORMAT_CHANGE:
      {
        PinosPacketFormatChange change;
        GstCaps *caps;

        if (!pinos_buffer_iter_parse_format_change (&it, &change))
          goto error;
        GST_DEBUG ("got format change %d %s", change.id, change.format);

        caps = gst_caps_from_string (change.format);
        if (caps) {
          gst_caps_take (&depay->caps, caps);
          gst_pad_push_event (depay->srcpad, gst_event_new_caps (depay->caps));
        }
        break;
      }
      default:
        break;
    }
  }
  pinos_buffer_iter_init (&it, &pbuf);
  pinos_buffer_unref (&pbuf);
  gst_buffer_unmap (buffer, &info);
  gst_buffer_unref (buffer);

  if (outbuf) {
    if (fdids != NULL) {
      gst_mini_object_set_qdata (GST_MINI_OBJECT_CAST (outbuf),
          fdids_quark, fdids, NULL);
      gst_mini_object_weak_ref (GST_MINI_OBJECT_CAST (outbuf),
          (GstMiniObjectNotify) reuse_fds, g_object_ref (depay));
    }
    return gst_pad_push (depay->srcpad, outbuf);
  }
  else
    return GST_FLOW_OK;

error:
  {
    GST_ELEMENT_ERROR (depay, RESOURCE, SETTINGS, (NULL),
        ("can't get fd: %s", err->message));
    g_clear_error (&err);
    gst_buffer_unref (outbuf);
    return GST_FLOW_ERROR;
  }
}

static void
gst_pinos_depay_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  GstPinosDepay *depay = GST_PINOS_DEPAY (object);

  switch (prop_id) {
    case PROP_CAPS:
    {
      const GstCaps *caps;

      caps = gst_value_get_caps (value);
      gst_caps_replace (&depay->caps, (GstCaps *)caps);
      break;
    }

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pinos_depay_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  GstPinosDepay *depay = GST_PINOS_DEPAY (object);

  switch (prop_id) {
    case PROP_CAPS:
      gst_value_set_caps (value, depay->caps);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_pinos_depay_finalize (GObject * object)
{
  GstPinosDepay *depay = GST_PINOS_DEPAY (object);

  GST_DEBUG_OBJECT (depay, "finalize");

  gst_caps_replace (&depay->caps, NULL);
  g_object_unref (depay->fd_allocator);
  g_hash_table_unref (depay->mem_ids);

  G_OBJECT_CLASS (gst_pinos_depay_parent_class)->finalize (object);
}

static void
gst_pinos_depay_class_init (GstPinosDepayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class =
      GST_ELEMENT_CLASS (klass);

  gobject_class->finalize = gst_pinos_depay_finalize;
  gobject_class->set_property = gst_pinos_depay_set_property;
  gobject_class->get_property = gst_pinos_depay_get_property;

  g_object_class_install_property (gobject_class, PROP_CAPS,
      g_param_spec_boxed ("caps", "Caps",
          "The caps of the source pad", GST_TYPE_CAPS,
          G_PARAM_READWRITE | G_PARAM_STATIC_STRINGS));

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_pinos_depay_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_pinos_depay_sink_template));

  gst_element_class_set_static_metadata (element_class,
      "Pinos Deplayloder", "Generic",
      "Pinos Depayloader for zero-copy IPC via Pinos",
      "Wim Taymans <wim.taymans@gmail.com>");

  GST_DEBUG_CATEGORY_INIT (gst_pinos_depay_debug_category, "pinosdepay", 0,
      "debug category for pinosdepay element");

  fdids_quark = g_quark_from_static_string ("GstPinosDepayFDIds");
}

static void
gst_pinos_depay_init (GstPinosDepay * depay)
{
  depay->srcpad = gst_pad_new_from_static_template (&gst_pinos_depay_src_template, "src");
  gst_element_add_pad (GST_ELEMENT (depay), depay->srcpad);

  depay->sinkpad = gst_pad_new_from_static_template (&gst_pinos_depay_sink_template, "sink");
  gst_pad_set_chain_function (depay->sinkpad, gst_pinos_depay_chain);
  gst_pad_set_event_function (depay->sinkpad, gst_pinos_depay_sink_event);
  gst_element_add_pad (GST_ELEMENT (depay), depay->sinkpad);

  depay->fd_allocator = gst_fd_allocator_new ();
  depay->mem_ids = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, (GDestroyNotify) gst_memory_unref);
}
