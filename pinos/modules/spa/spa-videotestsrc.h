/* Pinos
 * Copyright (C) 2016 Axis Communications AB
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

#ifndef __PINOS_SPA_VIDEOTESTSRC_H__
#define __PINOS_SPA_VIDEOTESTSRC_H__

#include <glib-object.h>

#include <client/pinos.h>
#include <server/node.h>

G_BEGIN_DECLS

#define PINOS_TYPE_SPA_VIDEOTESTSRC                 (pinos_spa_videotestsrc_get_type ())
#define PINOS_IS_SPA_VIDEOTESTSRC(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_SPA_VIDEOTESTSRC))
#define PINOS_IS_SPA_VIDEOTESTSRC_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_SPA_VIDEOTESTSRC))
#define PINOS_SPA_VIDEOTESTSRC_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_SPA_VIDEOTESTSRC, PinosSpaVideoTestSrcClass))
#define PINOS_SPA_VIDEOTESTSRC(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_SPA_VIDEOTESTSRC, PinosSpaVideoTestSrc))
#define PINOS_SPA_VIDEOTESTSRC_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_SPA_VIDEOTESTSRC, PinosSpaVideoTestSrcClass))
#define PINOS_SPA_VIDEOTESTSRC_CAST(obj)            ((PinosSpaVideoTestSrc*)(obj))
#define PINOS_SPA_VIDEOTESTSRC_CLASS_CAST(klass)    ((PinosSpaVideoTestSrcClass*)(klass))

typedef struct _PinosSpaVideoTestSrc PinosSpaVideoTestSrc;
typedef struct _PinosSpaVideoTestSrcClass PinosSpaVideoTestSrcClass;
typedef struct _PinosSpaVideoTestSrcPrivate PinosSpaVideoTestSrcPrivate;

struct _PinosSpaVideoTestSrc {
  PinosNode object;

  PinosSpaVideoTestSrcPrivate *priv;
};

struct _PinosSpaVideoTestSrcClass {
  PinosNodeClass parent_class;
};

GType             pinos_spa_videotestsrc_get_type (void);

PinosNode *       pinos_spa_videotestsrc_new      (PinosDaemon     *daemon,
                                                   const gchar     *name,
                                                   PinosProperties *properties);

G_END_DECLS

#endif /* __PINOS_SPA_VIDEOTESTSRC_H__ */
