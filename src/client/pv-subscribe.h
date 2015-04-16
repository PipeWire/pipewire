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

typedef enum {
    PV_SUBSCRIPTION_FLAGS_CLIENT        = (1 << 0),
    PV_SUBSCRIPTION_FLAGS_DEVICE        = (1 << 1),
    PV_SUBSCRIPTION_FLAGS_SOURCE        = (1 << 2),
    PV_SUBSCRIPTION_FLAGS_SOURCE_OUTPUT = (1 << 3),

    PV_SUBSCRIPTION_FLAGS_ALL           = 0xf
} PvSubscriptionFlags;

typedef enum {
    PV_SUBSCRIPTION_EVENT_NEW           = 0,
    PV_SUBSCRIPTION_EVENT_CHANGE        = 1,
    PV_SUBSCRIPTION_EVENT_REMOVE        = 2,
} PvSubscriptionEvent;

G_END_DECLS

#endif /* __PV_SUBSCRIBE_H__ */

