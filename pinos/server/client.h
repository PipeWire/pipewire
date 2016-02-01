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

#ifndef __PINOS_CLIENT_H__
#define __PINOS_CLIENT_H__

#include <glib-object.h>

#include <pinos/server/daemon.h>

G_BEGIN_DECLS

#define PINOS_TYPE_CLIENT                 (pinos_client_get_type ())
#define PINOS_IS_CLIENT(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_CLIENT))
#define PINOS_IS_CLIENT_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_CLIENT))
#define PINOS_CLIENT_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_CLIENT, PinosClientClass))
#define PINOS_CLIENT(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_CLIENT, PinosClient))
#define PINOS_CLIENT_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_CLIENT, PinosClientClass))
#define PINOS_CLIENT_CAST(obj)            ((PinosClient*)(obj))
#define PINOS_CLIENT_CLASS_CAST(klass)    ((PinosClientClass*)(klass))

typedef struct _PinosClient PinosClient;
typedef struct _PinosClientClass PinosClientClass;
typedef struct _PinosClientPrivate PinosClientPrivate;

/**
 * PinosClient:
 *
 * Pinos client object class.
 */
struct _PinosClient {
  GObject object;

  PinosClientPrivate *priv;
};

/**
 * PinosClientClass:
 *
 * Pinos client object class.
 */
struct _PinosClientClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType           pinos_client_get_type             (void);

PinosClient *   pinos_client_new                  (PinosDaemon     *daemon,
                                                   const gchar     *sender,
                                                   const gchar     *prefix,
                                                   PinosProperties *properties);

const gchar *   pinos_client_get_sender           (PinosClient *client);
const gchar *   pinos_client_get_object_path      (PinosClient *client);

G_END_DECLS

#endif /* __PINOS_CLIENT_H__ */
