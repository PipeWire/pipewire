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

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PINOS_SUBSCRIPTION_STATE_UNCONNECTED     = 0,
    PINOS_SUBSCRIPTION_STATE_CONNECTING      = 1,
    PINOS_SUBSCRIPTION_STATE_READY           = 2,
    PINOS_SUBSCRIPTION_STATE_ERROR           = 3,
} PinosSubscriptionState;

typedef enum {
    PINOS_SUBSCRIPTION_FLAG_DAEMON          = (1 << 0),
    PINOS_SUBSCRIPTION_FLAG_CLIENT          = (1 << 1),
    PINOS_SUBSCRIPTION_FLAG_NODE            = (1 << 2),
    PINOS_SUBSCRIPTION_FLAG_LINK            = (1 << 3)
} PinosSubscriptionFlags;

#define PINOS_SUBSCRIPTION_FLAGS_ALL 0x0f

typedef enum {
    PINOS_SUBSCRIPTION_EVENT_NEW           = 0,
    PINOS_SUBSCRIPTION_EVENT_CHANGE        = 1,
    PINOS_SUBSCRIPTION_EVENT_REMOVE        = 2,
} PinosSubscriptionEvent;

typedef void (*PinosSubscriptionFunc)  (PinosContext           *context,
                                        PinosSubscriptionFlags  flags,
                                        PinosSubscriptionEvent  event,
                                        uint32_t                id,
                                        void                   *data);

void         pinos_context_subscribe  (PinosContext           *context,
                                       PinosSubscriptionFlags  mask,
                                       PinosSubscriptionFunc   func,
                                       void                   *data);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_SUBSCRIBE_H__ */
