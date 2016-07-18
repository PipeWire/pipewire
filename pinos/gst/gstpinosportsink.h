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

#ifndef __GST_PINOS_PORT_SINK_H__
#define __GST_PINOS_PORT_SINK_H__

#include <gio/gio.h>

#include <client/pinos.h>
#include <server/port.h>

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>

G_BEGIN_DECLS

#define GST_TYPE_PINOS_PORT_SINK            (gst_pinos_port_sink_get_type())
#define GST_PINOS_PORT_SINK(obj)            (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PINOS_PORT_SINK,GstPinosPortSink))
#define GST_PINOS_PORT_SINK_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PINOS_PORT_SINK,GstPinosPortSinkClass))
#define GST_IS_PINOS_PORT_SINK(obj)         (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PINOS_PORT_SINK))
#define GST_IS_PINOS_PORT_SINK_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PINOS_PORT_SINK))
#define GST_PINOS_PORT_SINK_CAST(obj)       ((GstPinosPortSink *) (obj))

typedef struct _GstPinosPortSink GstPinosPortSink;
typedef struct _GstPinosPortSinkClass GstPinosPortSinkClass;

/**
 * GstPinosPortSink:
 *
 * Opaque data structure.
 */
struct _GstPinosPortSink {
  GstBaseSink element;

  gboolean pinos_input;
  GstAllocator *allocator;

  PinosPort *port;
  PinosFdManager *fdmanager;
};

struct _GstPinosPortSinkClass {
  GstBaseSinkClass parent_class;
};

GType gst_pinos_port_sink_get_type (void);

G_END_DECLS

#endif /* __GST_PINOS_PORT_SINK_H__ */
