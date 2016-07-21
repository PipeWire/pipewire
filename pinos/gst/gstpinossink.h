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

#include <client/pinos.h>

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
 * GstPinosSinkMode:
 * @GST_PINOS_SINK_MODE_DEFAULT: the default mode as configured in the server
 * @GST_PINOS_SINK_MODE_RENDER: try to render the media
 * @GST_PINOS_SINK_MODE_PROVIDE: provide the media
 *
 * Different modes of operation.
 */
typedef enum
{
  GST_PINOS_SINK_MODE_DEFAULT,
  GST_PINOS_SINK_MODE_RENDER,
  GST_PINOS_SINK_MODE_PROVIDE,
} GstPinosSinkMode;

#define GST_TYPE_PINOS_SINK_MODE (gst_pinos_sink_mode_get_type ())

/**
 * GstPinosSink:
 *
 * Opaque data structure.
 */
struct _GstPinosSink {
  GstBaseSink element;

  /*< private >*/
  gchar *path;
  gchar *client_name;

  /* video state */
  gboolean negotiated;

  GMainContext *context;
  PinosMainLoop *loop;
  PinosContext *ctx;
  PinosStream *stream;
  GstAllocator *allocator;
  GstStructure *properties;
  GstPinosSinkMode mode;

  PinosFdManager *fdmanager;
  GHashTable *mem_ids;
};

struct _GstPinosSinkClass {
  GstBaseSinkClass parent_class;
};

GType gst_pinos_sink_get_type (void);
GType gst_pinos_sink_mode_get_type (void);

G_END_DECLS

#endif /* __GST_PINOS_SINK_H__ */
