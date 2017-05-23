/* PipeWire
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

#ifndef __PIPEWIRE_SUBSCRIBE_H__
#define __PIPEWIRE_SUBSCRIBE_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIPEWIRE_TYPE__Core                          "PipeWire:Object:Core"
#define PIPEWIRE_TYPE_CORE_BASE                      PIPEWIRE_TYPE__Core ":"

#define PIPEWIRE_TYPE__Registry                      "PipeWire:Object:Registry"
#define PIPEWIRE_TYPE_REGISYRY_BASE                  PIPEWIRE_TYPE__Registry ":"

#define PIPEWIRE_TYPE__Node                          "PipeWire:Object:Node"
#define PIPEWIRE_TYPE_NODE_BASE                      PIPEWIRE_TYPE__Node ":"

#define PIPEWIRE_TYPE__Client                        "PipeWire:Object:Client"
#define PIPEWIRE_TYPE_CLIENT_BASE                    PIPEWIRE_TYPE__Client ":"

#define PIPEWIRE_TYPE__Link                          "PipeWire:Object:Link"
#define PIPEWIRE_TYPE_LINK_BASE                      PIPEWIRE_TYPE__Link ":"

#define PIPEWIRE_TYPE__Module                        "PipeWire:Object:Module"
#define PIPEWIRE_TYPE_MODULE_BASE                    PIPEWIRE_TYPE__Module ":"

enum pw_subscription_event {
    PW_SUBSCRIPTION_EVENT_NEW           = 0,
    PW_SUBSCRIPTION_EVENT_CHANGE        = 1,
    PW_SUBSCRIPTION_EVENT_REMOVE        = 2,
};

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_SUBSCRIBE_H__ */
