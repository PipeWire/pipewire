/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef __GST_PIPEWIRE_SRC_H__
#define __GST_PIPEWIRE_SRC_H__

#include "config.h"

#include "gstpipewirestream.h"

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>

#include <gst/video/video.h>

#include <pipewire/pipewire.h>
#include <gst/gstpipewirepool.h>
#include <gst/gstpipewirecore.h>

G_BEGIN_DECLS

#define GST_TYPE_PIPEWIRE_SRC \
  (gst_pipewire_src_get_type())
#define GST_PIPEWIRE_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PIPEWIRE_SRC,GstPipeWireSrc))
#define GST_PIPEWIRE_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PIPEWIRE_SRC,GstPipeWireSrcClass))
#define GST_IS_PIPEWIRE_SRC(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PIPEWIRE_SRC))
#define GST_IS_PIPEWIRE_SRC_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PIPEWIRE_SRC))
#define GST_PIPEWIRE_SRC_CAST(obj) \
  ((GstPipeWireSrc *) (obj))

typedef struct _GstPipeWireSrc GstPipeWireSrc;
typedef struct _GstPipeWireSrcClass GstPipeWireSrcClass;

/**
 * GstPipeWireSrc:
 *
 * Opaque data structure.
 */
struct _GstPipeWireSrc {
  GstPushSrc element;

  GstPipeWireStream *stream;

  /*< private >*/
  gboolean always_copy;
  gint min_buffers;
  gint max_buffers;
  gboolean resend_last;
  gint keepalive_time;
  gboolean autoconnect;

  GstCaps *caps;
  GstCaps *possible_caps;

  gboolean is_video;
  GstVideoInfo video_info;
#ifdef HAVE_GSTREAMER_DMA_DRM
  GstVideoInfoDmaDrm drm_info;
#endif

  gboolean negotiated;
  gboolean flushing;
  gboolean started;
  gboolean eos;

  gboolean is_live;
  int64_t delay;
  GstClockTime min_latency;
  GstClockTime max_latency;

  GstBuffer *last_buffer;

  enum spa_meta_videotransform_value transform_value;
};

struct _GstPipeWireSrcClass {
  GstPushSrcClass parent_class;
};

GType gst_pipewire_src_get_type (void);

G_END_DECLS

#endif /* __GST_PIPEWIRE_SRC_H__ */
