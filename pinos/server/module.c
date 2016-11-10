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

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <glib.h>
#include <gmodule.h>

#include "pinos/client/pinos.h"
#include "pinos/server/module.h"

#define PINOS_SYMBOL_MODULE_INIT "pinos__module_init"

typedef struct
{
  PinosModule this;

  PinosObject object;
  PinosInterface ifaces[1];

  GModule *module;
} PinosModuleImpl;


static void
module_destroy (PinosObject * object)
{
  PinosModuleImpl *impl = SPA_CONTAINER_OF (object, PinosModuleImpl, object);
  PinosModule *this = &impl->this;

  free (this->name);
  g_module_close (impl->module);
  free (impl);
}

GQuark
pinos_module_error_quark (void)
{
  static GQuark quark = 0;

  if (quark == 0) {
    quark = g_quark_from_static_string ("pinos_module_error");
  }

  return quark;
}

static gchar *
find_module (const gchar * path, const gchar *name)
{
  gchar *filename;
  GDir *dir;
  const gchar *entry;
  GError *err = NULL;

  filename = g_strconcat (path, G_DIR_SEPARATOR_S, name, ".", G_MODULE_SUFFIX, NULL);

  if (g_file_test (filename, G_FILE_TEST_IS_REGULAR)) {
    /* found a regular file with name */
    return filename;
  }

  g_clear_pointer (&filename, g_free);

  /* now recurse down in subdirectories and look for it there */

  dir = g_dir_open (path, 0, &err);
  if (dir == NULL) {
    pinos_log_warn ("could not open %s: %s", path, err->message);
    g_error_free (err);
    return NULL;
  }

  while ((entry = g_dir_read_name (dir))) {
    gchar *newpath;

    newpath = g_build_filename (path, entry, NULL);
    if (g_file_test (newpath, G_FILE_TEST_IS_DIR)) {
      filename = find_module (newpath, name);
    }
    g_free (newpath);

    if (filename != NULL)
      break;
  }

  g_dir_close (dir);

  return filename;
}

/**
 * pinos_module_load:
 * @core: a #PinosCore
 * @name: name of the module to load
 * @args: A string with arguments for the module
 * @err: Return location for a #GError, or %NULL
 *
 * Load module with @name.
 *
 * Returns: A #PinosModule if the module could be loaded, or %NULL on failure.
 */
PinosModule *
pinos_module_load (PinosCore    *core,
                   const gchar  *name,
                   const gchar  *args,
                   GError      **err)
{
  PinosModule *this;
  PinosModuleImpl *impl;
  GModule *gmodule;
  gchar *filename = NULL;
  const gchar *module_dir;
  PinosModuleInitFunc init_func;

  g_return_val_if_fail (name != NULL && name[0] != '\0', NULL);
  g_return_val_if_fail (core, NULL);

  if (!g_module_supported ()) {
    g_set_error (err, PINOS_MODULE_ERROR, PINOS_MODULE_ERROR_LOADING,
        "Dynamic module loading not supported");
    return NULL;
  }

  module_dir = g_getenv ("PINOS_MODULE_DIR");
  if (module_dir != NULL) {
    gchar **l;
    gint i;

    pinos_log_debug ("PINOS_MODULE_DIR set to: %s", module_dir);

    l = g_strsplit (module_dir, G_SEARCHPATH_SEPARATOR_S, 0);
    for (i = 0; l[i] != NULL; i++) {
      filename = find_module (l[i], name);
      if (filename != NULL)
        break;
    }
    g_strfreev (l);
  } else {
    pinos_log_debug ("moduledir set to: %s", MODULEDIR);

    filename = find_module (MODULEDIR, name);
  }

  if (filename == NULL) {
    g_set_error (err, PINOS_MODULE_ERROR, PINOS_MODULE_ERROR_NOT_FOUND,
        "No module \"%s\" was found", name);
    return NULL;
  }

  pinos_log_debug ("trying to load module: %s (%s)", name, filename);

  gmodule = g_module_open (filename, G_MODULE_BIND_LOCAL);
  g_free (filename);

  if (gmodule == NULL) {
    g_set_error (err, PINOS_MODULE_ERROR, PINOS_MODULE_ERROR_LOADING,
        "Failed to open module: %s", g_module_error ());
    return NULL;
  }

  if (!g_module_symbol (gmodule, PINOS_SYMBOL_MODULE_INIT,
          (gpointer *) &init_func)) {
    g_set_error (err, PINOS_MODULE_ERROR, PINOS_MODULE_ERROR_LOADING,
        "\"%s\" is not a pinos module", name);
    g_module_close (gmodule);
    return NULL;
  }

  impl = calloc (1, sizeof (PinosModuleImpl));
  impl->module = gmodule;

  this = &impl->this;
  this->name = strdup (name);
  this->core = core;

  impl->ifaces[0].type = impl->core->registry.uri.module;
  impl->ifaces[0].iface = this;


  pinos_object_init (&impl->object,
                     module_destroy,
                     1,
                     impl->ifaces);

  /* don't unload this module again */
  g_module_make_resident (gmodule);

  if (!init_func (this, (gchar *) args)) {
    g_set_error (err, PINOS_MODULE_ERROR, PINOS_MODULE_ERROR_INIT,
        "\"%s\" failed to initialize", name);
    module_destroy (this);
    return NULL;
  }

  pinos_log_debug ("loaded module: %s", this->name);

  return &this->object;
}
