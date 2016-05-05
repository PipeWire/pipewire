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

#ifndef __PINOS_GST_SOURCE_H__
#define __PINOS_GST_SOURCE_H__

#include <glib-object.h>

#include <client/pinos.h>
#include <server/node.h>
#include <server/source.h>

G_BEGIN_DECLS

#define PINOS_TYPE_GST_SOURCE                 (pinos_gst_source_get_type ())
#define PINOS_IS_GST_SOURCE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_GST_SOURCE))
#define PINOS_IS_GST_SOURCE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_GST_SOURCE))
#define PINOS_GST_SOURCE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_GST_SOURCE, PinosGstSourceClass))
#define PINOS_GST_SOURCE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_GST_SOURCE, PinosGstSource))
#define PINOS_GST_SOURCE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_GST_SOURCE, PinosGstSourceClass))
#define PINOS_GST_SOURCE_CAST(obj)            ((PinosGstSource*)(obj))
#define PINOS_GST_SOURCE_CLASS_CAST(klass)    ((PinosGstSourceClass*)(klass))

typedef struct _PinosGstSource PinosGstSource;
typedef struct _PinosGstSourceClass PinosGstSourceClass;
typedef struct _PinosGstSourcePrivate PinosGstSourcePrivate;

struct _PinosGstSource {
  PinosSource object;

  PinosGstSourcePrivate *priv;
};

struct _PinosGstSourceClass {
  PinosSourceClass parent_class;
};

GType           pinos_gst_source_get_type        (void);

PinosSource *   pinos_gst_source_new             (PinosNode       *node,
                                                  const gchar     *name,
                                                  PinosProperties *properties,
                                                  GstElement      *element,
                                                  GstCaps         *caps);

G_END_DECLS

#endif /* __PINOS_GST_SOURCE_H__ */
