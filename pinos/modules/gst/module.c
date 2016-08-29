/* Pinos
 * Copyright (C) 2016 Axis Communications <dev-gstreamer@axis.com>
 * @author Linus Svensson <linus.svensson@axis.com>
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

#include <server/daemon.h>
#include <server/module.h>

#include "gst-manager.h"
#include "gst-node-factory.h"

gboolean pinos__module_init (PinosModule *module);

G_MODULE_EXPORT gboolean
pinos__module_init (PinosModule * module)
{
  PinosNodeFactory *factory;

  pinos_gst_manager_new (module->daemon);

  factory = pinos_gst_node_factory_new ("gst-node-factory");
  pinos_daemon_add_node_factory (module->daemon, factory);

  g_object_unref (factory);

  return TRUE;
}
