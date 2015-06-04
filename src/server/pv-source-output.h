/* Pulsevideo
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

#ifndef __PV_SOURCE_OUTPUT_H__
#define __PV_SOURCE_OUTPUT_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define PV_TYPE_SOURCE_OUTPUT                 (pv_source_output_get_type ())
#define PV_IS_SOURCE_OUTPUT(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_SOURCE_OUTPUT))
#define PV_IS_SOURCE_OUTPUT_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PV_TYPE_SOURCE_OUTPUT))
#define PV_SOURCE_OUTPUT_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PV_TYPE_SOURCE_OUTPUT, PvSourceOutputClass))
#define PV_SOURCE_OUTPUT(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_SOURCE_OUTPUT, PvSourceOutput))
#define PV_SOURCE_OUTPUT_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PV_TYPE_SOURCE_OUTPUT, PvSourceOutputClass))
#define PV_SOURCE_OUTPUT_CAST(obj)            ((PvSourceOutput*)(obj))
#define PV_SOURCE_OUTPUT_CLASS_CAST(klass)    ((PvSourceOutputClass*)(klass))

typedef struct _PvSourceOutput PvSourceOutput;
typedef struct _PvSourceOutputClass PvSourceOutputClass;
typedef struct _PvSourceOutputPrivate PvSourceOutputPrivate;

/**
 * PvSourceOutput:
 *
 * Pulsevideo source output object class.
 */
struct _PvSourceOutput {
  GObject object;

  PvSourceOutputPrivate *priv;
};

/**
 * PvSourceOutputClass:
 *
 * Pulsevideo source output object class.
 */
struct _PvSourceOutputClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType              pv_source_output_get_type             (void);

void               pv_source_output_remove               (PvSourceOutput *output);

const gchar *      pv_source_output_get_object_path      (PvSourceOutput *output);

G_END_DECLS

#endif /* __PV_SOURCE_OUTPUT_H__ */

