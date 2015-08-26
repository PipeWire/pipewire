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
 * SECTION:element-gstfdpay
 *
 * The tmpfilepay element enables zero-copy passing of buffers between
 * processes by allocating memory in a temporary file.  This is a proof of
 * concept example.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 -v videotestsrc ! video/x-raw,format=RGB,width=1920,height=1080 \
 *         ! fdpay ! fdsink fd=1 \
 *     | gst-launch-1.0 fdsrc fd=0 ! fddepay \
 *         ! video/x-raw,format=RGB,width=1920,height=1080 ! autovideosink
 * ]|
 * Video frames are created in the first gst-launch-1.0 process and displayed
 * by the second with no copying.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <gst/gst.h>
#include <gst/allocators/gstfdmemory.h>
#include <gst/base/gstbasetransform.h>
#include "gstfdpay.h"
#include "gsttmpfileallocator.h"

#include <gst/net/gstnetcontrolmessagemeta.h>
#include <gio/gunixfdmessage.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <client/pinos.h>

GST_DEBUG_CATEGORY_STATIC (gst_fdpay_debug_category);
#define GST_CAT_DEFAULT gst_fdpay_debug_category

#define GST_UNREF(x) \
  do { \
    if ( x ) \
      gst_object_unref ( x ); \
    x = NULL; \
  } while (0);


/* prototypes */

static void gst_fdpay_dispose (GObject * object);

static GstCaps *gst_fdpay_transform_caps (GstBaseTransform * trans,
    GstPadDirection direction, GstCaps * caps, GstCaps * filter);
static gboolean gst_fdpay_transform_size (GstBaseTransform *trans,
    GstPadDirection direction, GstCaps *caps, gsize size, GstCaps *othercaps,
    gsize *othersize);
static gboolean gst_fdpay_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query);
static GstFlowReturn gst_fdpay_transform (GstBaseTransform * trans,
    GstBuffer * inbuf, GstBuffer * outbuf);

/* pad templates */

static GstStaticCaps fd_caps = GST_STATIC_CAPS ("application/x-fd");

static GstStaticPadTemplate gst_fdpay_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-fd"));

static GstStaticPadTemplate gst_fdpay_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);


/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstFdpay, gst_fdpay, GST_TYPE_BASE_TRANSFORM,
    GST_DEBUG_CATEGORY_INIT (gst_fdpay_debug_category, "fdpay", 0,
        "debug category for fdpay element"));

static void
gst_fdpay_class_init (GstFdpayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstBaseTransformClass *base_transform_class =
      GST_BASE_TRANSFORM_CLASS (klass);

  /* Setting up pads and setting metadata should be moved to
     base_class_init if you intend to subclass this class. */
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_static_pad_template_get (&gst_fdpay_src_template));
  gst_element_class_add_pad_template (GST_ELEMENT_CLASS (klass),
      gst_static_pad_template_get (&gst_fdpay_sink_template));

  gst_element_class_set_static_metadata (GST_ELEMENT_CLASS (klass),
      "Simple FD Payloader", "Generic",
      "Simple File-descriptor Payloader for zero-copy video IPC",
      "William Manley <will@williammanley.net>");

  gobject_class->dispose = gst_fdpay_dispose;
  base_transform_class->transform_caps =
      GST_DEBUG_FUNCPTR (gst_fdpay_transform_caps);
  base_transform_class->propose_allocation =
      GST_DEBUG_FUNCPTR (gst_fdpay_propose_allocation);
  base_transform_class->transform =
      GST_DEBUG_FUNCPTR (gst_fdpay_transform);
  base_transform_class->transform_size =
      GST_DEBUG_FUNCPTR (gst_fdpay_transform_size);
}

static void
gst_fdpay_init (GstFdpay * fdpay)
{
  fdpay->allocator = gst_tmpfile_allocator_new ();
}

void
gst_fdpay_dispose (GObject * object)
{
  GstFdpay *fdpay = GST_FDPAY (object);

  GST_DEBUG_OBJECT (fdpay, "dispose");

  /* clean up as possible.  may be called multiple times */
  GST_UNREF(fdpay->allocator);

  G_OBJECT_CLASS (gst_fdpay_parent_class)->dispose (object);
}

