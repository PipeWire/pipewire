/* PipeWire
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

#include <stdio.h>
#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"
#include "pipewire/interfaces.h"
#include "pipewire/utils.h"
#include "pipewire/module.h"

/** \cond */
struct impl {
	struct pw_module this;
	void *hnd;
};

struct resource_data {
	struct spa_hook resource_listener;
};


/** \endcond */

static char *find_module(const char *path, const char *name)
{
	char *filename;
	struct dirent *entry;
	struct stat s;
	DIR *dir;

	asprintf(&filename, "%s/%s.so", path, name);

	if (stat(filename, &s) == 0 && S_ISREG(s.st_mode)) {
		/* found a regular file with name */
		return filename;
	}

	free(filename);
	filename = NULL;

	/* now recurse down in subdirectories and look for it there */

	dir = opendir(path);
	if (dir == NULL) {
		pw_log_warn("could not open %s: %s", path, strerror(errno));
		return NULL;
	}

	while ((entry = readdir(dir))) {
		char *newpath;

		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
			continue;

		asprintf(&newpath, "%s/%s", path, entry->d_name);

		if (stat(newpath, &s) == 0 && S_ISDIR(s.st_mode)) {
			filename = find_module(newpath, name);
		}
		free(newpath);

		if (filename != NULL)
			break;
	}

	closedir(dir);

	return filename;
}

static void module_unbind_func(void *data)
{
	struct pw_resource *resource = data;
	spa_list_remove(&resource->link);
}

static const struct pw_resource_events resource_events = {
	PW_VERSION_RESOURCE_EVENTS,
	.destroy = module_unbind_func,
};

static int
module_bind_func(struct pw_global *global,
		 struct pw_client *client, uint32_t permissions,
		 uint32_t version, uint32_t id)
{
	struct pw_module *this = global->object;
	struct pw_resource *resource;
	struct resource_data *data;

	resource = pw_resource_new(client, id, permissions, global->type, version, sizeof(*data));
	if (resource == NULL)
		goto no_mem;

	data = pw_resource_get_user_data(resource);
	pw_resource_add_listener(resource, &data->resource_listener, &resource_events, resource);

	pw_log_debug("module %p: bound to %d", this, resource->id);

	spa_list_append(&this->resource_list, &resource->link);

	this->info.change_mask = ~0;
	pw_module_resource_info(resource, &this->info);
	this->info.change_mask = 0;

	return 0;

      no_mem:
	pw_log_error("can't create module resource");
	pw_core_resource_error(client->core_resource,
			       client->core_resource->id, -ENOMEM, "no memory");
	return -ENOMEM;
}

struct pw_module * pw_core_find_module(struct pw_core *core, const char *filename)
{
	struct pw_module *module;
	spa_list_for_each(module, &core->module_list, link) {
                if (strcmp(module->info.filename, filename) == 0)
                        return module;
        }
	return NULL;
}

/** Load a module
 *
 * \param core a \ref pw_core
 * \param name name of the module to load
 * \param args A string with arguments for the module
 * \param[out] error Return location for an error string, or NULL
 * \return A \ref pw_module if the module could be loaded, or NULL on failure.
 *
 * \memberof pw_module
 */
struct pw_module *pw_module_load(struct pw_core *core, const char *name, const char *args)
{
	struct pw_module *this;
	struct impl *impl;
	void *hnd;
	char *filename = NULL;
	const char *module_dir;
	int res;
	pw_module_init_func_t init_func;

	module_dir = getenv("PIPEWIRE_MODULE_DIR");
	if (module_dir != NULL) {
		char **l;
		int i, n_paths;

		pw_log_debug("PIPEWIRE_MODULE_DIR set to: %s", module_dir);

		l = pw_split_strv(module_dir, "/", 0, &n_paths);
		for (i = 0; l[i] != NULL; i++) {
			filename = find_module(l[i], name);
			if (filename != NULL)
				break;
		}
		pw_free_strv(l);
	} else {
		pw_log_debug("moduledir set to: %s", MODULEDIR);

		filename = find_module(MODULEDIR, name);
	}

	if (filename == NULL)
		goto not_found;

	pw_log_debug("trying to load module: %s (%s)", name, filename);

	hnd = dlopen(filename, RTLD_NOW | RTLD_LOCAL);

	if (hnd == NULL)
		goto open_failed;

	if ((init_func = dlsym(hnd, PIPEWIRE_SYMBOL_MODULE_INIT)) == NULL)
		goto no_pw_module;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto no_mem;

	impl->hnd = hnd;

	this = &impl->this;
	this->core = core;

	spa_list_init(&this->resource_list);
	spa_hook_list_init(&this->listener_list);

	this->info.name = name ? strdup(name) : NULL;
	this->info.filename = filename;
	this->info.args = args ? strdup(args) : NULL;
	this->info.props = NULL;

	spa_list_append(&core->module_list, &this->link);
	this->global = pw_global_new(core, core->type.module, PW_VERSION_MODULE,
					  module_bind_func, this);

	if (this->global != NULL) {
		pw_global_register(this->global, NULL, core->global);
		this->info.id = this->global->id;
	}

	if ((res = init_func(this, args)) < 0)
		goto init_failed;

	pw_log_debug("loaded module: %s", this->info.name);

	return this;

      not_found:
	pw_log_error("No module \"%s\" was found", name);
	return NULL;
      open_failed:
	pw_log_error("Failed to open module: \"%s\" %s", filename, dlerror());
	free(filename);
	return NULL;
      no_mem:
      no_pw_module:
	pw_log_error("\"%s\" is not a pipewire module", filename);
	dlclose(hnd);
	free(filename);
	return NULL;
      init_failed:
	pw_log_error("\"%s\" failed to initialize: %s", filename, spa_strerror(res));
	pw_module_destroy(this);
	return NULL;
}

/** Destroy a module
 * \param module the module to destroy
 * \memberof pw_module
 */
void pw_module_destroy(struct pw_module *module)
{
	struct impl *impl = SPA_CONTAINER_OF(module, struct impl, this);
	struct pw_resource *resource, *tmp;

	pw_log_debug("module %p: destroy", module);
	spa_hook_list_call(&module->listener_list, struct pw_module_events, destroy);

	spa_list_for_each_safe(resource, tmp, &module->resource_list, link)
		pw_resource_destroy(resource);

	if (module->info.name)
		free((char *) module->info.name);
	if (module->info.filename)
		free((char *) module->info.filename);
	if (module->info.args)
		free((char *) module->info.args);

	spa_list_remove(&module->link);
	if (module->global)
		pw_global_destroy(module->global);
	dlclose(impl->hnd);
	free(impl);
}

struct pw_core *
pw_module_get_core(struct pw_module *module)
{
	return module->core;
}

struct pw_global * pw_module_get_global(struct pw_module *module)
{
	return module->global;
}

const struct pw_module_info *
pw_module_get_info(struct pw_module *module)
{
	return &module->info;
}

void pw_module_add_listener(struct pw_module *module,
			    struct spa_hook *listener,
			    const struct pw_module_events *events,
			    void *data)
{
	spa_hook_list_append(&module->listener_list, listener, events, data);
}
