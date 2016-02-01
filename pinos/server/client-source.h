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

#ifndef __PINOS_CLIENT_SOURCE_H__
#define __PINOS_CLIENT_SOURCE_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosClientSource PinosClientSource;
typedef struct _PinosClientSourceClass PinosClientSourceClass;
typedef struct _PinosClientSourcePrivate PinosClientSourcePrivate;

#include <pinos/server/source.h>

#define PINOS_TYPE_CLIENT_SOURCE                 (pinos_client_source_get_type ())
#define PINOS_IS_CLIENT_SOURCE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_SOURCE))
#define PINOS_IS_CLIENT_SOURCE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_SOURCE))
#define PINOS_CLIENT_SOURCE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_SOURCE, PinosClientSourceClass))
#define PINOS_CLIENT_SOURCE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_SOURCE, PinosClientSource))
#define PINOS_CLIENT_SOURCE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_SOURCE, PinosClientSourceClass))
#define PINOS_CLIENT_SOURCE_CAST(obj)            ((PinosClientSource*)(obj))
#define PINOS_CLIENT_SOURCE_CLASS_CAST(klass)    ((PinosClientSourceClass*)(klass))

/**
 * PinosClientSource:
 *
 * Pinos client source object class.
 */
struct _PinosClientSource {
  PinosSource object;

  PinosClientSourcePrivate *priv;
};

/**
 * PinosClientSourceClass:
 *
 * Pinos client source object class.
 */
struct _PinosClientSourceClass {
  PinosSourceClass parent_class;
};

/* normal GObject stuff */
GType               pinos_client_source_get_type         (void);

PinosSource *       pinos_client_source_new              (PinosDaemon *daemon,
                                                          GBytes      *possible_formats);

PinosSourceOutput * pinos_client_source_get_source_input (PinosClientSource *source,
                                                          const gchar       *client_path,
                                                          GBytes            *format_filter,
                                                          PinosProperties   *props,
                                                          const gchar       *prefix,
                                                          GError            **error);

G_END_DECLS

#endif /* __PINOS_CLIENT_SOURCE_H__ */
