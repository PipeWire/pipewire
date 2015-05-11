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

#ifndef __GST_PULSEVIDEO_SINK_H__
#define __GST_PULSEVIDEO_SINK_H__

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>

#include <gst/video/video.h>

#include <client/pv-context.h>
#include <client/pv-stream.h>
#include <client/pv-introspect.h>

G_BEGIN_DECLS

#define GST_TYPE_PULSEVIDEO_SINK \
  (gst_pulsevideo_sink_get_type())
#define GST_PULSEVIDEO_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PULSEVIDEO_SINK,GstPulsevideoSink))
#define GST_PULSEVIDEO_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PULSEVIDEO_SINK,GstPulsevideoSinkClass))
#define GST_IS_PULSEVIDEO_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PULSEVIDEO_SINK))
#define GST_IS_PULSEVIDEO_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PULSEVIDEO_SINK))
#define GST_PULSEVIDEO_SINK_CAST(obj) \
  ((GstPulsevideoSink *) (obj))

typedef struct _GstPulsevideoSink GstPulsevideoSink;
typedef struct _GstPulsevideoSinkClass GstPulsevideoSinkClass;

/**
 * GstPulsevideoSink:
 *
 * Opaque data structure.
 */
struct _GstPulsevideoSink {
  GstBaseSink element;

  /*< private >*/

  /* video state */
  GstVideoInfo info;

  GMainContext *context;
  GMainLoop *loop;
  GThread *thread;
  PvContext *ctx;
  PvStream *stream;
  GstAllocator *allocator;

  GMutex lock;
  GCond cond;
};

struct _GstPulsevideoSinkClass {
  GstBaseSinkClass parent_class;
};

GType gst_pulsevideo_sink_get_type (void);

G_END_DECLS

#endif /* __GST_PULSEVIDEO_SINK_H__ */
