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

#ifndef __GST_PINOS_SINK_H__
#define __GST_PINOS_SINK_H__

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>

#include <client/context.h>
#include <client/stream.h>
#include <client/introspect.h>

G_BEGIN_DECLS

#define GST_TYPE_PINOS_SINK \
  (gst_pinos_sink_get_type())
#define GST_PINOS_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PINOS_SINK,GstPinosSink))
#define GST_PINOS_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PINOS_SINK,GstPinosSinkClass))
#define GST_IS_PINOS_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PINOS_SINK))
#define GST_IS_PINOS_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PINOS_SINK))
#define GST_PINOS_SINK_CAST(obj) \
  ((GstPinosSink *) (obj))

typedef struct _GstPinosSink GstPinosSink;
typedef struct _GstPinosSinkClass GstPinosSinkClass;

/**
 * GstPinosSink:
 *
 * Opaque data structure.
 */
struct _GstPinosSink {
  GstBaseSink element;

  /*< private >*/

  /* video state */
  gboolean negotiated;

  GMainContext *context;
  GMainLoop *loop;
  GThread *thread;
  PinosContext *ctx;
  PinosStream *stream;
  GstAllocator *allocator;

  GPollFunc poll_func;
  GMutex lock;
  GCond cond;
};

struct _GstPinosSinkClass {
  GstBaseSinkClass parent_class;
};

GType gst_pinos_sink_get_type (void);

G_END_DECLS

#endif /* __GST_PINOS_SINK_H__ */
