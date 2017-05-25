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

#ifndef __PIPEWIRE_CONTEXT_H__
#define __PIPEWIRE_CONTEXT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <pipewire/client/map.h>
#include <pipewire/client/loop.h>
#include <pipewire/client/properties.h>
#include <pipewire/client/subscribe.h>
#include <pipewire/client/proxy.h>
#include <pipewire/client/type.h>

/**
 * pw_context_state:
 * @PW_CONTEXT_STATE_ERROR: context is in error
 * @PW_CONTEXT_STATE_UNCONNECTED: not connected
 * @PW_CONTEXT_STATE_CONNECTING: connecting to daemon
 * @PW_CONTEXT_STATE_CONNECTED: context is connected and ready
 *
 * The state of a pw_context
 */
enum pw_context_state {
  PW_CONTEXT_STATE_ERROR        = -1,
  PW_CONTEXT_STATE_UNCONNECTED  = 0,
  PW_CONTEXT_STATE_CONNECTING   = 1,
  PW_CONTEXT_STATE_CONNECTED    = 2,
};

const char * pw_context_state_as_string (enum pw_context_state state);

enum pw_context_flags {
  PW_CONTEXT_FLAG_NONE          = 0,
  PW_CONTEXT_FLAG_NO_REGISTRY   = (1 << 0),
  PW_CONTEXT_FLAG_NO_PROXY      = (1 << 1),
};

/**
 * pw_context:
 *
 * PipeWire context object class.
 */
struct pw_context {
  char                 *name;
  struct pw_properties *properties;

  struct pw_type        type;

  struct pw_loop       *loop;

  struct pw_proxy      *core_proxy;
  struct pw_proxy      *registry_proxy;

  struct pw_map         objects;
  uint32_t              n_types;
  struct pw_map         types;

  struct spa_list       global_list;
  struct spa_list       stream_list;
  struct spa_list       proxy_list;

  void            *protocol_private;

  enum pw_context_state state;
  char *error;
  PW_SIGNAL (state_changed,  (struct pw_listener *listener,
                              struct pw_context  *context));

  PW_SIGNAL (subscription,   (struct pw_listener         *listener,
                              struct pw_context          *context,
                              enum pw_subscription_event  event,
                              uint32_t                    type,
                              uint32_t                    id));

  PW_SIGNAL (destroy_signal, (struct pw_listener *listener,
                              struct pw_context  *context));
};

struct pw_context * pw_context_new                   (struct pw_loop        *loop,
                                                      const char            *name,
                                                      struct pw_properties  *properties);
void                pw_context_destroy               (struct pw_context     *context);

bool                pw_context_connect               (struct pw_context     *context,
                                                      enum pw_context_flags  flags);
bool                pw_context_connect_fd            (struct pw_context     *context,
                                                      enum pw_context_flags  flags,
                                                      int                    fd);
bool                pw_context_disconnect            (struct pw_context     *context);

#ifdef __cplusplus
}
#endif

#endif /* __PIPEWIRE_CONTEXT_H__ */
