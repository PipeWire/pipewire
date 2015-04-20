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

#ifndef __PV_DAEMON_H__
#define __PV_DAEMON_H__

#include <glib-object.h>
#include <gio/gio.h>

#include "dbus/org-pulsevideo.h"

G_BEGIN_DECLS

#define PV_TYPE_DAEMON                 (pv_daemon_get_type ())
#define PV_IS_DAEMON(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_DAEMON))
#define PV_IS_DAEMON_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PV_TYPE_DAEMON))
#define PV_DAEMON_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PV_TYPE_DAEMON, PvDaemonClass))
#define PV_DAEMON(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_DAEMON, PvDaemon))
#define PV_DAEMON_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PV_TYPE_DAEMON, PvDaemonClass))
#define PV_DAEMON_CAST(obj)            ((PvDaemon*)(obj))
#define PV_DAEMON_CLASS_CAST(klass)    ((PvDaemonClass*)(klass))

typedef struct _PvDaemon PvDaemon;
typedef struct _PvDaemonClass PvDaemonClass;
typedef struct _PvDaemonPrivate PvDaemonPrivate;

#include <client/pv-source.h>
#include <server/pv-source-provider.h>

/**
 * PvDaemon:
 *
 * Pulsevideo daemon object class.
 */
struct _PvDaemon {
  GObject object;

  PvDaemonPrivate *priv;
};

/**
 * PvDaemonClass:
 *
 * Pulsevideo daemon object class.
 */
struct _PvDaemonClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType              pv_daemon_get_type        (void);

PvDaemon *         pv_daemon_new             (void);

void               pv_daemon_start           (PvDaemon *daemon);
void               pv_daemon_stop            (PvDaemon *daemon);

gchar *            pv_daemon_export_uniquely (PvDaemon *daemon, GDBusObjectSkeleton *skel);
void               pv_daemon_unexport        (PvDaemon *daemon, const gchar *name);

void               pv_daemon_add_source      (PvDaemon *daemon, PvSource *source);
void               pv_daemon_remove_source   (PvDaemon *daemon, PvSource *source);

PvSource1 *        pv_daemon_get_source      (PvDaemon *daemon, const gchar *name);

G_END_DECLS

#endif /* __PV_DAEMON_H__ */

