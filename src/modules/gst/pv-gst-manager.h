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

#ifndef __PV_GST_MANAGER_H__
#define __PV_GST_MANAGER_H__

#include <glib-object.h>

#include <client/pinos.h>
#include <server/pv-daemon.h>

G_BEGIN_DECLS

#define PV_TYPE_GST_MANAGER                 (pv_gst_manager_get_type ())
#define PV_IS_GST_MANAGER(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_GST_MANAGER))
#define PV_IS_GST_MANAGER_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PV_TYPE_GST_MANAGER))
#define PV_GST_MANAGER_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PV_TYPE_GST_MANAGER, PvGstManagerClass))
#define PV_GST_MANAGER(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_GST_MANAGER, PvGstManager))
#define PV_GST_MANAGER_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PV_TYPE_GST_MANAGER, PvGstManagerClass))
#define PV_GST_MANAGER_CAST(obj)            ((PvGstManager*)(obj))
#define PV_GST_MANAGER_CLASS_CAST(klass)    ((PvGstManagerClass*)(klass))

typedef struct _PvGstManager PvGstManager;
typedef struct _PvGstManagerClass PvGstManagerClass;
typedef struct _PvGstManagerPrivate PvGstManagerPrivate;

struct _PvGstManager {
  GObject object;

  PvGstManagerPrivate *priv;
};

struct _PvGstManagerClass {
  GObjectClass parent_class;
};

GType           pv_gst_manager_get_type            (void);

PvGstManager *  pv_gst_manager_new                 (PvDaemon *daemon);

G_END_DECLS

#endif /* __PV_GST_MANAGER_H__ */

