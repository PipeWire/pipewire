/* GStreamer
 * Copyright (C) 2014 William Manley <will@williammanley.net>
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
 * SECTION:element-gstfddepay
 *
 * The fddepay element does FIXME stuff.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch -v fakesrc ! fddepay ! FIXME ! fakesink
 * ]|
 * FIXME Describe what the pipeline does.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstfddepay.h"
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

GST_DEBUG_CATEGORY_STATIC (gst_fddepay_debug_category);
#define GST_CAT_DEFAULT gst_fddepay_debug_category

/* prototypes */


static GstCaps *gst_fddepay_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static void gst_fddepay_dispose (GObject * object);

static GstFlowReturn gst_fddepay_transform_ip (GstBaseTransform * trans,
    GstBuffer * buf);

/* pad templates */

static GstStaticCaps fd_caps = GST_STATIC_CAPS ("application/x-fd");

static GstStaticPadTemplate gst_fddepay_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

static GstStaticPadTemplate gst_fddepay_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-fd"));


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstFddepay, gst_fddepay, GST_TYPE_BASE_TRANSFORM,
    GST_DEBUG_CATEGORY_INIT (gst_fddepay_debug_category, "fddepay", 0,
        "debug category for fddepay element"));

static void
gst_fddepay_class_init (GstFddepayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_static_pad_template_get (&gst_fddepay_src_template));
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_static_pad_template_get (&gst_fddepay_sink_template));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Simple FD Deplayloder", "Generic",
      "Simple File-descriptor Depayloader for zero-copy video IPC",
      "William Manley <will@williammanley.net>");

  gobject_class->dispose = gst_fddepay_dispose;
  base_transform_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_fddepay_transform_caps);
  base_transform_class->transform_ip =
      GST_DEBUG_FUNCPTR (gst_fddepay_transform_ip);

}

static void
gst_fddepay_init (GstFddepay * fddepay)
{
  fddepay->fd_allocator = gst_fd_allocator_new ();
}

void
gst_fddepay_dispose (GObject * object)
{
  GstFddepay *fddepay = GST_FDDEPAY (object);

  GST_DEBUG_OBJECT (fddepay, "dispose");

  /* clean up as possible.  may be called multiple times */
  if (fddepay->fd_allocator != NULL) {
    g_object_unref (G_OBJECT (fddepay->fd_allocator));
    fddepay->fd_allocator = NULL;
  }

  G_OBJECT_CLASS (gst_fddepay_parent_class)->dispose (object);
}

static GstCaps *
gst_fddepay_transform_caps (GstBaseTransform * trans, GstPadDirection direction,
    GstCaps * caps, GstCaps * filter)
{
  GstFddepay *fddepay = GST_FDDEPAY (trans);
  GstCaps *othercaps;

  GST_DEBUG_OBJECT (fddepay, "transform_caps");


  if (direction == GST_PAD_SRC) {
    /* transform caps going upstream */
    othercaps = gst_static_caps_get (&fd_caps);
  } else {
    /* transform caps going downstream */
    othercaps = gst_caps_new_any ();
  }

  if (filter) {
    GstCaps *intersect;

    intersect = gst_caps_intersect (othercaps, filter);
    gst_caps_unref (othercaps);

    return intersect;
  } else {
    return othercaps;
  }
}

static GstFlowReturn
gst_fddepay_transform_ip (GstBaseTransform * trans, GstBuffer * buf)
{
  GstFddepay *fddepay = GST_FDDEPAY (trans);
  PinosBuffer pbuf;
  PinosPacketIter it;
  GstNetControlMessageMeta * meta;
  GSocketControlMessage *msg = NULL;
  const PinosBufferHeader *hdr;
  gpointer data;
  gsize size;
  GError *err = NULL;

  GST_DEBUG_OBJECT (fddepay, "transform_ip");

  gst_buffer_extract_dup (buf, 0, gst_buffer_get_size (buf), &data, &size);

  meta = ((GstNetControlMessageMeta*) gst_buffer_get_meta (
      buf, GST_NET_CONTROL_MESSAGE_META_API_TYPE));

  if (meta) {
    msg = meta->message;
    gst_buffer_remove_meta (buf, (GstMeta *) meta);
    meta = NULL;
  }

  pinos_buffer_init_take_data (&pbuf, data, size, msg);

  gst_buffer_remove_all_memory (buf);

  pinos_packet_iter_init (&it, &pbuf);
  while (pinos_packet_iter_next (&it)) {
    switch (pinos_packet_iter_get_type (&it)) {
      case PINOS_PACKET_TYPE_FD_PAYLOAD:
      {
        GstMemory *fdmem = NULL;
        PinosPacketFDPayload p;
        int fd;

        pinos_packet_iter_parse_fd_payload (&it, &p);
        fd = pinos_buffer_get_fd (&pbuf, p.fd_index, &err);
        if (fd == -1)
          goto error;

        fdmem = gst_fd_allocator_alloc (fddepay->fd_allocator, fd,
                  p.offset + p.size, GST_FD_MEMORY_FLAG_NONE);
        gst_memory_resize (fdmem, p.offset, p.size);
        gst_buffer_append_memory (buf, fdmem);
        break;
      }
      default:
        break;
    }
  }
  hdr = pinos_buffer_get_header (&pbuf, NULL);
  GST_BUFFER_OFFSET (buf) = hdr->seq;

  return GST_FLOW_OK;

error:
  {
    GST_ELEMENT_ERROR (fddepay, RESOURCE, SETTINGS, (NULL),
        ("can't get fd: %s", err->message));
    g_clear_error (&err);
    return GST_FLOW_ERROR;
  }
}
