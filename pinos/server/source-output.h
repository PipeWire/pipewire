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

#ifndef __PINOS_SOURCE_OUTPUT_H__
#define __PINOS_SOURCE_OUTPUT_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define PINOS_TYPE_SOURCE_OUTPUT                 (pinos_source_output_get_type ())
#define PINOS_IS_SOURCE_OUTPUT(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_SOURCE_OUTPUT))
#define PINOS_IS_SOURCE_OUTPUT_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_SOURCE_OUTPUT))
#define PINOS_SOURCE_OUTPUT_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_SOURCE_OUTPUT, PinosSourceOutputClass))
#define PINOS_SOURCE_OUTPUT(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_SOURCE_OUTPUT, PinosSourceOutput))
#define PINOS_SOURCE_OUTPUT_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_SOURCE_OUTPUT, PinosSourceOutputClass))
#define PINOS_SOURCE_OUTPUT_CAST(obj)            ((PinosSourceOutput*)(obj))
#define PINOS_SOURCE_OUTPUT_CLASS_CAST(klass)    ((PinosSourceOutputClass*)(klass))

typedef struct _PinosSourceOutput PinosSourceOutput;
typedef struct _PinosSourceOutputClass PinosSourceOutputClass;
typedef struct _PinosSourceOutputPrivate PinosSourceOutputPrivate;

/**
 * PinosSourceOutput:
 *
 * Pinos source output object class.
 */
struct _PinosSourceOutput {
  GObject object;

  PinosSourceOutputPrivate *priv;
};

/**
 * PinosSourceOutputClass:
 *
 * Pinos source output object class.
 */
struct _PinosSourceOutputClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType              pinos_source_output_get_type             (void);

void               pinos_source_output_remove               (PinosSourceOutput *output);

const gchar *      pinos_source_output_get_object_path      (PinosSourceOutput *output);

G_END_DECLS

#endif /* __PINOS_SOURCE_OUTPUT_H__ */
