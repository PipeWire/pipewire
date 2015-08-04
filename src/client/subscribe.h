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

#ifndef __PINOS_SUBSCRIBE_H__
#define __PINOS_SUBSCRIBE_H__

#include <glib.h>

G_BEGIN_DECLS

#define PINOS_TYPE_SUBSCRIBE                 (pinos_subscribe_get_type ())
#define PINOS_IS_SUBSCRIBE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PINOS_TYPE_SUBSCRIBE))
#define PINOS_IS_SUBSCRIBE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PINOS_TYPE_SUBSCRIBE))
#define PINOS_SUBSCRIBE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PINOS_TYPE_SUBSCRIBE, PinosSubscribeClass))
#define PINOS_SUBSCRIBE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PINOS_TYPE_SUBSCRIBE, PinosSubscribe))
#define PINOS_SUBSCRIBE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PINOS_TYPE_SUBSCRIBE, PinosSubscribeClass))
#define PINOS_SUBSCRIBE_CAST(obj)            ((PinosSubscribe*)(obj))
#define PINOS_SUBSCRIBE_CLASS_CAST(klass)    ((PinosSubscribeClass*)(klass))

typedef struct _PinosSubscribe PinosSubscribe;
typedef struct _PinosSubscribeClass PinosSubscribeClass;
typedef struct _PinosSubscribePrivate PinosSubscribePrivate;

typedef enum {
    PINOS_SUBSCRIPTION_STATE_UNCONNECTED     = 0,
    PINOS_SUBSCRIPTION_STATE_CONNECTING      = 1,
    PINOS_SUBSCRIPTION_STATE_READY           = 2,
    PINOS_SUBSCRIPTION_STATE_ERROR           = 3,
} PinosSubscriptionState;

typedef enum {
    PINOS_SUBSCRIPTION_FLAG_DAEMON          = (1 << 0),
    PINOS_SUBSCRIPTION_FLAG_CLIENT          = (1 << 1),
    PINOS_SUBSCRIPTION_FLAG_SOURCE          = (1 << 2),
    PINOS_SUBSCRIPTION_FLAG_SOURCE_OUTPUT   = (1 << 3),
} PinosSubscriptionFlags;

#define PINOS_SUBSCRIPTION_FLAGS_ALL 0xf

typedef enum {
    PINOS_SUBSCRIPTION_EVENT_NEW           = 0,
    PINOS_SUBSCRIPTION_EVENT_CHANGE        = 1,
    PINOS_SUBSCRIPTION_EVENT_REMOVE        = 2,
} PinosSubscriptionEvent;

/**
 * PinosSubscribe:
 *
 * Pinos subscribe object class.
 */
struct _PinosSubscribe {
  GObject object;

  PinosSubscribePrivate *priv;
};

/**
 * PinosSubscribeClass:
 *
 * Pinos subscribe object class.
 */
struct _PinosSubscribeClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType                  pinos_subscribe_get_type           (void);

PinosSubscribe *       pinos_subscribe_new                (void);

PinosSubscriptionState pinos_subscribe_get_state          (PinosSubscribe *subscribe);
GError *               pinos_subscribe_get_error          (PinosSubscribe *subscribe);

void                   pinos_subscribe_get_proxy          (PinosSubscribe      *subscribe,
                                                           const gchar         *name,
                                                           const gchar         *object_path,
                                                           const gchar         *interface_name,
                                                           GCancellable        *cancellable,
                                                           GAsyncReadyCallback  callback,
                                                           gpointer             user_data);
GDBusProxy *           pinos_subscribe_get_proxy_finish   (PinosSubscribe *subscribe,
                                                           GAsyncResult   *res,
                                                           GError         **error);



G_END_DECLS

#endif /* __PINOS_SUBSCRIBE_H__ */

