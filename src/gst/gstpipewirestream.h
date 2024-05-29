/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2024 Collabora Ltd. */
/* SPDX-License-Identifier: MIT */

#ifndef __GST_PIPEWIRE_STREAM_H__
#define __GST_PIPEWIRE_STREAM_H__

#include "config.h"

#include "gstpipewirecore.h"
#include "gstpipewirepool.h"
#include "gstpipewireclock.h"

#include <gst/gst.h>
#include <pipewire/pipewire.h>

G_BEGIN_DECLS

#define GST_TYPE_PIPEWIRE_STREAM (gst_pipewire_stream_get_type())
G_DECLARE_FINAL_TYPE(GstPipeWireStream, gst_pipewire_stream, GST, PIPEWIRE_STREAM, GstObject)

struct _GstPipeWireStream {
  GstObject parent;

  /* relatives */
  GstElement *element;
  GstPipeWireCore *core;
  GstPipeWirePool *pool;
  GstClock *clock;

  /* the actual pw stream */
  struct pw_stream *pwstream;
  struct spa_hook pwstream_listener;

  /* common properties */
  int fd;
  gchar *path;
  gchar *target_object;
  gchar *client_name;
  GstStructure *client_properties;
  GstStructure *stream_properties;
};

GstPipeWireStream * gst_pipewire_stream_new (GstElement * element);

gboolean gst_pipewire_stream_open (GstPipeWireStream * self,
    const struct pw_stream_events * pwstream_events);
void gst_pipewire_stream_close (GstPipeWireStream * self);

G_END_DECLS

#endif /* __GST_PIPEWIRE_STREAM_H__ */
