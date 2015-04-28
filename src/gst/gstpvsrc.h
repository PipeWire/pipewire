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

#ifndef __GST_PULSEVIDEO_SRC_H__
#define __GST_PULSEVIDEO_SRC_H__

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>

#include <gst/video/video.h>

#include <client/pv-context.h>
#include <client/pv-stream.h>

G_BEGIN_DECLS

#define GST_TYPE_PULSEVIDEO_SRC \
  (gst_pulsevideo_src_get_type())
#define GST_PULSEVIDEO_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PULSEVIDEO_SRC,GstPulsevideoSrc))
#define GST_PULSEVIDEO_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PULSEVIDEO_SRC,GstPulsevideoSrcClass))
#define GST_IS_PULSEVIDEO_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PULSEVIDEO_SRC))
#define GST_IS_PULSEVIDEO_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PULSEVIDEO_SRC))
#define GST_PULSEVIDEO_SRC_CAST(obj) \
  ((GstPulsevideoSrc *) (obj))

typedef struct _GstPulsevideoSrc GstPulsevideoSrc;
typedef struct _GstPulsevideoSrcClass GstPulsevideoSrcClass;

/**
 * GstPulsevideoSrc:
 *
 * Opaque data structure.
 */
struct _GstPulsevideoSrc {
  GstPushSrc element;

  /*< private >*/

  /* video state */
  GstVideoInfo info;

  GMainContext *context;
  GMainLoop *loop;
  GThread *thread;
  PvContext *ctx;
  PvStream *stream;
  GstAllocator *fd_allocator;
};

struct _GstPulsevideoSrcClass {
  GstPushSrcClass parent_class;
};

GType gst_pulsevideo_src_get_type (void);

G_END_DECLS

#endif /* __GST_PULSEVIDEO_SRC_H__ */
