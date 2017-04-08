/* Pinos
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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
#include <dlfcn.h>

#include "config.h"

#include "pinos/server/core.h"
#include "pinos/server/module.h"

#define AUDIOMIXER_LIB "build/spa/plugins/audiomixer/libspa-audiomixer.so"

typedef struct {
  PinosCore       *core;
  PinosProperties *properties;

  void *hnd;
  const SpaHandleFactory *factory;

  PinosListener  check_send;
  PinosListener  check_dispatch;
} ModuleImpl;

static const SpaHandleFactory *
find_factory (ModuleImpl *impl)
{
  SpaEnumHandleFactoryFunc enum_func;
  uint32_t index;
  const SpaHandleFactory *factory = NULL;
  SpaResult res;

  if ((impl->hnd = dlopen (AUDIOMIXER_LIB, RTLD_NOW)) == NULL) {
    pinos_log_error ("can't load %s: %s", AUDIOMIXER_LIB, dlerror());
    return NULL;
  }
  if ((enum_func = dlsym (impl->hnd, "spa_enum_handle_factory")) == NULL) {
    pinos_log_error ("can't find enum function");
    goto no_symbol;
  }

  for (index = 0; ; index++) {
    if ((res = enum_func (&factory, index)) < 0) {
      if (res != SPA_RESULT_ENUM_END)
        pinos_log_error ("can't enumerate factories: %d", res);
      goto enum_failed;
    }
    if (strcmp (factory->name, "audiomixer") == 0)
      break;
  }
  return factory;

enum_failed:
no_symbol:
  dlclose (impl->hnd);
  impl->hnd = NULL;
  return NULL;
}

static PinosNode *
make_node (ModuleImpl *impl)
{
  SpaHandle *handle;
  SpaResult res;
  void *iface;
  SpaNode *spa_node;
  SpaClock *spa_clock;
  PinosNode *node;

  handle = calloc (1, impl->factory->size);
  if ((res = spa_handle_factory_init (impl->factory,
                                      handle,
                                      NULL,
                                      impl->core->support,
                                      impl->core->n_support)) < 0) {
    pinos_log_error ("can't make factory instance: %d", res);
    goto init_failed;
  }
  if ((res = spa_handle_get_interface (handle,
                                       impl->core->type.spa_node,
                                       &iface)) < 0) {
    pinos_log_error ("can't get interface %d", res);
    goto interface_failed;
  }
  spa_node = iface;

  if ((res = spa_handle_get_interface (handle,
                                       impl->core->type.spa_clock,
                                       &iface)) < 0) {
    iface = NULL;
  }
  spa_clock = iface;

  node = pinos_node_new (impl->core,
                         "audiomixer",
                         false,
                         spa_node,
                         spa_clock,
                         NULL);
  return node;

interface_failed:
  spa_handle_clear (handle);
init_failed:
  free (handle);
  return NULL;
}

static ModuleImpl *
module_new (PinosCore       *core,
            PinosProperties *properties)
{
  ModuleImpl *impl;
  PinosNode *n;

  impl = calloc (1, sizeof (ModuleImpl));
  pinos_log_debug ("module %p: new", impl);

  impl->core = core;
  impl->properties = properties;

  impl->factory = find_factory (impl);

  spa_list_for_each (n, &core->node_list, link) {
    const char *str;
    char *error;
    PinosNode *node;
    PinosPort *ip, *op;

    if (n->global == NULL)
      continue;

    if (n->properties == NULL)
      continue;

    if ((str = pinos_properties_get (n->properties, "media.class")) == NULL)
      continue;

    if (strcmp (str, "Audio/Sink") != 0)
      continue;

    if ((ip = pinos_node_get_free_port (n, PINOS_DIRECTION_INPUT)) == NULL)
      continue;

    node = make_node (impl);
    op = pinos_node_get_free_port (node, PINOS_DIRECTION_OUTPUT);
    if (op == NULL)
      continue;

    pinos_port_link (op, ip, NULL, NULL, &error);
  }
  return impl;
}

#if 0
static void
module_destroy (ModuleImpl *impl)
{
  pinos_log_debug ("module %p: destroy", impl);

  free (impl);
}
#endif

bool
pinos__module_init (PinosModule * module, const char * args)
{
  module_new (module->core, NULL);
  return true;
}
