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

#ifndef __PV_CLIENT_H__
#define __PV_CLIENT_H__

#include <glib-object.h>

#include "pv-daemon.h"

G_BEGIN_DECLS

#define PV_TYPE_CLIENT                 (pv_client_get_type ())
#define PV_IS_CLIENT(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_CLIENT))
#define PV_IS_CLIENT_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PV_TYPE_CLIENT))
#define PV_CLIENT_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PV_TYPE_CLIENT, PvClientClass))
#define PV_CLIENT(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_CLIENT, PvClient))
#define PV_CLIENT_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PV_TYPE_CLIENT, PvClientClass))
#define PV_CLIENT_CAST(obj)            ((PvClient*)(obj))
#define PV_CLIENT_CLASS_CAST(klass)    ((PvClientClass*)(klass))

typedef struct _PvClient PvClient;
typedef struct _PvClientClass PvClientClass;
typedef struct _PvClientPrivate PvClientPrivate;

/**
 * PvClient:
 *
 * Pulsevideo client object class.
 */
struct _PvClient {
  GObject object;

  PvClientPrivate *priv;
};

/**
 * PvClientClass:
 *
 * Pulsevideo client object class.
 */
struct _PvClientClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType           pv_client_get_type             (void);

PvClient *      pv_client_new                  (PvDaemon *daemon,
                                                const gchar *sender,
                                                const gchar *prefix,
                                                GVariant *properties);

const gchar *   pv_client_get_sender           (PvClient *client);
const gchar *   pv_client_get_object_path      (PvClient *client);

G_END_DECLS

#endif /* __PV_CLIENT_H__ */

