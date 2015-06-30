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

#ifndef __PV_CLIENT_SOURCE_H__
#define __PV_CLIENT_SOURCE_H__

#include <glib-object.h>
#include <gio/gio.h>

G_BEGIN_DECLS

typedef struct _PvClientSource PvClientSource;
typedef struct _PvClientSourceClass PvClientSourceClass;
typedef struct _PvClientSourcePrivate PvClientSourcePrivate;

#include "server/pv-source.h"

#define PV_TYPE_CLIENT_SOURCE                 (pv_client_source_get_type ())
#define PV_IS_CLIENT_SOURCE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_SOURCE))
#define PV_IS_CLIENT_SOURCE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PV_TYPE_SOURCE))
#define PV_CLIENT_SOURCE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PV_TYPE_SOURCE, PvClientSourceClass))
#define PV_CLIENT_SOURCE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_SOURCE, PvClientSource))
#define PV_CLIENT_SOURCE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PV_TYPE_SOURCE, PvClientSourceClass))
#define PV_CLIENT_SOURCE_CAST(obj)            ((PvClientSource*)(obj))
#define PV_CLIENT_SOURCE_CLASS_CAST(klass)    ((PvClientSourceClass*)(klass))

/**
 * PvClientSource:
 *
 * Pinos client source object class.
 */
struct _PvClientSource {
  PvSource object;

  PvClientSourcePrivate *priv;
};

/**
 * PvClientSourceClass:
 *
 * Pinos client source object class.
 */
struct _PvClientSourceClass {
  PvSourceClass parent_class;
};

/* normal GObject stuff */
GType            pv_client_source_get_type              (void);

PvSource *       pv_client_source_new                   (PvDaemon *daemon);

PvSourceOutput * pv_client_source_get_source_input      (PvClientSource *source,
                                                         const gchar    *client_path,
                                                         GBytes         *format_filter,
                                                         const gchar    *prefix,
                                                         GError         **error);

G_END_DECLS

#endif /* __PV_CLIENT_SOURCE_H__ */

