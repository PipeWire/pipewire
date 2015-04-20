/* Pulsevideo
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

#ifndef __PV_SUBSCRIBE_H__
#define __PV_SUBSCRIBE_H__

#include <glib.h>

G_BEGIN_DECLS

#define PV_TYPE_SUBSCRIBE                 (pv_subscribe_get_type ())
#define PV_IS_SUBSCRIBE(obj)              (G_TYPE_CHECK_INSTANCE_TYPE ((obj), PV_TYPE_SUBSCRIBE))
#define PV_IS_SUBSCRIBE_CLASS(klass)      (G_TYPE_CHECK_CLASS_TYPE ((klass), PV_TYPE_SUBSCRIBE))
#define PV_SUBSCRIBE_GET_CLASS(obj)       (G_TYPE_INSTANCE_GET_CLASS ((obj), PV_TYPE_SUBSCRIBE, PvSubscribeClass))
#define PV_SUBSCRIBE(obj)                 (G_TYPE_CHECK_INSTANCE_CAST ((obj), PV_TYPE_SUBSCRIBE, PvSubscribe))
#define PV_SUBSCRIBE_CLASS(klass)         (G_TYPE_CHECK_CLASS_CAST ((klass), PV_TYPE_SUBSCRIBE, PvSubscribeClass))
#define PV_SUBSCRIBE_CAST(obj)            ((PvSubscribe*)(obj))
#define PV_SUBSCRIBE_CLASS_CAST(klass)    ((PvSubscribeClass*)(klass))

typedef struct _PvSubscribe PvSubscribe;
typedef struct _PvSubscribeClass PvSubscribeClass;
typedef struct _PvSubscribePrivate PvSubscribePrivate;

typedef enum {
    PV_SUBSCRIPTION_FLAGS_CLIENT        = (1 << 0),
    PV_SUBSCRIPTION_FLAGS_SOURCE        = (1 << 1),
    PV_SUBSCRIPTION_FLAGS_SOURCE_OUTPUT = (1 << 2),
} PvSubscriptionFlags;

#define PV_SUBSCRIPTION_FLAGS_ALL 0x3

typedef enum {
    PV_SUBSCRIPTION_EVENT_NEW           = 0,
    PV_SUBSCRIPTION_EVENT_CHANGE        = 1,
    PV_SUBSCRIPTION_EVENT_REMOVE        = 2,
} PvSubscriptionEvent;

/**
 * PvSubscribe:
 *
 * Pulsevideo subscribe object class.
 */
struct _PvSubscribe {
  GObject object;

  PvSubscribePrivate *priv;
};

/**
 * PvSubscribeClass:
 *
 * Pulsevideo subscribe object class.
 */
struct _PvSubscribeClass {
  GObjectClass parent_class;
};

/* normal GObject stuff */
GType             pv_subscribe_get_type              (void);

PvSubscribe *     pv_subscribe_new                   (void);

G_END_DECLS

#endif /* __PV_SUBSCRIBE_H__ */

