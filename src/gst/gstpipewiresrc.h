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
#include <spa/param/format.h>
#include <gst/gstpipewirepool.h>
#include <gst/gstpipewirecore.h>

G_BEGIN_DECLS

#define GST_TYPE_PIPEWIRE_SRC (gst_pipewire_src_get_type())
#define GST_PIPEWIRE_SRC_CAST(obj) ((GstPipeWireSrc *) (obj))
G_DECLARE_FINAL_TYPE (GstPipeWireSrc, gst_pipewire_src, GST, PIPEWIRE_SRC, GstPushSrc)

/**
 * GstPipeWireSrcOnDisconnect:
 * @GST_PIPEWIRE_SRC_ON_DISCONNECT_EOS: send EoS downstream
 * @GST_PIPEWIRE_SRC_ON_DISCONNECT_ERROR: raise pipeline error
 * @GST_PIPEWIRE_SRC_ON_DISCONNECT_NONE: no action
 *
 * Different actions on disconnect.
 */
typedef enum
{
  GST_PIPEWIRE_SRC_ON_DISCONNECT_NONE,
  GST_PIPEWIRE_SRC_ON_DISCONNECT_EOS,
  GST_PIPEWIRE_SRC_ON_DISCONNECT_ERROR,
} GstPipeWireSrcOnDisconnect;

#define GST_TYPE_PIPEWIRE_SRC_ON_DISCONNECT (gst_pipewire_src_on_disconnect_get_type ())

/**
 * GstPipeWireSrc:
 *
 * Opaque data structure.
 */
struct _GstPipeWireSrc {
  GstPushSrc element;

  GstPipeWireStream *stream;

  /*< private >*/
  gint n_buffers;
  gint use_bufferpool;
  gint min_buffers;
  gint max_buffers;
  gboolean resend_last;
  gint keepalive_time;
  gboolean autoconnect;

  GstCaps *caps;
  GstCaps *possible_caps;

  enum spa_media_type media_type;
  gboolean is_rawvideo;
  GstVideoInfo video_info;
#ifdef HAVE_GSTREAMER_DMA_DRM
  GstVideoInfoDmaDrm drm_info;
#endif

  gboolean negotiated;
  gboolean flushing;
  gboolean started;
  gboolean eos;
  gboolean flushing_on_remove_buffer;

  gboolean is_live;
  int64_t delay;
  GstClockTime min_latency;
  GstClockTime max_latency;

  GstBuffer *last_buffer;

  enum spa_meta_videotransform_value transform_value;

  GstPipeWireSrcOnDisconnect on_disconnect;
};

GType gst_pipewire_src_on_stream_disconnect_get_type (void);

G_END_DECLS

#endif /* __GST_PIPEWIRE_SRC_H__ */
