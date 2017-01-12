/* Pinos
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "config.h"

#include "pinos/server/core.h"
#include "pinos/server/module.h"

typedef struct {
  PinosCore       *core;
  PinosProperties *properties;

  PinosListener  check_send;
  PinosListener  check_dispatch;
} ModuleImpl;

static bool
check_global_owner (PinosCore   *core,
                    PinosClient *client,
                    uint32_t     id)
{
  PinosGlobal *global;

  global = pinos_map_lookup (&core->objects, id);
  if (global == NULL)
    return false;

  if (global->owner == NULL)
    return true;

  if (global->owner->ucred.uid == client->ucred.uid)
    return true;

  return false;
}

static void
do_check_send (PinosListener    *listener,
               PinosAccessFunc   func,
               PinosAccessData  *data)
{
  PinosClient *client = data->client;
  PinosCore *core = client->core;

  if (data->resource->type == core->uri.registry) {
    switch (data->opcode) {
      case PINOS_MESSAGE_NOTIFY_GLOBAL:
      {
        PinosMessageNotifyGlobal *m = data->message;

        if (check_global_owner (core, client, m->id))
          data->res = SPA_RESULT_OK;
        else
          data->res = SPA_RESULT_SKIPPED;
        break;
      }
      case PINOS_MESSAGE_NOTIFY_GLOBAL_REMOVE:
      {
        PinosMessageNotifyGlobalRemove *m = data->message;

        if (check_global_owner (core, client, m->id))
          data->res = SPA_RESULT_OK;
        else
          data->res = SPA_RESULT_SKIPPED;
        break;
      }

      default:
        data->res = SPA_RESULT_NO_PERMISSION;
        break;
    }
  }
  else {
    data->res = SPA_RESULT_OK;
  }
}

static void
do_check_dispatch (PinosListener    *listener,
                   PinosAccessFunc   func,
                   PinosAccessData  *data)
{
  PinosClient *client = data->client;
  PinosCore *core = client->core;

  if (data->resource->type == core->uri.registry) {
    if (data->opcode == PINOS_MESSAGE_BIND) {
      PinosMessageBind *m = data->message;

      if (check_global_owner (core, client, m->id))
        data->res = SPA_RESULT_OK;
      else
        data->res = SPA_RESULT_NO_PERMISSION;
    } else {
      data->res = SPA_RESULT_NO_PERMISSION;
    }
  }
  else {
    data->res = SPA_RESULT_OK;
  }
}

static ModuleImpl *
module_new (PinosCore       *core,
            PinosProperties *properties)
{
  ModuleImpl *impl;

  impl = calloc (1, sizeof (ModuleImpl));
  pinos_log_debug ("module %p: new", impl);

  impl->core = core;
  impl->properties = properties;

  pinos_signal_add (&core->access.check_send,
                    &impl->check_send,
                    do_check_send);
  pinos_signal_add (&core->access.check_dispatch,
                    &impl->check_dispatch,
                    do_check_dispatch);

  return impl;
}

#if 0
static void
module_destroy (ModuleImpl *impl)
{
  pinos_log_debug ("module %p: destroy", impl);

  pinos_global_destroy (impl->global);

  pinos_signal_remove (&impl->global_added);
  pinos_signal_remove (&impl->global_removed);
  pinos_signal_remove (&impl->port_added);
  pinos_signal_remove (&impl->port_removed);
  pinos_signal_remove (&impl->port_unlinked);
  pinos_signal_remove (&impl->link_state_changed);
  free (impl);
}
#endif

bool
pinos__module_init (PinosModule * module, const char * args)
{
  module_new (module->core, NULL);
  return true;
}
