/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef __GST_PIPEWIRE_SINK_H__
#define __GST_PIPEWIRE_SINK_H__

#include "gstpipewirestream.h"

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>

#include <pipewire/pipewire.h>
#include <gst/gstpipewirepool.h>
#include <gst/gstpipewirecore.h>

G_BEGIN_DECLS

#define GST_TYPE_PIPEWIRE_SINK (gst_pipewire_sink_get_type())
#define GST_PIPEWIRE_SINK_CAST(obj) ((GstPipeWireSink *) (obj))
G_DECLARE_FINAL_TYPE (GstPipeWireSink, gst_pipewire_sink, GST, PIPEWIRE_SINK, GstBaseSink)

/**
 * GstPipeWireSinkMode:
 * @GST_PIPEWIRE_SINK_MODE_DEFAULT: the default mode as configured in the server
 * @GST_PIPEWIRE_SINK_MODE_RENDER: try to render the media
 * @GST_PIPEWIRE_SINK_MODE_PROVIDE: provide the media
 *
 * Different modes of operation.
 */
typedef enum
{
  GST_PIPEWIRE_SINK_MODE_DEFAULT,
  GST_PIPEWIRE_SINK_MODE_RENDER,
  GST_PIPEWIRE_SINK_MODE_PROVIDE,
} GstPipeWireSinkMode;

#define GST_TYPE_PIPEWIRE_SINK_MODE (gst_pipewire_sink_mode_get_type ())

/**
 * GstPipeWireSink:
 *
 * Opaque data structure.
 */
struct _GstPipeWireSink {
  GstBaseSink element;

  /*< private >*/
  GstPipeWireStream *stream;

  /* video state */
  gboolean negotiated;
  gboolean rate_match;
  gint rate;

  GstPipeWireSinkMode mode;

  struct spa_io_rate_match *match;
};

GType gst_pipewire_sink_mode_get_type (void);

G_END_DECLS

#endif /* __GST_PIPEWIRE_SINK_H__ */
