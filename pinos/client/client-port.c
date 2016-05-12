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

#include <string.h>

#include <gst/gst.h>
#include <gio/gio.h>

#include "pinos/client/pinos.h"
#include "pinos/client/enumtypes.h"

#include "pinos/client/client-port.h"

#define PINOS_CLIENT_PORT_GET_PRIVATE(obj)  \
   (G_TYPE_INSTANCE_GET_PRIVATE ((obj), PINOS_TYPE_CLIENT_PORT, PinosClientPortPrivate))

struct _PinosClientPortPrivate
{
  gint foo;
};

G_DEFINE_TYPE (PinosClientPort, pinos_client_port, PINOS_TYPE_PORT);

enum
{
  PROP_0,
};

static void
pinos_client_port_get_property (GObject    *_object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PinosClientPort *port = PINOS_CLIENT_PORT (_object);

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (port, prop_id, pspec);
      break;
  }
}

static void
pinos_client_port_set_property (GObject      *_object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosClientPort *port = PINOS_CLIENT_PORT (_object);

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (port, prop_id, pspec);
      break;
  }
}

static void
pinos_client_port_constructed (GObject * object)
{
  PinosClientPort *port = PINOS_CLIENT_PORT (object);

  g_debug ("client-port %p: constructed", port);

  G_OBJECT_CLASS (pinos_client_port_parent_class)->constructed (object);
}

static void
pinos_client_port_dispose (GObject * object)
{
  PinosClientPort *port = PINOS_CLIENT_PORT (object);

  g_debug ("client-port %p: dispose", port);

  G_OBJECT_CLASS (pinos_client_port_parent_class)->dispose (object);
}

static void
pinos_client_port_finalize (GObject * object)
{
  PinosClientPort *port = PINOS_CLIENT_PORT (object);

  g_debug ("client-port %p: finalize", port);

  G_OBJECT_CLASS (pinos_client_port_parent_class)->finalize (object);
}

static void
pinos_client_port_class_init (PinosClientPortClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosClientPortPrivate));

  gobject_class->constructed = pinos_client_port_constructed;
  gobject_class->dispose = pinos_client_port_dispose;
  gobject_class->finalize = pinos_client_port_finalize;
  gobject_class->set_property = pinos_client_port_set_property;
  gobject_class->get_property = pinos_client_port_get_property;
}

static void
pinos_client_port_init (PinosClientPort * port)
{
  port->priv = PINOS_CLIENT_PORT_GET_PRIVATE (port);
}
