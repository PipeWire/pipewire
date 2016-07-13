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

#ifndef __PINOS_SPA_V4L2_SOURCE_H__
#define __PINOS_SPA_V4L2_SOURCE_H__

#include <glib-object.h>

#include <client/pinos.h>
#include <server/server-node.h>

G_BEGIN_DECLS

#define PINOS_TYPE_SPA_V4L2_SOURCE                 (pinos_spa_v4l2_source_get_type ())
#define PINOS_IS_SPA_V4L2_SOURCE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_SPA_V4L2_SOURCE))
#define PINOS_IS_SPA_V4L2_SOURCE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_SPA_V4L2_SOURCE))
#define PINOS_SPA_V4L2_SOURCE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_SPA_V4L2_SOURCE, PinosSpaV4l2SourceClass))
#define PINOS_SPA_V4L2_SOURCE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_SPA_V4L2_SOURCE, PinosSpaV4l2Source))
#define PINOS_SPA_V4L2_SOURCE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_SPA_V4L2_SOURCE, PinosSpaV4l2SourceClass))
#define PINOS_SPA_V4L2_SOURCE_CAST(obj)            ((PinosSpaV4l2Source*)(obj))
#define PINOS_SPA_V4L2_SOURCE_CLASS_CAST(klass)    ((PinosSpaV4l2SourceClass*)(klass))

typedef struct _PinosSpaV4l2Source PinosSpaV4l2Source;
typedef struct _PinosSpaV4l2SourceClass PinosSpaV4l2SourceClass;
typedef struct _PinosSpaV4l2SourcePrivate PinosSpaV4l2SourcePrivate;

struct _PinosSpaV4l2Source {
  PinosServerNode object;

  PinosSpaV4l2SourcePrivate *priv;
};

struct _PinosSpaV4l2SourceClass {
  PinosServerNodeClass parent_class;
};

GType             pinos_spa_v4l2_source_get_type (void);

PinosServerNode * pinos_spa_v4l2_source_new      (PinosDaemon     *daemon,
                                                  const gchar     *name,
                                                  PinosProperties *properties);

G_END_DECLS

#endif /* __PINOS_SPA_V4L2_SOURCE_H__ */
