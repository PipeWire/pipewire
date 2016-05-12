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

#ifndef __PINOS_SERVER_PORT_H__
#define __PINOS_SERVER_PORT_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosServerPort PinosServerPort;
typedef struct _PinosServerPortClass PinosServerPortClass;
typedef struct _PinosServerPortPrivate PinosServerPortPrivate;

#include <pinos/client/introspect.h>
#include <pinos/client/port.h>
#include <pinos/server/daemon.h>

#define PINOS_TYPE_SERVER_PORT             (pinos_server_port_get_type ())
#define PINOS_IS_SERVER_PORT(obj)          (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_SERVER_PORT))
#define PINOS_IS_SERVER_PORT_CLASS(klass)  (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_SERVER_PORT))
#define PINOS_SERVER_PORT_GET_CLASS(obj)   (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_SERVER_PORT, PinosServerPortClass))
#define PINOS_SERVER_PORT(obj)             (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_SERVER_PORT, PinosServerPort))
#define PINOS_SERVER_PORT_CLASS(klass)     (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_SERVER_PORT, PinosServerPortClass))
#define PINOS_SERVER_PORT_CAST(obj)        ((PinosServerPort*)(obj))
#define PINOS_SERVER_PORT_CLASS_CAST(klass)((PinosServerPortClass*)(klass))

/**
 * PinosServerPort:
 *
 * Pinos port object class.
 */
struct _PinosServerPort {
  PinosPort object;

  PinosServerPortPrivate *priv;
};

/**
 * PinosServerPortClass:
 *
 * Pinos port object class.
 */
struct _PinosServerPortClass {
  PinosPortClass parent_class;
};

/* normal GObject stuff */
GType               pinos_server_port_get_type           (void);

const gchar *       pinos_server_port_get_object_path    (PinosServerPort *port);

G_END_DECLS

#endif /* __PINOS_SERVER_PORT_H__ */
