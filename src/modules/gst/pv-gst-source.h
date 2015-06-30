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

#ifndef __PV_GST_SOURCE_H__
#define __PV_GST_SOURCE_H__

#include <glib-object.h>

#include <client/pinos.h>
#include <server/pv-daemon.h>
#include <server/pv-source.h>

G_BEGIN_DECLS

#define PV_TYPE_GST_SOURCE                 (pv_gst_source_get_type ())
#define PV_IS_GST_SOURCE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_GST_SOURCE))
#define PV_IS_GST_SOURCE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PV_TYPE_GST_SOURCE))
#define PV_GST_SOURCE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PV_TYPE_GST_SOURCE, PvGstSourceClass))
#define PV_GST_SOURCE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_GST_SOURCE, PvGstSource))
#define PV_GST_SOURCE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PV_TYPE_GST_SOURCE, PvGstSourceClass))
#define PV_GST_SOURCE_CAST(obj)            ((PvGstSource*)(obj))
#define PV_GST_SOURCE_CLASS_CAST(klass)    ((PvGstSourceClass*)(klass))

typedef struct _PvGstSource PvGstSource;
typedef struct _PvGstSourceClass PvGstSourceClass;
typedef struct _PvGstSourcePrivate PvGstSourcePrivate;

struct _PvGstSource {
  PvSource object;

  PvGstSourcePrivate *priv;
};

struct _PvGstSourceClass {
  PvSourceClass parent_class;
};

GType           pv_gst_source_get_type             (void);

PvSource *      pv_gst_source_new                  (PvDaemon *daemon, const gchar *name, GstElement *element);

G_END_DECLS

#endif /* __PV_GST_SOURCE_H__ */

