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

#define PINOS_DAEMON_URI                            "http://pinos.org/ns/daemon"
#define PINOS_DAEMON_PREFIX                         PINOS_DAEMON_URI "#"

typedef struct _PinosDaemon PinosDaemon;

#include <pinos/client/properties.h>
#include <pinos/server/core.h>
#include <pinos/server/node.h>

/**
 * PinosDaemon:
 *
 * Pinos daemon object class.
 */
struct _PinosDaemon {
  PinosProperties *properties;

  PinosCore *core;

  SpaResult  (*start)    (PinosDaemon *daemon);
  SpaResult  (*stop)     (PinosDaemon *daemon);
};

PinosObject *     pinos_daemon_new               (PinosCore       *core,
                                                  PinosProperties *properties);

const char *      pinos_daemon_get_object_path   (PinosDaemon *daemon);

#define pinos_daemon_start(d)   (d)->start(d)
#define pinos_daemon_stop(d)    (d)->stop(d)

char *            pinos_daemon_export_uniquely   (PinosDaemon *daemon, GDBusObjectSkeleton *skel);
void              pinos_daemon_unexport          (PinosDaemon *daemon, const char *name);

PinosPort *       pinos_daemon_find_port         (PinosDaemon     *daemon,
                                                  PinosPort       *other_port,
                                                  const char      *name,
                                                  PinosProperties *props,
                                                  GPtrArray       *format_filter,
                                                  GError         **error);

G_END_DECLS

#endif /* __PINOS_DAEMON_H__ */
