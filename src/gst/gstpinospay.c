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
 * SECTION:element-gstpinospay
 *
 * The pinospay element converts regular GStreamer buffers into the format
 * expected by Pinos.
 *
 * <refsect2>
 * <title>Example launch line</title>
 * |[
 * gst-launch-1.0 -v videotestsrc ! video/x-raw,format=RGB,width=1920,height=1080 \
 *         ! pinospay ! fdsink fd=1 \
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
#include "gstpinospay.h"
#include "gsttmpfileallocator.h"

#include <gst/net/gstnetcontrolmessagemeta.h>
#include <gio/gunixfdmessage.h>

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <client/pinos.h>

GST_DEBUG_CATEGORY_STATIC (gst_pinos_pay_debug_category);
#define GST_CAT_DEFAULT gst_pinos_pay_debug_category

/* prototypes */

/* pad templates */
static GstStaticPadTemplate gst_pinos_pay_src_template =
GST_STATIC_PAD_TEMPLATE ("src",
    GST_PAD_SRC,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS ("application/x-pinos"));

static GstStaticPadTemplate gst_pinos_pay_sink_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS_ANY);

/* class initialization */

G_DEFINE_TYPE_WITH_CODE (GstPinosPay, gst_pinos_pay, GST_TYPE_ELEMENT,
    GST_DEBUG_CATEGORY_INIT (gst_pinos_pay_debug_category, "pinospay", 0,
        "debug category for pinospay element"));

/* propose allocation query parameters for input buffers */
static gboolean
gst_pinos_pay_query (GstPad * pad, GstObject * parent, GstQuery * query)
{
  GstPinosPay *pay = GST_PINOS_PAY (parent);
  gboolean res = FALSE;

  switch (GST_QUERY_TYPE (query)) {
    case GST_QUERY_ALLOCATION:
    {
      GST_DEBUG_OBJECT (pay, "propose_allocation");
      gst_query_add_allocation_param (query, pay->allocator, NULL);
      res = TRUE;
      break;
    }
    default:
      res = gst_pad_query_default (pad, parent, query);
      break;
  }
  return res;
}

static gboolean
gst_pinos_pay_sink_event (GstPad * pad, GstObject * parent, GstEvent * event)
{
  GstPinosPay *pay = GST_PINOS_PAY (parent);
  gboolean res = FALSE;

  switch (GST_EVENT_TYPE (event)) {
    case GST_EVENT_CAPS:
    {
      GstCaps *caps;

      caps = gst_caps_new_empty_simple ("application/x-pinos");
      res = gst_pad_push_event (pay->srcpad, gst_event_new_caps (caps));
      gst_caps_unref (caps);
      gst_event_unref (event);
      break;
    }
    default:
      res = gst_pad_event_default (pad, parent, event);
      break;
  }
  return res;
}

static GstMemory *
gst_pinos_pay_get_fd_memory (GstPinosPay * tmpfilepay, GstBuffer * buffer)
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
gst_pinos_pay_chain (GstPad * pad, GstObject * parent, GstBuffer * buffer)
{
  GstPinosPay *pay = GST_PINOS_PAY (parent);
  GstMemory *fdmem = NULL;
  GError *err = NULL;
  GstBuffer *outbuf;
  PinosBuffer pbuf;
  PinosBufferBuilder builder;
  PinosBufferHeader hdr;
  PinosPacketFDPayload p;
  gsize size;
  gpointer data;
  GSocketControlMessage *msg;

  hdr.flags = 0;
  hdr.seq = GST_BUFFER_OFFSET (buffer);
  hdr.pts = GST_BUFFER_PTS (buffer) + GST_ELEMENT_CAST (pay)->base_time;
  hdr.dts_offset = 0;

  pinos_buffer_builder_init (&builder);
  pinos_buffer_builder_set_header (&builder, &hdr);

  fdmem = gst_pinos_pay_get_fd_memory (pay, buffer);
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

  data = pinos_buffer_steal (&pbuf, &size, &msg);

  outbuf = gst_buffer_new_wrapped (data, size);
  GST_BUFFER_PTS (outbuf) = GST_BUFFER_PTS (buffer);
  GST_BUFFER_DTS (outbuf) = GST_BUFFER_DTS (buffer);
  GST_BUFFER_DURATION (outbuf) = GST_BUFFER_DURATION (buffer);
  GST_BUFFER_OFFSET (outbuf) = GST_BUFFER_OFFSET (buffer);
  GST_BUFFER_OFFSET_END (outbuf) = GST_BUFFER_OFFSET_END (buffer);
  gst_buffer_unref (buffer);

  gst_buffer_add_net_control_message_meta (outbuf, msg);
  g_object_unref (msg);

  return gst_pad_push (pay->srcpad, outbuf);

  /* ERRORS */
add_fd_failed:
  {
    GST_WARNING_OBJECT (pay, "Adding fd failed: %s", err->message);
    gst_memory_unref(fdmem);
    g_clear_error (&err);

    return GST_FLOW_ERROR;
  }
}
static void
gst_pinos_pay_finalize (GObject * object)
{
  GstPinosPay *pay = GST_PINOS_PAY (object);

  GST_DEBUG_OBJECT (pay, "finalize");

  g_object_unref (pay->allocator);

  G_OBJECT_CLASS (gst_pinos_pay_parent_class)->finalize (object);
}

static void
gst_pinos_pay_init (GstPinosPay * pay)
{
  pay->srcpad = gst_pad_new_from_static_template (&gst_pinos_pay_src_template, "src");
  gst_element_add_pad (GST_ELEMENT (pay), pay->srcpad);

  pay->sinkpad = gst_pad_new_from_static_template (&gst_pinos_pay_sink_template, "sink");
  gst_pad_set_chain_function (pay->sinkpad, gst_pinos_pay_chain);
  gst_pad_set_event_function (pay->sinkpad, gst_pinos_pay_sink_event);
  gst_pad_set_query_function (pay->sinkpad, gst_pinos_pay_query);
  gst_element_add_pad (GST_ELEMENT (pay), pay->sinkpad);

  pay->allocator = gst_tmpfile_allocator_new ();
}

static void
gst_pinos_pay_class_init (GstPinosPayClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);

  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_pinos_pay_src_template));
  gst_element_class_add_pad_template (element_class,
      gst_static_pad_template_get (&gst_pinos_pay_sink_template));

  gst_element_class_set_static_metadata (element_class,
      "Pinos Payloader", "Generic",
      "Pinos Payloader for zero-copy IPC with Pinos",
      "Wim Taymans <wim.taymans@gmail.com>");

  gobject_class->finalize = gst_pinos_pay_finalize;
}
