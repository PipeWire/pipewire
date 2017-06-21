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

#include <dlfcn.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>

#include "pipewire/client/pipewire.h"
#include "pipewire/client/interfaces.h"
#include "pipewire/client/utils.h"
#include "pipewire/client/extension.h"

/** \cond */
struct impl {
	struct pw_extension this;
	void *hnd;
};
/** \endcond */

/** Load an extension \memberof pw_extension
 *
 * \param context a \ref pw_context
 * \param name name of the extension to load
 * \param args A string with arguments for the extension
 * \param[out] err Return location for an error string, or NULL
 * \return A \ref pw_extension if the extension could be loaded, or NULL on failure.
 */
struct pw_extension *pw_extension_load(struct pw_context *context,
				       const char *name, const char *args)
{
	struct pw_extension *this;
	struct impl *impl;
	void *hnd;
	char *filename = NULL;
	const char *module_dir;
	pw_extension_init_func_t init_func;

	module_dir = getenv("PIPEWIRE_MODULE_DIR");
	if (module_dir == NULL)
		module_dir = MODULEDIR;

	pw_log_debug("PIPEWIRE_MODULE_DIR set to: %s", module_dir);

	asprintf(&filename, "%s/%s.so", module_dir, name);
	if (filename == NULL)
		goto no_filename;

	pw_log_debug("trying to load extension: %s (%s)", name, filename);

	hnd = dlopen(filename, RTLD_NOW | RTLD_LOCAL);

	if (hnd == NULL)
		goto open_failed;

	if ((init_func = dlsym(hnd, PIPEWIRE_SYMBOL_EXTENSION_INIT)) == NULL)
		goto no_pw_extension;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto no_mem;

	impl->hnd = hnd;

	this = &impl->this;
	this->context = context;
	this->filename = filename;
	this->args = args ? strdup(args) : NULL;
	this->props = NULL;

	pw_signal_init(&this->destroy_signal);

	if (!init_func(this, (char *) args))
		goto init_failed;

	spa_list_insert(&context->extension_list, &this->link);

	pw_log_debug("loaded extension: %s", filename);

	return this;

      no_filename:
	pw_log_error("No memory");
	return NULL;
      open_failed:
	pw_log_error("Failed to open module: \"%s\" %s", filename, dlerror());
	free(filename);
	return NULL;
      no_mem:
      no_pw_extension:
	pw_log_error("\"%s\" is not a pipewire extension", name);
	dlclose(hnd);
	free(filename);
	return NULL;
      init_failed:
	pw_log_error("\"%s\" failed to initialize", name);
	pw_extension_destroy(this);
	return NULL;
}

/** Destroy a extension
 * \param extension the extension to destroy
 * \memberof pw_extension
 */
void pw_extension_destroy(struct pw_extension *extension)
{
	struct impl *impl = SPA_CONTAINER_OF(extension, struct impl, this);

	pw_signal_emit(&extension->destroy_signal, extension);

	if (extension->filename)
		free((char *) extension->filename);
	if (extension->args)
		free((char *) extension->args);
	if (extension->props)
		pw_properties_free(extension->props);
	dlclose(impl->hnd);
	free(impl);
}
