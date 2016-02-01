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

#ifndef __PINOS_SOURCE_H__
#define __PINOS_SOURCE_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosSource PinosSource;
typedef struct _PinosSourceClass PinosSourceClass;
typedef struct _PinosSourcePrivate PinosSourcePrivate;

#include <pinos/client/introspect.h>
#include <pinos/server/source-output.h>

#define PINOS_TYPE_SOURCE                 (pinos_source_get_type ())
#define PINOS_IS_SOURCE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_SOURCE))
#define PINOS_IS_SOURCE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_SOURCE))
#define PINOS_SOURCE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_SOURCE, PinosSourceClass))
#define PINOS_SOURCE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_SOURCE, PinosSource))
#define PINOS_SOURCE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_SOURCE, PinosSourceClass))
#define PINOS_SOURCE_CAST(obj)            ((PinosSource*)(obj))
#define PINOS_SOURCE_CLASS_CAST(klass)    ((PinosSourceClass*)(klass))

/**
 * PinosSource:
 *
 * Pinos source object class.
 */
struct _PinosSource {
  GObject object;

  PinosSourcePrivate *priv;
};

/**
 * PinosSourceClass:
 * @get_formats: called to get a list of supported formats from the source
 * @set_state: called to change the current state of the source
 * @create_source_output: called to create a new source-output object
 * @release_source_output: called to release a source-output object
 *
 * Pinos source object class.
 */
struct _PinosSourceClass {
  GObjectClass parent_class;

  GBytes *            (*get_formats)  (PinosSource  *source,
                                       GBytes       *filter,
                                       GError      **error);

  gboolean            (*set_state)  (PinosSource *source, PinosSourceState);

  PinosSourceOutput * (*create_source_output)  (PinosSource     *source,
                                                const gchar     *client_path,
                                                GBytes          *format_filter,
                                                PinosProperties *props,
                                                const gchar     *prefix,
                                                GError          **error);
  gboolean            (*release_source_output) (PinosSource       *source,
                                                PinosSourceOutput *output);
};

/* normal GObject stuff */
GType               pinos_source_get_type                (void);

const gchar *       pinos_source_get_object_path         (PinosSource *source);

GBytes *            pinos_source_get_formats             (PinosSource *source,
                                                          GBytes      *filter,
                                                          GError     **error);

gboolean            pinos_source_set_state               (PinosSource *source, PinosSourceState state);
void                pinos_source_update_state            (PinosSource *source, PinosSourceState state);
void                pinos_source_report_error            (PinosSource *source, GError *error);
void                pinos_source_report_idle             (PinosSource *source);
void                pinos_source_report_busy             (PinosSource *source);

void                pinos_source_update_possible_formats (PinosSource *source, GBytes *formats);

PinosSourceOutput * pinos_source_create_source_output    (PinosSource     *source,
                                                          const gchar     *client_path,
                                                          GBytes          *format_filter,
                                                          PinosProperties *props,
                                                          const gchar     *prefix,
                                                          GError          **error);
gboolean            pinos_source_release_source_output   (PinosSource       *source,
                                                          PinosSourceOutput *output);

G_END_DECLS

#endif /* __PINOS_SOURCE_H__ */
