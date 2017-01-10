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

#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>


#include "pinos/client/pinos.h"
#include "pinos/client/utils.h"
#include "pinos/server/module.h"

#define PINOS_SYMBOL_MODULE_INIT "pinos__module_init"

typedef struct
{
  PinosModule this;

  void *hnd;
} PinosModuleImpl;

static char *
find_module (const char * path, const char *name)
{
  char *filename;
  struct dirent *entry;
  struct stat s;
  DIR *dir;

  asprintf (&filename, "%s/%s.so", path, name);

  if (stat (filename, &s) == 0 && S_ISREG (s.st_mode)) {
    /* found a regular file with name */
    return filename;
  }

  free (filename);
  filename = NULL;

  /* now recurse down in subdirectories and look for it there */

  dir = opendir (path);
  if (dir == NULL) {
    pinos_log_warn ("could not open %s: %s", path, strerror (errno));
    return NULL;
  }

  while ((entry = readdir (dir))) {
    char *newpath;

    if (strcmp (entry->d_name, ".") == 0 ||
        strcmp (entry->d_name, "..") == 0)
      continue;

    asprintf (&newpath, "%s/%s", path, entry->d_name);

    if (stat (newpath, &s) == 0 && S_ISDIR (s.st_mode)) {
      filename = find_module (newpath, name);
    }
    free (newpath);

    if (filename != NULL)
      break;
  }

  closedir (dir);

  return filename;
}

static SpaResult
module_dispatch_func (void             *object,
                      PinosMessageType  type,
                      void             *message,
                      void             *data)
{
  PinosResource *resource = object;
  PinosModule *module = resource->object;

  switch (type) {
    default:
      pinos_log_warn ("module %p: unhandled message %d", module, type);
      break;
  }
  return SPA_RESULT_OK;
}

static SpaResult
module_bind_func (PinosGlobal *global,
                  PinosClient *client,
                  uint32_t     version,
                  uint32_t     id)
{
  PinosModule *this = global->object;
  PinosResource *resource;
  PinosMessageModuleInfo m;
  PinosModuleInfo info;

  resource = pinos_resource_new (client,
                                 id,
                                 global->type,
                                 global->object,
                                 NULL);
  if (resource == NULL)
    goto no_mem;

  pinos_resource_set_dispatch (resource,
                               module_dispatch_func,
                               global);

  pinos_log_debug ("module %p: bound to %d", global->object, resource->id);

  m.info = &info;
  info.id = global->id;
  info.change_mask = ~0;
  info.name = this->name;
  info.filename = this->filename;
  info.args = this->args;
  info.props = NULL;

  return pinos_resource_send_message (resource,
                                      PINOS_MESSAGE_MODULE_INFO,
                                      &m,
                                      true);
no_mem:
  pinos_resource_send_error (resource,
                             SPA_RESULT_NO_MEMORY,
                             "no memory");
  return SPA_RESULT_NO_MEMORY;
}

/**
 * pinos_module_load:
 * @core: a #PinosCore
 * @name: name of the module to load
 * @args: A string with arguments for the module
 * @err: Return location for an error string, or %NULL
 *
 * Load module with @name.
 *
 * Returns: A #PinosModule if the module could be loaded, or %NULL on failure.
 */
PinosModule *
pinos_module_load (PinosCore    *core,
                   const char   *name,
                   const char   *args,
                   char        **err)
{
  PinosModule *this;
  PinosModuleImpl *impl;
  void *hnd;
  char *filename = NULL;
  const char *module_dir;
  PinosModuleInitFunc init_func;

  module_dir = getenv ("PINOS_MODULE_DIR");
  if (module_dir != NULL) {
    char **l;
    int i, n_paths;

    pinos_log_debug ("PINOS_MODULE_DIR set to: %s", module_dir);

    l = pinos_split_strv (module_dir, "/", 0, &n_paths);
    for (i = 0; l[i] != NULL; i++) {
      filename = find_module (l[i], name);
      if (filename != NULL)
        break;
    }
    pinos_free_strv (l);
  } else {
    pinos_log_debug ("moduledir set to: %s", MODULEDIR);

    filename = find_module (MODULEDIR, name);
  }

  if (filename == NULL)
    goto not_found;

  pinos_log_debug ("trying to load module: %s (%s)", name, filename);

  hnd = dlopen (filename, RTLD_NOW | RTLD_LOCAL);

  if (hnd == NULL)
    goto open_failed;

  if ((init_func = dlsym (hnd, PINOS_SYMBOL_MODULE_INIT)) == NULL)
    goto no_pinos_module;

  impl = calloc (1, sizeof (PinosModuleImpl));
  if (impl == NULL)
    goto no_mem;

  impl->hnd = hnd;

  this = &impl->this;
  this->name = name ? strdup (name) : NULL;
  this->filename = filename;
  this->args = args ? strdup (args) : NULL;
  this->core = core;

  if (!init_func (this, (char *) args))
    goto init_failed;

  pinos_log_debug ("loaded module: %s", this->name);

  this->global = pinos_core_add_global (core,
                                        NULL,
                                        core->uri.module,
                                        0,
                                        impl,
                                        module_bind_func);

  return this;

not_found:
  {
    asprintf (err, "No module \"%s\" was found", name);
    return NULL;
  }
open_failed:
  {
    asprintf (err, "Failed to open module: \"%s\" %s", filename, dlerror ());
    free (filename);
    return NULL;
  }
no_mem:
no_pinos_module:
  {
    asprintf (err, "\"%s\" is not a pinos module", name);
    dlclose (hnd);
    free (filename);
    return NULL;
  }
init_failed:
  {
    asprintf (err, "\"%s\" failed to initialize", name);
    pinos_module_destroy (this);
    return NULL;
  }
}

void
pinos_module_destroy (PinosModule *this)
{
  PinosModuleImpl *impl = SPA_CONTAINER_OF (this, PinosModuleImpl, this);

  pinos_signal_emit (&this->destroy_signal, this);

  if (this->name)
    free (this->name);
  if (this->filename)
    free (this->filename);
  if (this->args)
    free (this->args);
  dlclose (impl->hnd);
  free (impl);
}
