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

#ifndef __PINOS_PORT_H__
#define __PINOS_PORT_H__

#include <glib-object.h>

G_BEGIN_DECLS

typedef struct _PinosPort PinosPort;
typedef struct _PinosPortClass PinosPortClass;
typedef struct _PinosPortPrivate PinosPortPrivate;

#include <pinos/client/introspect.h>
#include <pinos/client/buffer.h>

#define PINOS_TYPE_PORT                 (pinos_port_get_type ())
#define PINOS_IS_PORT(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_PORT))
#define PINOS_IS_PORT_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_PORT))
#define PINOS_PORT_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_PORT, PinosPortClass))
#define PINOS_PORT(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_PORT, PinosPort))
#define PINOS_PORT_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_PORT, PinosPortClass))
#define PINOS_PORT_CAST(obj)            ((PinosPort*)(obj))
#define PINOS_PORT_CLASS_CAST(klass)    ((PinosPortClass*)(klass))

/**
 * PinosPort:
 *
 * Pinos port object class.
 */
struct _PinosPort {
  GObject object;

  PinosPortPrivate *priv;
};

/**
 * PinosPortClass:
 *
 * Pinos port object class.
 */
struct _PinosPortClass {
  GObjectClass parent_class;
};

typedef void (*PinosReceivedBufferCallback) (PinosPort *port, gpointer user_data);


/* normal GObject stuff */
GType               pinos_port_get_type           (void);

void                pinos_port_set_received_buffer_cb (PinosPort *port,
                                                       PinosReceivedBufferCallback cb,
                                                       gpointer user_data,
                                                       GDestroyNotify notify);

void                pinos_port_remove               (PinosPort *port);

PinosNode *         pinos_port_get_node             (PinosPort *port);
GSocket *           pinos_port_get_socket           (PinosPort *port);
const gchar *       pinos_port_get_name             (PinosPort *port);
PinosDirection      pinos_port_get_direction        (PinosPort *port);
GBytes *            pinos_port_get_possible_formats (PinosPort *port);
GBytes *            pinos_port_get_format           (PinosPort *port);
PinosProperties *   pinos_port_get_properties       (PinosPort *port);

GBytes *            pinos_port_filter_formats       (PinosPort *port,
                                                     GBytes    *filter,
                                                     GError   **error);

GSocket *           pinos_port_get_socket_pair      (PinosPort *port,
                                                     GError   **error);

gboolean            pinos_port_link                 (PinosPort   *source,
                                                     PinosPort   *destination);
gboolean            pinos_port_unlink               (PinosPort   *source,
                                                     PinosPort   *destination);
PinosPort *         pinos_port_get_links            (PinosPort   *port,
                                                     guint       *n_links);

PinosBuffer *       pinos_port_peek_buffer          (PinosPort   *port);

void                pinos_port_buffer_builder_init  (PinosPort   *port,
                                                     PinosBufferBuilder *builder);

gboolean            pinos_port_send_buffer          (PinosPort   *port,
                                                     PinosBuffer *buffer,
                                                     GError     **error);

G_END_DECLS

#endif /* __PINOS_PORT_H__ */
