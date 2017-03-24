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

#ifndef __PINOS_CONTEXT_H__
#define __PINOS_CONTEXT_H__

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _PinosContext PinosContext;

#include <pinos/client/map.h>
#include <pinos/client/loop.h>
#include <pinos/client/properties.h>
#include <pinos/client/subscribe.h>
#include <pinos/client/proxy.h>
#include <pinos/client/type.h>

/**
 * PinosContextState:
 * @PINOS_CONTEXT_STATE_ERROR: context is in error
 * @PINOS_CONTEXT_STATE_UNCONNECTED: not connected
 * @PINOS_CONTEXT_STATE_CONNECTING: connecting to daemon
 * @PINOS_CONTEXT_STATE_CONNECTED: context is connected and ready
 *
 * The state of a #PinosContext
 */
typedef enum {
  PINOS_CONTEXT_STATE_ERROR        = -1,
  PINOS_CONTEXT_STATE_UNCONNECTED  = 0,
  PINOS_CONTEXT_STATE_CONNECTING   = 1,
  PINOS_CONTEXT_STATE_CONNECTED    = 2,
} PinosContextState;

const char * pinos_context_state_as_string (PinosContextState state);

/**
 * PinosContext:
 *
 * Pinos context object class.
 */
struct _PinosContext {
  char            *name;
  PinosProperties *properties;

  PinosType        type;

  PinosLoop       *loop;

  PinosProxy      *core_proxy;
  PinosProxy      *registry_proxy;

  PinosMap         objects;
  uint32_t         n_types;
  PinosMap         types;

  SpaList          global_list;
  SpaList          stream_list;
  SpaList          proxy_list;

  void            *protocol_private;

  PinosContextState state;
  char *error;
  PINOS_SIGNAL (state_changed,  (PinosListener *listener,
                                 PinosContext  *context));

  PINOS_SIGNAL (subscription,   (PinosListener          *listener,
                                 PinosContext           *context,
                                 PinosSubscriptionEvent  event,
                                 uint32_t                type,
                                 uint32_t                id));

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosContext  *context));
};

PinosContext *    pinos_context_new                   (PinosLoop         *loop,
                                                       const char        *name,
                                                       PinosProperties   *properties);
void              pinos_context_destroy               (PinosContext      *context);

bool              pinos_context_connect               (PinosContext      *context);
bool              pinos_context_connect_fd            (PinosContext      *context,
                                                       int                fd);
bool              pinos_context_disconnect            (PinosContext      *context);

#ifdef __cplusplus
}
#endif

#endif /* __PINOS_CONTEXT_H__ */
