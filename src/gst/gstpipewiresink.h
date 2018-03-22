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

#ifndef __GST_PIPEWIRE_SINK_H__
#define __GST_PIPEWIRE_SINK_H__

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>

#include <pipewire/pipewire.h>
#include <gst/gstpipewirepool.h>

G_BEGIN_DECLS

#define GST_TYPE_PIPEWIRE_SINK \
  (gst_pipewire_sink_get_type())
#define GST_PIPEWIRE_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_PIPEWIRE_SINK,GstPipeWireSink))
#define GST_PIPEWIRE_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_PIPEWIRE_SINK,GstPipeWireSinkClass))
#define GST_IS_PIPEWIRE_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_PIPEWIRE_SINK))
#define GST_IS_PIPEWIRE_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_PIPEWIRE_SINK))
#define GST_PIPEWIRE_SINK_CAST(obj) \
  ((GstPipeWireSink *) (obj))

typedef struct _GstPipeWireSink GstPipeWireSink;
typedef struct _GstPipeWireSinkClass GstPipeWireSinkClass;


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
  gchar *path;
  gchar *client_name;
  int fd;

  /* video state */
  gboolean negotiated;

  struct pw_loop *loop;
  struct pw_thread_loop *main_loop;

  struct pw_core *core;
  struct pw_type *type;
  struct pw_remote *remote;
  struct spa_hook remote_listener;

  struct pw_stream *stream;
  struct spa_hook stream_listener;

  GstStructure *properties;
  GstPipeWireSinkMode mode;

  GstPipeWirePool *pool;
  GQueue queue;
  guint need_ready;
};

struct _GstPipeWireSinkClass {
  GstBaseSinkClass parent_class;
};

GType gst_pipewire_sink_get_type (void);
GType gst_pipewire_sink_mode_get_type (void);

G_END_DECLS

#endif /* __GST_PIPEWIRE_SINK_H__ */
