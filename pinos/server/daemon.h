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

#ifdef __cplusplus
extern "C" {
#endif

#define PINOS_DAEMON_URI                            "http://pinos.org/ns/daemon"
#define PINOS_DAEMON_PREFIX                         PINOS_DAEMON_URI "#"

typedef struct _PinosDaemon PinosDaemon;

#include <pinos/client/properties.h>
#include <pinos/server/core.h>
#include <pinos/server/node.h>
#include <pinos/server/port.h>

/**
 * PinosDaemon:
 *
 * Pinos daemon object class.
 */
struct _PinosDaemon {
  PinosCore   *core;
  SpaList      link;
  PinosGlobal *global;

  PinosProperties *properties;

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosDaemon   *daemon));

  SpaResult  (*start)    (PinosDaemon *daemon);
  SpaResult  (*stop)     (PinosDaemon *daemon);
};

PinosDaemon *     pinos_daemon_new               (PinosCore       *core,
                                                  PinosProperties *properties);
void              pinos_daemon_destroy           (PinosDaemon     *daemon);


#define pinos_daemon_start(d)   (d)->start(d)
#define pinos_daemon_stop(d)    (d)->stop(d)

PinosPort *       pinos_daemon_find_port         (PinosDaemon      *daemon,
                                                  PinosPort        *other_port,
                                                  const char       *name,
                                                  PinosProperties  *props,
                                                  SpaFormat       **format_filter,
                                                  char            **error);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_DAEMON_H__ */
