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

#ifndef __PINOS_CHANNEL_H__
#define __PINOS_CHANNEL_H__

#include <glib-object.h>

G_BEGIN_DECLS

#define PINOS_TYPE_CHANNEL                 (pinos_channel_get_type ())
#define PINOS_IS_CHANNEL(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_CHANNEL))
#define PINOS_IS_CHANNEL_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_CHANNEL))
#define PINOS_CHANNEL_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_CHANNEL, PinosChannelClass))
#define PINOS_CHANNEL(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_CHANNEL, PinosChannel))
#define PINOS_CHANNEL_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_CHANNEL, PinosChannelClass))
#define PINOS_CHANNEL_CAST(obj)            ((PinosChannel*)(obj))
#define PINOS_CHANNEL_CLASS_CAST(klass)    ((PinosChannelClass*)(klass))

typedef struct _PinosChannel PinosChannel;
typedef struct _PinosChannelClass PinosChannelClass;
typedef struct _PinosChannelPrivate PinosChannelPrivate;

/**
 * PinosChannel:
 *
 * Pinos source channel object class.
 */
struct _PinosChannel {
  GObject object;

  PinosChannelPrivate *priv;
};

/**
 * PinosChannelClass:
 *
 * Pinos source channel object class.
 */
struct _PinosChannelClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType              pinos_channel_get_type             (void);

void               pinos_channel_remove               (PinosChannel *channel);

const gchar *      pinos_channel_get_client_path      (PinosChannel *channel);
const gchar *      pinos_channel_get_object_path      (PinosChannel *channel);

GSocket *          pinos_channel_get_socket_pair      (PinosChannel  *channel,
                                                       GError       **error);

G_END_DECLS

#endif /* __PINOS_CHANNEL_H__ */