static GstCaps *
gst_fdpay_transform_caps (GstBaseTransform * trans, GstPadDirection direction,
    GstCaps * caps, GstCaps * filter)
{
  GstFdpay *fdpay = GST_FDPAY (trans);
  GstCaps *othercaps;

  GST_DEBUG_OBJECT (fdpay, "transform_caps");


  if (direction == GST_PAD_SRC) {
    /* transform caps going upstream */
    othercaps = gst_caps_new_any ();
  } else {
    /* transform caps going downstream */
    othercaps = gst_static_caps_get (&fd_caps);
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

static gboolean
gst_fdpay_transform_size (GstBaseTransform *trans, GstPadDirection direction,
    GstCaps *caps, gsize size, GstCaps *othercaps, gsize *othersize)
{
  if (direction == GST_PAD_SRC) {
    /* transform size going upstream - don't know how to do this */
    return FALSE;
  } else {
    /* transform size going downstream */
    *othersize = sizeof (PinosBuffer) + 30;
  }

  return TRUE;
}

/* propose allocation query parameters for input buffers */
static gboolean
gst_fdpay_propose_allocation (GstBaseTransform * trans,
    GstQuery * decide_query, GstQuery * query)
{
  GstFdpay *fdpay = GST_FDPAY (trans);

  GST_DEBUG_OBJECT (fdpay, "propose_allocation");

  gst_query_add_allocation_param (query, fdpay->allocator, NULL);

  return TRUE;
}

static GstMemory *
gst_fdpay_get_fd_memory (GstFdpay * tmpfilepay, GstBuffer * buffer)
{
  GstMemory *mem = NULL;

  if (gst_buffer_n_memory (buffer) == 1
      && gst_is_fd_memory (gst_buffer_peek_memory (buffer, 0)))
    mem = gst_buffer_get_memory (buffer, 0);
  else {
    GstMapInfo info;
    GstAllocationParams params = {0, 0, 0, 0, { NULL, }};
    gsize size = gst_buffer_get_size (buffer);
    GST_INFO_OBJECT (tmpfilepay, "Buffer cannot be payloaded without copying");
    mem = gst_allocator_alloc (tmpfilepay->allocator, size, &params);
    if (!gst_memory_map (mem, &info, GST_MAP_WRITE))
      return NULL;
    gst_buffer_extract (buffer, 0, info.data, size);
    gst_memory_unmap (mem, &info);
  }
  return mem;
}

static GstFlowReturn
gst_fdpay_transform (GstBaseTransform * trans, GstBuffer * inbuf,
    GstBuffer * outbuf)
{
  GstFdpay *fdpay = GST_FDPAY (trans);
  GstMemory *fdmem = NULL;
  GstMapInfo info;
  GError *err = NULL;
  PinosBuffer pbuf;
  PinosBufferBuilder builder;
  PinosBufferHeader hdr;
  PinosPacketFDPayload p;
  gsize size;

  GST_DEBUG_OBJECT (fdpay, "transform_ip");

  fdmem = gst_fdpay_get_fd_memory (fdpay, inbuf);

  hdr.flags = 0;
  hdr.seq = GST_BUFFER_OFFSET (inbuf);
  hdr.pts = GST_BUFFER_TIMESTAMP (inbuf) + GST_ELEMENT_CAST (trans)->base_time;
  hdr.dts_offset = 0;

  pinos_buffer_builder_init (&builder);
  pinos_buffer_builder_set_header (&builder, &hdr);

  p.fd_index = pinos_buffer_builder_add_fd (&builder, gst_fd_memory_get_fd (fdmem), &err);
  if (p.fd_index == -1)
    goto add_fd_failed;
  p.id = 0;
  p.offset = fdmem->offset;
  p.size = fdmem->size;
  pinos_buffer_builder_add_fd_payload (&builder, &p);

  pinos_buffer_builder_end (&builder, &pbuf);
  gst_memory_unref(fdmem);
  fdmem = NULL;

  gst_buffer_add_net_control_message_meta (outbuf,
                                           pinos_buffer_get_socket_control_message (&pbuf));

  size = pinos_buffer_get_size (&pbuf);

  gst_buffer_map (outbuf, &info, GST_MAP_WRITE);
  pinos_buffer_store (&pbuf, info.data);
  gst_buffer_unmap (outbuf, &info);
  gst_buffer_resize (outbuf, 0, size);

  pinos_buffer_clear (&pbuf);

  return GST_FLOW_OK;

  /* ERRORS */
add_fd_failed:
  {
    GST_WARNING_OBJECT (trans, "Adding fd failed: %s", err->message);
    gst_memory_unref(fdmem);
    g_clear_error (&err);

    return GST_FLOW_ERROR;
  }
}
