/* GStreamer */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-FileCopyrightText: Copyright © 2024 Collabora Ltd. */
/* SPDX-License-Identifier: MIT */

#ifndef __GST_PIPEWIRE_STREAM_H__
#define __GST_PIPEWIRE_STREAM_H__

#include "config.h"

#include "gstpipewirecore.h"

#include <gst/gst.h>
#include <spa/utils/dll.h>
#include <pipewire/pipewire.h>

G_BEGIN_DECLS

typedef struct _GstPipeWirePool GstPipeWirePool;

#define GST_TYPE_PIPEWIRE_STREAM (gst_pipewire_stream_get_type())
G_DECLARE_FINAL_TYPE(GstPipeWireStream, gst_pipewire_stream, GST, PIPEWIRE_STREAM, GstObject)

struct _GstPipeWireStream {
  GstObject parent;

  /* relatives */
  GstElement *element;
  GstPipeWireCore *core;
  GstPipeWirePool *pool;
  GstClock *clock;

  guint64 position;
  guint64 buf_duration;
  struct spa_dll dll;
  double err_avg, err_var, err_wdw;
  guint64 last_ts;
  guint64 base_buffer_ts;
  guint64 base_ts;

  /* the actual pw stream */
  struct pw_stream *pwstream;
  struct spa_hook pwstream_listener;

  struct spa_io_position *io_position;

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
