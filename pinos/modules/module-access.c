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
                    PinosGlobal *global)
{
  pinos_log_debug ("%p", global);

  if (global == NULL)
    return false;

  pinos_log_debug ("%p", global->owner);

  if (global->owner == NULL)
    return true;

  pinos_log_debug ("%d %d", global->owner->ucred.uid, client->ucred.uid);

  if (global->owner->ucred.uid == client->ucred.uid)
    return true;

  return false;
}

static SpaResult
do_view_global (PinosAccess      *access,
                PinosClient      *client,
                PinosGlobal      *global)
{
  if (global->type == client->core->type.link) {
    PinosLink *link = global->object;

    pinos_log_debug ("link %p: global %p %p %p %p", link, global->owner, client, link->output, link->input);

    /* we must be able to see both nodes */
    if (link->output && !check_global_owner (client->core, client, link->output->node->global))
      return SPA_RESULT_ERROR;

    pinos_log_debug ("link %p: global %p %p %p %p", link, global->owner, client, link->output, link->input);

    if (link->input && !check_global_owner (client->core, client, link->input->node->global))
      return SPA_RESULT_ERROR;

    pinos_log_debug ("link %p: global %p %p %p %p", link, global->owner, client, link->output, link->input);
  }
  else if (!check_global_owner (client->core, client, global))
    return SPA_RESULT_ERROR;

  return SPA_RESULT_OK;
}

static SpaResult
do_create_node (PinosAccess      *access,
                PinosAccessData  *data,
                const char       *factory_name,
                const char       *name,
                PinosProperties  *properties)
{
  data->res = SPA_RESULT_OK;
  data->complete_cb (data);
  return SPA_RESULT_OK;
}

#if 0
static void
check_timeout (SpaSource *source,
               void      *d)
{
  PinosAccessData *data = d;

  data->res = SPA_RESULT_OK;
  data->complete_cb (data);
}
#endif

static SpaResult
do_create_client_node (PinosAccess      *access,
                       PinosAccessData  *data,
                       const char       *name,
                       PinosProperties  *properties)
{
#if 0
  struct timespec value;
  SpaSource *timer;

  pinos_log_debug ("access %p: check %s %p", access, name, properties);

  timer = pinos_loop_add_timer (data->resource->core->main_loop->loop,
                                check_timeout,
                                data->async_copy (data, 0));
  value.tv_sec = 3;
  value.tv_nsec = 0;
  pinos_loop_update_timer (data->resource->core->main_loop->loop,
                           timer,
                           &value,
                           NULL,
                           false);

  return SPA_RESULT_RETURN_ASYNC (0);
#else
  data->res = SPA_RESULT_OK;
  return SPA_RESULT_OK;
#endif
}

static PinosAccess access_checks =
{
  do_view_global,
  do_create_node,
  do_create_client_node,
};

static ModuleImpl *
module_new (PinosCore       *core,
            PinosProperties *properties)
{
  ModuleImpl *impl;

  impl = calloc (1, sizeof (ModuleImpl));
  pinos_log_debug ("module %p: new", impl);

  impl->core = core;
  impl->properties = properties;

  core->access = &access_checks;

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
