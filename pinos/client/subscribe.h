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

#define PINOS_TYPE__Core                          "Pinos:Object:Core"
#define PINOS_TYPE_CORE_BASE                      PINOS_TYPE__Core ":"

#define PINOS_TYPE__Registry                      "Pinos:Object:Registry"
#define PINOS_TYPE_REGISYRY_BASE                  PINOS_TYPE__Registry ":"

#define PINOS_TYPE__Node                          "Pinos:Object:Node"
#define PINOS_TYPE_NODE_BASE                      PINOS_TYPE__Node ":"

#define PINOS_TYPE__Client                        "Pinos:Object:Client"
#define PINOS_TYPE_CLIENT_BASE                    PINOS_TYPE__Client ":"

#define PINOS_TYPE__Link                          "Pinos:Object:Link"
#define PINOS_TYPE_LINK_BASE                      PINOS_TYPE__Link ":"

#define PINOS_TYPE__Module                        "Pinos:Object:Module"
#define PINOS_TYPE_MODULE_BASE                    PINOS_TYPE__Module ":"

typedef enum {
    PINOS_SUBSCRIPTION_EVENT_NEW           = 0,
    PINOS_SUBSCRIPTION_EVENT_CHANGE        = 1,
    PINOS_SUBSCRIPTION_EVENT_REMOVE        = 2,
} PinosSubscriptionEvent;

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_SUBSCRIBE_H__ */
