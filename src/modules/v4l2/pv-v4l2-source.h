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

#ifndef __PV_V4L2_SOURCE_H__
#define __PV_V4L2_SOURCE_H__

#include <glib-object.h>
#include "server/pv-source.h"

G_BEGIN_DECLS

#define PV_TYPE_V4L2_SOURCE                 (pv_v4l2_source_get_type ())
#define PV_IS_V4L2_SOURCE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_V4L2_SOURCE))
#define PV_IS_V4L2_SOURCE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PV_TYPE_V4L2_SOURCE))
#define PV_V4L2_SOURCE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PV_TYPE_V4L2_SOURCE, PvV4l2SourceClass))
#define PV_V4L2_SOURCE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_V4L2_SOURCE, PvV4l2Source))
#define PV_V4L2_SOURCE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PV_TYPE_V4L2_SOURCE, PvV4l2SourceClass))
#define PV_V4L2_SOURCE_CAST(obj)            ((PvV4l2Source*)(obj))
#define PV_V4L2_SOURCE_CLASS_CAST(klass)    ((PvV4l2SourceClass*)(klass))

typedef struct _PvV4l2Source PvV4l2Source;
typedef struct _PvV4l2SourceClass PvV4l2SourceClass;
typedef struct _PvV4l2SourcePrivate PvV4l2SourcePrivate;

struct _PvV4l2Source {
  PvSource object;

  PvV4l2SourcePrivate *priv;
};

struct _PvV4l2SourceClass {
  PvSourceClass parent_class;
};

GType           pv_v4l2_source_get_type             (void);

PvSource *      pv_v4l2_source_new                  (PvDaemon *daemon);

G_END_DECLS

#endif /* __PV_V4L2_SOURCE_H__ */

