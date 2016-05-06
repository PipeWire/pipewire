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
#include <pinos/server/node.h>
#include <pinos/server/channel.h>

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
 * @get_formats: called to get a list of supported formats from the port
 * @create_channel: called to create a new channel object
 * @release_channel: called to release a channel object
 *
 * Pinos port object class.
 */
struct _PinosPortClass {
  GObjectClass parent_class;

  PinosChannel *      (*create_channel)  (PinosPort       *port,
                                          const gchar     *client_path,
                                          GBytes          *format_filter,
                                          PinosProperties *props,
                                          GError          **error);
  gboolean            (*release_channel) (PinosPort       *port,
                                          PinosChannel    *channel);
};

/* normal GObject stuff */
GType               pinos_port_get_type                  (void);

PinosPort *         pinos_port_new                       (PinosDaemon     *daemon,
                                                          const gchar     *node_path,
                                                          PinosDirection   direction,
                                                          const gchar     *name,
                                                          GBytes          *possible_formats,
                                                          PinosProperties *props);

const gchar *       pinos_port_get_object_path           (PinosPort *port);

GBytes *            pinos_port_get_formats               (PinosPort   *port,
                                                          GBytes      *filter,
                                                          GError     **error);

PinosChannel *      pinos_port_create_channel            (PinosPort       *port,
                                                          const gchar     *client_path,
                                                          GBytes          *format_filter,
                                                          PinosProperties *props,
                                                          GError         **error);
gboolean            pinos_port_release_channel           (PinosPort       *port,
                                                          PinosChannel    *channel);
GList *             pinos_port_get_channels              (PinosPort       *port);

G_END_DECLS

#endif /* __PINOS_PORT_H__ */
