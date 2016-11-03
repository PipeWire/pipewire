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

#define PINOS_MODULE_GET_PRIVATE(module)   \
  (G_TYPE_INSTANCE_GET_PRIVATE ((module), PINOS_TYPE_MODULE, PinosModulePrivate))

struct _PinosModulePrivate
{
  GModule *module;
};

G_DEFINE_TYPE (PinosModule, pinos_module, G_TYPE_OBJECT);

enum
{
  PROP_0,
  PROP_DAEMON,
  PROP_NAME,
};

static void
pinos_module_finalize (GObject * object)
{
  PinosModule *module = PINOS_MODULE (object);
  PinosModulePrivate *priv = module->priv;

  g_clear_object (&module->daemon);
  g_free (module->name);
  g_module_close (priv->module);

  G_OBJECT_CLASS (pinos_module_parent_class)->finalize (object);
}

static void
pinos_module_set_property (GObject      *object,
                           guint         prop_id,
                           const GValue *value,
                           GParamSpec   *pspec)
{
  PinosModule *module = PINOS_MODULE (object);

  switch (prop_id) {
    case PROP_DAEMON:
      module->daemon = g_value_dup_object (value);
      break;
    case PROP_NAME:
      module->name = g_value_dup_string (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
pinos_module_get_property (GObject    *object,
                           guint       prop_id,
                           GValue     *value,
                           GParamSpec *pspec)
{
  PinosModule *module = PINOS_MODULE (object);

  switch (prop_id) {
    case PROP_DAEMON:
      g_value_set_object (value, module->daemon);
      break;
    case PROP_NAME:
      g_value_set_string (value, module->name);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
pinos_module_class_init (PinosModuleClass * klass)
{
  GObjectClass *gobject_class = G_OBJECT_CLASS (klass);

  g_type_class_add_private (klass, sizeof (PinosModulePrivate));

  gobject_class->finalize = pinos_module_finalize;
  gobject_class->set_property = pinos_module_set_property;
  gobject_class->get_property = pinos_module_get_property;

  g_object_class_install_property (gobject_class,
                                   PROP_DAEMON,
                                   g_param_spec_object ("daemon",
                                                        "Daemon",
                                                        "A Pinos Daemon",
                                                        PINOS_TYPE_DAEMON,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (gobject_class,
                                   PROP_NAME,
                                   g_param_spec_string ("name",
                                                        "Name",
                                                        "The name of the plugin",
                                                        NULL,
                                                        G_PARAM_READWRITE |
                                                        G_PARAM_CONSTRUCT_ONLY |
                                                        G_PARAM_STATIC_STRINGS));
}

static void
pinos_module_init (PinosModule * module)
{
  PinosModulePrivate *priv = PINOS_MODULE_GET_PRIVATE (module);
  module->priv = priv;
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

static PinosModule *
pinos_module_new (const gchar * name, PinosDaemon * daemon)
{
  return g_object_new (PINOS_TYPE_MODULE, "name", name, "daemon", daemon, NULL);
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
 * @daemon: a #PinosDaemon
 * @name: name of the module to load
 * @args: A string with arguments for the module
 * @err: Return location for a #GError, or %NULL
 *
 * Load module with @name.
 *
 * Returns: A #PinosModule if the module could be loaded, or %NULL on failure.
 */
PinosModule *
pinos_module_load (PinosDaemon  * daemon,
                   const gchar  * name,
                   const gchar  * args,
                   GError      ** err)
{
  PinosModule *module;
  PinosModulePrivate *priv;
  GModule *gmodule;
  gchar *filename = NULL;
  const gchar *module_dir;
  PinosModuleInitFunc init_func;

  g_return_val_if_fail (name != NULL && name[0] != '\0', NULL);
  g_return_val_if_fail (PINOS_IS_DAEMON (daemon), NULL);

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

  module = pinos_module_new (name, daemon);
  priv = module->priv;
  priv->module = gmodule;

  /* don't unload this module again */
  g_module_make_resident (gmodule);

  if (!init_func (module, (gchar *) args)) {
    g_set_error (err, PINOS_MODULE_ERROR, PINOS_MODULE_ERROR_INIT,
        "\"%s\" failed to initialize", name);
    g_object_unref (module);
    return NULL;
  }

  pinos_log_debug ("loaded module: %s", module->name);

  return module;
}
