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

#ifndef __PINOS_CLIENT_PORT_H__
#define __PINOS_CLIENT_PORT_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosClientPort PinosClientPort;
typedef struct _PinosClientPortClass PinosClientPortClass;
typedef struct _PinosClientPortPrivate PinosClientPortPrivate;

#include <pinos/client/introspect.h>
#include <pinos/client/port.h>
#include <pinos/client/client-node.h>

#define PINOS_TYPE_CLIENT_PORT                 (pinos_client_port_get_type ())
#define PINOS_IS_CLIENT_PORT(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_CLIENT_PORT))
#define PINOS_IS_CLIENT_PORT_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_CLIENT_PORT))
#define PINOS_CLIENT_PORT_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_CLIENT_PORT, PinosClientPortClass))
#define PINOS_CLIENT_PORT(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_CLIENT_PORT, PinosClientPort))
#define PINOS_CLIENT_PORT_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_CLIENT_PORT, PinosClientPortClass))
#define PINOS_CLIENT_PORT_CAST(obj)            ((PinosClientPort*)(obj))
#define PINOS_CLIENT_PORT_CLASS_CAST(klass)    ((PinosClientPortClass*)(klass))

/**
 * PinosClientPort:
 *
 * Pinos client port object class.
 */
struct _PinosClientPort {
  PinosPort object;

  PinosClientPortPrivate *priv;
};

/**
 * PinosClientPortClass:
 * @get_formats: called to get a list of supported formats from the port
 * @create_channel: called to create a new channel object
 * @release_channel: called to release a channel object
 *
 * Pinos client port object class.
 */
struct _PinosClientPortClass {
  PinosPortClass parent_class;
};

/* normal GObject stuff */
GType               pinos_client_port_get_type           (void);

PinosClientPort *   pinos_client_port_new                (PinosClientNode *node,
                                                          gpointer         id,
                                                          GSocket         *socket);

GBytes *            pinos_client_port_get_formats        (PinosClientPort *port,
                                                          GBytes          *filter,
                                                          GError         **error);

G_END_DECLS

#endif /* __PINOS_CLIENT_PORT_H__ */
