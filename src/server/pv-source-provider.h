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

#ifndef __PV_SOURCE_PROVIDER_H__
#define __PV_SOURCE_PROVIDER_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define PV_TYPE_SOURCE_PROVIDER                 (pv_source_provider_get_type ())
#define PV_IS_SOURCE_PROVIDER(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_SOURCE_PROVIDER))
#define PV_IS_SOURCE_PROVIDER_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PV_TYPE_SOURCE_PROVIDER))
#define PV_SOURCE_PROVIDER_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PV_TYPE_SOURCE_PROVIDER, PvSourceProviderClass))
#define PV_SOURCE_PROVIDER(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_SOURCE_PROVIDER, PvSourceProvider))
#define PV_SOURCE_PROVIDER_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PV_TYPE_SOURCE_PROVIDER, PvSourceProviderClass))
#define PV_SOURCE_PROVIDER_CAST(obj)            ((PvSourceProvider*)(obj))
#define PV_SOURCE_PROVIDER_CLASS_CAST(klass)    ((PvSourceProviderClass*)(klass))

typedef struct _PvSourceProvider PvSourceProvider;
typedef struct _PvSourceProviderClass PvSourceProviderClass;
typedef struct _PvSourceProviderPrivate PvSourceProviderPrivate;

#include "pv-daemon.h"


/**
 * PvSourceProvider:
 *
 * Pulsevideo source provider object class.
 */
struct _PvSourceProvider {
  GObject object;

  PvSourceProviderPrivate *priv;
};

/**
 * PvSourceProviderClass:
 *
 * Pulsevideo source provider object class.
 */
struct _PvSourceProviderClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType               pv_source_provider_get_type             (void);

PvSourceProvider *  pv_source_provider_new                  (PvDaemon *daemon, const gchar *prefix,
                                                             const gchar *name, const gchar *path);

const gchar *       pv_source_provider_get_object_path      (PvSourceProvider *client);

G_END_DECLS

#endif /* __PV_SOURCE_PROVIDER_H__ */

