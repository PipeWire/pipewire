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

#ifndef __PINOS_SPA_ALSA_MONITOR_H__
#define __PINOS_SPA_ALSA_MONITOR_H__

#include <glib-object.h>

#include <pinos/server/daemon.h>

G_BEGIN_DECLS

#define PINOS_TYPE_SPA_ALSA_MONITOR                 (pinos_spa_alsa_monitor_get_type ())
#define PINOS_IS_SPA_ALSA_MONITOR(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_SPA_ALSA_MONITOR))
#define PINOS_IS_SPA_ALSA_MONITOR_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_SPA_ALSA_MONITOR))
#define PINOS_SPA_ALSA_MONITOR_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_SPA_ALSA_MONITOR, PinosSpaALSAMonitorClass))
#define PINOS_SPA_ALSA_MONITOR(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_SPA_ALSA_MONITOR, PinosSpaALSAMonitor))
#define PINOS_SPA_ALSA_MONITOR_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_SPA_ALSA_MONITOR, PinosSpaALSAMonitorClass))
#define PINOS_SPA_ALSA_MONITOR_CAST(obj)            ((PinosSpaALSAMonitor*)(obj))
#define PINOS_SPA_ALSA_MONITOR_CLASS_CAST(klass)    ((PinosSpaALSAMonitorClass*)(klass))

typedef struct _PinosSpaALSAMonitor PinosSpaALSAMonitor;
typedef struct _PinosSpaALSAMonitorClass PinosSpaALSAMonitorClass;
typedef struct _PinosSpaALSAMonitorPrivate PinosSpaALSAMonitorPrivate;

struct _PinosSpaALSAMonitor {
  GObject object;

  PinosSpaALSAMonitorPrivate *priv;
};

struct _PinosSpaALSAMonitorClass {
  GObjectClass parent_class;
};

GType             pinos_spa_alsa_monitor_get_type (void);

GObject *         pinos_spa_alsa_monitor_new      (PinosDaemon *daemon);

G_END_DECLS

#endif /* __PINOS_SPA_ALSA_MONITOR_H__ */
