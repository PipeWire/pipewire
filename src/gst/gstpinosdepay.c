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

/* prototypes */


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

G_DEFINE_TYPE_WITH_CODE (GstPinosDepay, gst_pinos_depay, GST_TYPE_ELEMENT,
    GST_DEBUG_CATEGORY_INIT (gst_pinos_depay_debug_category, "pinosdepay", 0,
        "debug category for pinosdepay element"));

static GstFlowReturn
gst_pinos_depay_chain (GstPad *pad, GstObject * parent, GstBuffer * buffer)
{
  GstPinosDepay *depay = GST_PINOS_DEPAY (parent);
  GstBuffer *outbuf;
  GstMapInfo info;
  PinosBuffer pbuf;
  PinosBufferIter it;
  GstNetControlMessageMeta * meta;
  GSocketControlMessage *msg = NULL;
  const PinosBufferHeader *hdr;
  GError *err = NULL;

  meta = ((GstNetControlMessageMeta*) gst_buffer_get_meta (
      buffer, GST_NET_CONTROL_MESSAGE_META_API_TYPE));
  if (meta) {
    msg = meta->message;
    gst_buffer_remove_meta (buffer, (GstMeta *) meta);
    meta = NULL;
  }

  outbuf = gst_buffer_new ();

  gst_buffer_map (buffer, &info, GST_MAP_READ);
  pinos_buffer_init_data (&pbuf, info.data, info.size, msg);

  pinos_buffer_iter_init (&it, &pbuf);
  while (pinos_buffer_iter_next (&it)) {
    switch (pinos_buffer_iter_get_type (&it)) {
      case PINOS_PACKET_TYPE_FD_PAYLOAD:
      {
        GstMemory *fdmem = NULL;
        PinosPacketFDPayload p;
        int fd;

        pinos_buffer_iter_parse_fd_payload (&it, &p);
        fd = pinos_buffer_get_fd (&pbuf, p.fd_index, &err);
        if (fd == -1)
          goto error;

        fdmem = gst_fd_allocator_alloc (depay->fd_allocator, fd,
                  p.offset + p.size, GST_FD_MEMORY_FLAG_NONE);
        gst_memory_resize (fdmem, p.offset, p.size);
        gst_buffer_append_memory (outbuf, fdmem);
        break;
      }
      default:
        break;
    }
  }
  hdr = pinos_buffer_get_header (&pbuf, NULL);
  GST_BUFFER_OFFSET (buffer) = hdr->seq;
  pinos_buffer_clear (&pbuf);
  gst_buffer_unmap (buffer, &info);
  gst_buffer_unref (buffer);

  return gst_pad_push (depay->srcpad, outbuf);

error:
  {
    GST_ELEMENT_ERROR (depay, RESOURCE, SETTINGS, (NULL),
        ("can't get fd: %s", err->message));
    g_clear_error (&err);
    return GST_FLOW_ERROR;
  }
}

static void
gst_pinos_depay_finalize (GObject * object)
{
  GstPinosDepay *depay = GST_PINOS_DEPAY (object);

  GST_DEBUG_OBJECT (depay, "finalize");

  g_object_unref (depay->fd_allocator);

  G_OBJECT_CLASS (gst_pinos_depay_parent_class)->finalize (object);
}

static void
gst_pinos_depay_class_init (GstPinosDepayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class =
      GST_ELEMENT_CLASS (klass);

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

  gobject_class->finalize = gst_pinos_depay_finalize;
}

static void
gst_pinos_depay_init (GstPinosDepay * depay)
{
  depay->srcpad = gst_pad_new_from_static_template (&gst_pinos_depay_src_template, "src");
  gst_element_add_pad (GST_ELEMENT (depay), depay->srcpad);

  depay->sinkpad = gst_pad_new_from_static_template (&gst_pinos_depay_sink_template, "sink");
  gst_pad_set_chain_function (depay->sinkpad, gst_pinos_depay_chain);
  gst_element_add_pad (GST_ELEMENT (depay), depay->sinkpad);

  depay->fd_allocator = gst_fd_allocator_new ();
}

