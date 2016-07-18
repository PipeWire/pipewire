/* Pinos
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#ifndef __PINOS_GST_SINK_H__
#define __PINOS_GST_SINK_H__

#include <glib-object.h>

#include <client/pinos.h>
#include <server/node.h>

G_BEGIN_DECLS

#define PINOS_TYPE_GST_SINK                 (pinos_gst_sink_get_type ())
#define PINOS_IS_GST_SINK(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_GST_SINK))
#define PINOS_IS_GST_SINK_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_GST_SINK))
#define PINOS_GST_SINK_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_GST_SINK, PinosGstSinkClass))
#define PINOS_GST_SINK(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_GST_SINK, PinosGstSink))
#define PINOS_GST_SINK_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_GST_SINK, PinosGstSinkClass))
#define PINOS_GST_SINK_CAST(obj)            ((PinosGstSink*)(obj))
#define PINOS_GST_SINK_CLASS_CAST(klass)    ((PinosGstSinkClass*)(klass))

typedef struct _PinosGstSink PinosGstSink;
typedef struct _PinosGstSinkClass PinosGstSinkClass;
typedef struct _PinosGstSinkPrivate PinosGstSinkPrivate;

struct _PinosGstSink {
  PinosNode object;

  PinosGstSinkPrivate *priv;
};

struct _PinosGstSinkClass {
  PinosNodeClass parent_class;
};

GType             pinos_gst_sink_get_type        (void);

PinosNode *       pinos_gst_sink_new             (PinosDaemon     *daemon,
                                                  const gchar     *name,
                                                  PinosProperties *properties,
                                                  GstElement      *element,
                                                  GstCaps         *caps,
                                                  GstElement      *mixer,
                                                  const gchar     *convert_name);

G_END_DECLS

#endif /* __PINOS_GST_SINK_H__ */
