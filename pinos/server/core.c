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

#include <pinos/server/core.h>
#include <pinos/server/data-loop.h>

typedef struct {
  PinosCore object;

  PinosDataLoop *data_loop;

  SpaSupport support[4];

} PinosCoreImpl;

static void
core_destroy (PinosObject *object)
{
  free (object);
}

PinosCore *
pinos_core_new (PinosMainLoop *main_loop)
{
  PinosCoreImpl *impl;

  impl = calloc (1, sizeof (PinosCoreImpl));
  pinos_registry_init (&impl->object.registry);

  pinos_object_init (&impl->object.object,
                     impl->object.registry.uri.core,
                     impl,
                     core_destroy);

  impl->data_loop = pinos_data_loop_new ();
  impl->object.main_loop = main_loop;

  impl->support[0].uri = SPA_ID_MAP_URI;
  impl->support[0].data = impl->object.registry.map;
  impl->support[1].uri = SPA_LOG_URI;
  impl->support[1].data = pinos_log_get ();
  impl->support[2].uri = SPA_POLL__DataLoop;
  impl->support[2].data = &impl->data_loop->poll;
  impl->support[3].uri = SPA_POLL__MainLoop;
  impl->support[3].data = &impl->object.main_loop->poll;
  impl->object.support = impl->support;
  impl->object.n_support = 4;

  return &impl->object;
}
