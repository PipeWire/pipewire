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

#ifndef __PINOS_SINK_H__
#define __PINOS_SINK_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosSink PinosSink;
typedef struct _PinosSinkClass PinosSinkClass;
typedef struct _PinosSinkPrivate PinosSinkPrivate;

#include <pinos/client/introspect.h>
#include <pinos/server/channel.h>

#define PINOS_TYPE_SINK                 (pinos_sink_get_type ())
#define PINOS_IS_SINK(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_SINK))
#define PINOS_IS_SINK_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_SINK))
#define PINOS_SINK_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_SINK, PinosSinkClass))
#define PINOS_SINK(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_SINK, PinosSink))
#define PINOS_SINK_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_SINK, PinosSinkClass))
#define PINOS_SINK_CAST(obj)            ((PinosSink*)(obj))
#define PINOS_SINK_CLASS_CAST(klass)    ((PinosSinkClass*)(klass))

/**
 * PinosSink:
 *
 * Pinos sink object class.
 */
struct _PinosSink {
  GObject object;

  PinosSinkPrivate *priv;
};

/**
 * PinosSinkClass:
 * @get_formats: called to get a list of supported formats from the sink
 * @set_state: called to change the current state of the sink
 * @create_channel: called to create a new channel object
 * @release_channel: called to release a channel object
 *
 * Pinos sink object class.
 */
struct _PinosSinkClass {
  GObjectClass parent_class;

  GBytes *            (*get_formats)  (PinosSink    *sink,
                                       GBytes       *filter,
                                       GError      **error);

  gboolean            (*set_state)  (PinosSink *sink, PinosSinkState);

  PinosChannel *      (*create_channel)  (PinosSink       *sink,
                                          const gchar     *client_path,
                                          GBytes          *format_filter,
                                          PinosProperties *props,
                                          const gchar     *prefix,
                                          GError          **error);
  gboolean            (*release_channel) (PinosSink       *sink,
                                          PinosChannel    *channel);
};

/* normal GObject stuff */
GType               pinos_sink_get_type                  (void);

GBytes *            pinos_sink_get_formats               (PinosSink *sink,
                                                          GBytes      *filter,
                                                          GError     **error);

gboolean            pinos_sink_set_state                 (PinosSink *sink, PinosSinkState state);
void                pinos_sink_update_state              (PinosSink *sink, PinosSinkState state);
void                pinos_sink_report_error              (PinosSink *sink, GError *error);
void                pinos_sink_report_idle               (PinosSink *sink);
void                pinos_sink_report_busy               (PinosSink *sink);

void                pinos_sink_update_possible_formats   (PinosSink *sink, GBytes *formats);
void                pinos_sink_update_format             (PinosSink *sink, GBytes *format);

PinosChannel *      pinos_sink_create_channel            (PinosSink     *sink,
                                                          const gchar     *client_path,
                                                          GBytes          *format_filter,
                                                          PinosProperties *props,
                                                          const gchar     *prefix,
                                                          GError          **error);
gboolean            pinos_sink_release_channel           (PinosSink     *sink,
                                                          PinosChannel    *channel);

G_END_DECLS

#endif /* __PINOS_SINK_H__ */
