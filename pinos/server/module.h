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

#ifndef __PINOS_MODULE_H__
#define __PINOS_MODULE_H__


#include <pinos/server/core.h>

typedef struct _PinosModule PinosModule;

struct _PinosModule {
  SpaList link;
  gchar *name;
  PinosCore *core;

  PINOS_SIGNAL (destroy_signal, (PinosListener *listener,
                                 PinosModule   *module));
};

/**
 * PinosModuleInitFunc:
 * @module: A #PinosModule
 * @args: Arguments to the module
 *
 * A module should provide an init function with this signature. This function
 * will be called when a module is loaded.
 *
 * Returns: %true on success, %false otherwise
 */
typedef bool (*PinosModuleInitFunc) (PinosModule *module, char *args);

PinosModule *     pinos_module_load              (PinosCore   *core,
                                                  const gchar *name,
                                                  const gchar *args,
                                                  char       **err);
void              pinos_module_destroy           (PinosModule *module);

G_END_DECLS

#endif /* __PINOS_MODULE_H__ */
