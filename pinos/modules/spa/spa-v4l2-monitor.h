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

#ifndef __PINOS_SPA_V4L2_MONITOR_H__
#define __PINOS_SPA_V4L2_MONITOR_H__

#include <glib-object.h>

#include <pinos/server/daemon.h>

G_BEGIN_DECLS

#define PINOS_TYPE_SPA_V4L2_MONITOR                 (pinos_spa_v4l2_monitor_get_type ())
#define PINOS_IS_SPA_V4L2_MONITOR(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_SPA_V4L2_MONITOR))
#define PINOS_IS_SPA_V4L2_MONITOR_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_SPA_V4L2_MONITOR))
#define PINOS_SPA_V4L2_MONITOR_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_SPA_V4L2_MONITOR, PinosSpaV4l2MonitorClass))
#define PINOS_SPA_V4L2_MONITOR(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_SPA_V4L2_MONITOR, PinosSpaV4l2Monitor))
#define PINOS_SPA_V4L2_MONITOR_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_SPA_V4L2_MONITOR, PinosSpaV4l2MonitorClass))
#define PINOS_SPA_V4L2_MONITOR_CAST(obj)            ((PinosSpaV4l2Monitor*)(obj))
#define PINOS_SPA_V4L2_MONITOR_CLASS_CAST(klass)    ((PinosSpaV4l2MonitorClass*)(klass))

typedef struct _PinosSpaV4l2Monitor PinosSpaV4l2Monitor;
typedef struct _PinosSpaV4l2MonitorClass PinosSpaV4l2MonitorClass;
typedef struct _PinosSpaV4l2MonitorPrivate PinosSpaV4l2MonitorPrivate;

struct _PinosSpaV4l2Monitor {
  GObject object;

  PinosSpaV4l2MonitorPrivate *priv;
};

struct _PinosSpaV4l2MonitorClass {
  GObjectClass parent_class;
};

GType             pinos_spa_v4l2_monitor_get_type (void);

GObject *         pinos_spa_v4l2_monitor_new      (PinosDaemon *daemon);

G_END_DECLS

#endif /* __PINOS_SPA_V4L2_MONITOR_H__ */
