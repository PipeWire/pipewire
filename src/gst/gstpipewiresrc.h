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

#ifndef __GST_PIPEWIRE_SRC_H__
#define __GST_PIPEWIRE_SRC_H__

#include <gst/gst.h>
#include <gst/base/gstpushsrc.h>

#include <pipewire/pipewire.h>
#include <gst/gstpipewirepool.h>

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

  /*< private >*/
  gchar *path;
  gchar *client_name;
  gboolean always_copy;
  int fd;

  gboolean negotiated;
  gboolean flushing;
  gboolean started;

  gboolean is_live;
  GstClockTime min_latency;
  GstClockTime max_latency;

  struct pw_loop *loop;
  struct pw_thread_loop *main_loop;

  struct pw_core *core;
  struct pw_type *type;
  struct pw_remote *remote;
  struct spa_hook remote_listener;

  struct pw_stream *stream;
  struct spa_hook stream_listener;

  GstStructure *properties;

  GstPipeWirePool *pool;
  GQueue queue;
  GstClock *clock;
};

struct _GstPipeWireSrcClass {
  GstPushSrcClass parent_class;
};

GType gst_pipewire_src_get_type (void);

G_END_DECLS

#endif /* __GST_PIPEWIRE_SRC_H__ */
