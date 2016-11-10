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

#include <glib-object.h>

G_BEGIN_DECLS

#include <pinos/server/core.h>

#define PINOS_MODULE_URI                            "http://pinos.org/ns/module"
#define PINOS_MODULE_PREFIX                         PINOS_MODULE_URI "#"

typedef struct _PinosModule PinosModule;

struct _PinosModule {
  gchar *name;
  PinosCore *core;
};

GQuark pinos_module_error_quark (void);
/**
 * PINOS_MODULE_ERROR:
 *
 * Pinos module error.
 */
#define PINOS_MODULE_ERROR pinos_module_error_quark ()

/**
 * PinosModuleError:
 * @PINOS_MODULE_ERROR_GENERIC: Generic module error.
 * @PINOS_MODULE_ERROR_NOT_FOUND: Module could not be found.
 * @PINOS_MODULE_ERROR_LOADING: Module could not be loaded.
 * @PINOS_MODULE_ERROR_INIT: The module failed to initialize.
 *
 * Error codes for Pinos modules.
 */
typedef enum
{
  PINOS_MODULE_ERROR_GENERIC,
  PINOS_MODULE_ERROR_NOT_FOUND,
  PINOS_MODULE_ERROR_LOADING,
  PINOS_MODULE_ERROR_INIT,
} PinosModuleError;

/**
 * PinosModuleInitFunc:
 * @module: A #PinosModule
 * @args: Arguments to the module
 *
 * A module should provide an init function with this signature. This function
 * will be called when a module is loaded.
 *
 * Returns: %TRUE on success, %FALSE otherwise
 */
typedef gboolean (*PinosModuleInitFunc) (PinosModule *module, gchar *args);

PinosObject *     pinos_module_load              (PinosCore   *core,
                                                  const gchar *name,
                                                  const gchar *args,
                                                  GError **err);

G_END_DECLS

#endif /* __PINOS_MODULE_H__ */
