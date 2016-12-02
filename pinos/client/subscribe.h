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

#define PINOS_CORE_URI                            "http://pinos.org/ns/core"
#define PINOS_CORE_PREFIX                         PINOS_CORE_URI "#"
#define PINOS_CORE_REGISTRY                       PINOS_CORE_PREFIX "Registry"

#define PINOS_NODE_URI                            "http://pinos.org/ns/node"
#define PINOS_NODE_PREFIX                         PINOS_NODE_URI "#"

#define PINOS_CLIENT_URI                          "http://pinos.org/ns/client"
#define PINOS_CLIENT_PREFIX                       PINOS_CLIENT_URI "#"

#define PINOS_LINK_URI                            "http://pinos.org/ns/link"
#define PINOS_LINK_PREFIX                         PINOS_LINK_URI "#"

#define PINOS_MODULE_URI                          "http://pinos.org/ns/module"
#define PINOS_MODULE_PREFIX                       PINOS_MODULE_URI "#"

typedef enum {
    PINOS_SUBSCRIPTION_EVENT_NEW           = 0,
    PINOS_SUBSCRIPTION_EVENT_CHANGE        = 1,
    PINOS_SUBSCRIPTION_EVENT_REMOVE        = 2,
} PinosSubscriptionEvent;

typedef void (*PinosSubscriptionFunc)  (PinosContext           *context,
                                        PinosSubscriptionEvent  event,
                                        uint32_t                type,
                                        uint32_t                id,
                                        void                   *data);

void         pinos_context_subscribe  (PinosContext           *context,
                                       PinosSubscriptionFunc   func,
                                       void                   *data);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_SUBSCRIBE_H__ */
