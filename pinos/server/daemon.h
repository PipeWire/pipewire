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

#ifndef __PINOS_DAEMON_H__
#define __PINOS_DAEMON_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define PINOS_TYPE_DAEMON                 (pinos_daemon_get_type ())
#define PINOS_IS_DAEMON(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_DAEMON))
#define PINOS_IS_DAEMON_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_DAEMON))
#define PINOS_DAEMON_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_DAEMON, PinosDaemonClass))
#define PINOS_DAEMON(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_DAEMON, PinosDaemon))
#define PINOS_DAEMON_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_DAEMON, PinosDaemonClass))
#define PINOS_DAEMON_CAST(obj)            ((PinosDaemon*)(obj))
#define PINOS_DAEMON_CLASS_CAST(klass)    ((PinosDaemonClass*)(klass))

typedef struct _PinosDaemon PinosDaemon;
typedef struct _PinosDaemonClass PinosDaemonClass;
typedef struct _PinosDaemonPrivate PinosDaemonPrivate;

#include <pinos/server/node.h>
#include <pinos/server/port.h>
#include <pinos/server/node-factory.h>
#include <pinos/client/properties.h>

/**
 * PinosDaemon:
 *
 * Pinos daemon object class.
 */
struct _PinosDaemon {
  GObject object;

  PinosDaemonPrivate *priv;
};

/**
 * PinosDaemonClass:
 *
 * Pinos daemon object class.
 */
struct _PinosDaemonClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType             pinos_daemon_get_type          (void);

PinosDaemon *     pinos_daemon_new               (PinosProperties *properties);
const gchar *     pinos_daemon_get_sender        (PinosDaemon *daemon);

void              pinos_daemon_start             (PinosDaemon *daemon);
void              pinos_daemon_stop              (PinosDaemon *daemon);

gchar *           pinos_daemon_export_uniquely   (PinosDaemon *daemon, GDBusObjectSkeleton *skel);
void              pinos_daemon_unexport          (PinosDaemon *daemon, const gchar *name);

void              pinos_daemon_add_node          (PinosDaemon *daemon, PinosNode *node);
void              pinos_daemon_remove_node       (PinosDaemon *daemon, PinosNode *node);

PinosPort *       pinos_daemon_find_port         (PinosDaemon     *daemon,
                                                  PinosDirection   direction,
                                                  const gchar     *name,
                                                  PinosProperties *props,
                                                  unsigned int     n_format_filters,
                                                  SpaFormat      **format_filters,
                                                  GError         **error);

void              pinos_daemon_add_node_factory  (PinosDaemon *daemon,
                                                  PinosNodeFactory *factory);
void              pinos_daemon_remove_node_factory (PinosDaemon *daemon,
                                                  PinosNodeFactory *factory);

G_END_DECLS

#endif /* __PINOS_DAEMON_H__ */
