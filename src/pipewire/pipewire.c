/* PipeWire
 *
 * Copyright © 2018 Wim Taymans
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <unistd.h>
#include <limits.h>
#include <stdio.h>
#ifndef __FreeBSD__
#include <sys/prctl.h>
#endif
#include <pwd.h>
#include <errno.h>
#include <dlfcn.h>

#include <spa/utils/names.h>
#include <spa/support/cpu.h>

#include "pipewire.h"
#include "private.h"

#define MAX_SUPPORT	32

#define SUPPORTLIB	"support/libspa-support"

struct plugin {
	struct spa_list link;
	char *filename;
	void *hnd;
	spa_handle_factory_enum_func_t enum_func;
	struct spa_list handles;
	int ref;
};

struct handle {
	struct spa_list link;
	struct plugin *plugin;
	char *factory_name;
	int ref;
	struct spa_handle handle;
};

struct registry {
	struct spa_list plugins;
};

struct support {
	char **categories;
	const char *plugin_dir;
	const char *support_lib;
	struct registry *registry;
	struct spa_support support[MAX_SUPPORT];
	uint32_t n_support;
};

static struct registry global_registry;
static struct support global_support;

static struct plugin *
find_plugin(struct registry *registry, const char *filename)
{
	struct plugin *p;
	spa_list_for_each(p, &registry->plugins, link) {
		if (!strcmp(p->filename, filename))
			return p;
	}
	return NULL;
}

static struct plugin *
open_plugin(struct registry *registry,
	    const char *path,
	    const char *lib)
{
	struct plugin *plugin;
	char *filename;
	void *hnd;
	spa_handle_factory_enum_func_t enum_func;
	int res;

        if ((filename = spa_aprintf("%s/%s.so", path, lib)) == NULL) {
		res = -errno;
		goto error_out;
	}

	if ((plugin = find_plugin(registry, filename)) != NULL) {
		free(filename);
		plugin->ref++;
		return plugin;
	}

        if ((hnd = dlopen(filename, RTLD_NOW)) == NULL) {
		res = -ENOENT;
		pw_log_error("can't load %s: %s", filename, dlerror());
		goto error_free_filename;
        }
        if ((enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		res = -ENOSYS;
		pw_log_error("can't find enum function: %s", dlerror());
		goto error_dlclose;
        }

	if ((plugin = calloc(1, sizeof(struct plugin))) == NULL) {
		res = -errno;
		goto error_dlclose;
	}

	pw_log_debug("loaded plugin:'%s'", filename);
	plugin->ref = 1;
	plugin->filename = filename;
	plugin->hnd = hnd;
	plugin->enum_func = enum_func;
	spa_list_init(&plugin->handles);

	spa_list_append(&registry->plugins, &plugin->link);

	return plugin;

error_dlclose:
	dlclose(hnd);
error_free_filename:
        free(filename);
error_out:
	errno = -res;
	return NULL;
}

static void
unref_plugin(struct plugin *plugin)
{
	if (--plugin->ref == 0) {
		spa_list_remove(&plugin->link);
		pw_log_debug("unloaded plugin:'%s'", plugin->filename);
		dlclose(plugin->hnd);
		free(plugin->filename);
		free(plugin);
	}
}

static const struct spa_handle_factory *find_factory(struct plugin *plugin, const char *factory_name)
{
	int res = -ENOENT;
	uint32_t index;
        const struct spa_handle_factory *factory;

        for (index = 0;;) {
                if ((res = plugin->enum_func(&factory, &index)) <= 0) {
                        if (res == 0)
				break;
                        goto out;
                }
                if (strcmp(factory->name, factory_name) == 0)
                        return factory;
	}
	res = -ENOENT;
out:
	pw_log_error("can't find factory %s: %s", factory_name, spa_strerror(res));
	errno = -res;
	return NULL;
}

static void unref_handle(struct handle *handle)
{
	if (--handle->ref == 0) {
		spa_list_remove(&handle->link);
		spa_handle_clear(&handle->handle);
		unref_plugin(handle->plugin);
		free(handle->factory_name);
		free(handle);
	}
}

static void configure_debug(struct support *support, const char *str)
{
	char **level;
	int n_tokens;

	level = pw_split_strv(str, ":", INT_MAX, &n_tokens);
	if (n_tokens > 0)
		pw_log_set_level(atoi(level[0]));

	if (n_tokens > 1)
		support->categories = pw_split_strv(level[1], ",", INT_MAX, &n_tokens);

	if (level)
		pw_free_strv(level);
}

SPA_EXPORT
uint32_t pw_get_support(struct spa_support *support, uint32_t max_support)
{
	uint32_t i, n = SPA_MIN(global_support.n_support, max_support);
	for (i = 0; i < n; i++)
		support[i] = global_support.support[i];
	return n;
}

SPA_EXPORT
struct spa_handle *pw_load_spa_handle(const char *lib,
		const char *factory_name,
		const struct spa_dict *info,
		uint32_t n_support,
		const struct spa_support support[])
{
	struct support *sup = &global_support;
	struct plugin *plugin;
	struct handle *handle;
        const struct spa_handle_factory *factory;
        int res;

	if (factory_name == NULL) {
		res = -EINVAL;
		goto error_out;
	}

	if (lib == NULL)
		lib = sup->support_lib;

	pw_log_debug("load lib:'%s' factory-name:'%s'", lib, factory_name);

	if ((plugin = open_plugin(sup->registry, sup->plugin_dir, lib)) == NULL) {
		res = -errno;
		pw_log_error("can't load '%s': %m", lib);
		goto error_out;
	}

	factory = find_factory(plugin, factory_name);
	if (factory == NULL) {
		res = -errno;
		pw_log_error("can't find factory '%s': %m %s", factory_name, spa_strerror(res));
		goto error_unref_plugin;
	}

	handle = calloc(1, sizeof(struct handle) + spa_handle_factory_get_size(factory, info));
	if (handle == NULL) {
		res = -errno;
		goto error_unref_plugin;
	}

        if ((res = spa_handle_factory_init(factory,
                                           &handle->handle, info,
					   support, n_support)) < 0) {
                pw_log_error("can't make factory instance '%s': %d (%s)",
				factory_name, res, spa_strerror(res));
                goto error_free_handle;
        }

	handle->ref = 1;
	handle->plugin = plugin;
	handle->factory_name = strdup(factory_name);
	spa_list_append(&plugin->handles, &handle->link);

	return &handle->handle;

error_free_handle:
	free(handle);
error_unref_plugin:
	unref_plugin(plugin);
error_out:
	errno = -res;
	return NULL;
}

static struct handle *find_handle(struct spa_handle *handle)
{
	struct registry *registry = global_support.registry;
	struct plugin *p;
	struct handle *h;

	spa_list_for_each(p, &registry->plugins, link) {
		spa_list_for_each(h, &p->handles, link) {
			if (&h->handle == handle)
				return h;
		}
	}
	return NULL;
}

SPA_EXPORT
int pw_unload_spa_handle(struct spa_handle *handle)
{
	struct handle *h;

	if ((h = find_handle(handle)) == NULL)
		return -ENOENT;

	unref_handle(h);

	return 0;
}

static void *add_interface(struct support *support,
		const char *factory_name,
		const char *type,
		const struct spa_dict *info)
{
	struct spa_handle *handle;
	void *iface = NULL;
	int res = -ENOENT;

	handle = pw_load_spa_handle(support->support_lib,
			factory_name, info,
			support->n_support, support->support);

	if (handle == NULL ||
	    (res = spa_handle_get_interface(handle, type, &iface)) < 0) {
			pw_log_error("can't get %s interface %d", type, res);
	} else {
		support->support[support->n_support++] =
			SPA_SUPPORT_INIT(type, iface);
	}
	return iface;
}

/** Initialize PipeWire
 *
 * \param argc pointer to argc
 * \param argv pointer to argv
 *
 * Initialize the PipeWire system, parse and modify any parameters given
 * by \a argc and \a argv and set up debugging.
 *
 * The environment variable \a PIPEWIRE_DEBUG
 *
 * \memberof pw_pipewire
 */
SPA_EXPORT
void pw_init(int *argc, char **argv[])
{
	const char *str;
	struct spa_dict_item items[4];
	uint32_t n_items;
	struct spa_dict info;
	struct support *support = &global_support;
	struct spa_log *log;
	char level[32];

	if (support->registry != NULL)
		return;

	if ((str = getenv("PIPEWIRE_DEBUG")))
		configure_debug(support, str);
	else
		pw_log_set_level(SPA_LOG_LEVEL_WARN);

	if ((str = getenv("SPA_PLUGIN_DIR")) == NULL)
		str = PLUGINDIR;
	support->plugin_dir = str;

	if ((str = getenv("SPA_SUPPORT_LIB")) == NULL)
		str = SUPPORTLIB;
	support->support_lib = str;

	spa_list_init(&global_registry.plugins);
	support->registry = &global_registry;

	n_items = 0;
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_COLORS, "true");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_TIMESTAMP, "true");
	snprintf(level, sizeof(level), "%d", pw_log_level);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_LEVEL, level);
	if ((str = getenv("PIPEWIRE_LOG")) != NULL)
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_FILE, str);
	info = SPA_DICT_INIT(items, n_items);

	log = add_interface(support, SPA_NAME_SUPPORT_LOG, SPA_TYPE_INTERFACE_Log, &info);
	if (log)
		pw_log_set(log);

	n_items = 0;
	if ((str = getenv("PIPEWIRE_CPU")))
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_CPU_FORCE, str);
	info = SPA_DICT_INIT(items, n_items);

	add_interface(support, SPA_NAME_SUPPORT_CPU, SPA_TYPE_INTERFACE_CPU, &info);
	pw_log_info("version %s", pw_get_library_version());
}

/** Check if a debug category is enabled
 *
 * \param name the name of the category to check
 * \return true if enabled
 *
 * Debugging categories can be enabled by using the PIPEWIRE_DEBUG
 * environment variable
 *
 * \memberof pw_pipewire
 */
SPA_EXPORT
bool pw_debug_is_category_enabled(const char *name)
{
	int i;

	if (global_support.categories == NULL)
		return false;

	for (i = 0; global_support.categories[i]; i++) {
		if (strcmp(global_support.categories[i], name) == 0)
			return true;
	}
	return false;
}

/** Get the application name \memberof pw_pipewire */
SPA_EXPORT
const char *pw_get_application_name(void)
{
	errno = ENOTSUP;
	return NULL;
}

/** Get the program name \memberof pw_pipewire */
SPA_EXPORT
const char *pw_get_prgname(void)
{
	static char tcomm[16 + 1];
	spa_zero(tcomm);

#ifndef __FreeBSD__
	if (prctl(PR_GET_NAME, (unsigned long) tcomm, 0, 0, 0) == 0)
		return tcomm;
#endif

	return NULL;
}

/** Get the user name \memberof pw_pipewire */
SPA_EXPORT
const char *pw_get_user_name(void)
{
	struct passwd *pw;

	if ((pw = getpwuid(getuid())))
		return pw->pw_name;

	return NULL;
}

/** Get the host name \memberof pw_pipewire */
SPA_EXPORT
const char *pw_get_host_name(void)
{
	static char hname[256];

	if (gethostname(hname, 256) < 0)
		return NULL;

	hname[255] = 0;
	return hname;
}

/** Get the client name
 *
 * Make a new PipeWire client name that can be used to construct a remote.
 *
 * \memberof pw_pipewire
 */
SPA_EXPORT
const char *pw_get_client_name(void)
{
	const char *cc;
	static char cname[256];

	if ((cc = pw_get_application_name()))
		return cc;
	else if ((cc = pw_get_prgname()))
		return cc;
	else {
		if (snprintf(cname, sizeof(cname), "pipewire-pid-%zd", (size_t) getpid()) < 0)
			return NULL;
		cname[255] = 0;
		return cname;
	}
}

/** Reverse the direction \memberof pw_pipewire */
SPA_EXPORT
enum pw_direction pw_direction_reverse(enum pw_direction direction)
{
	if (direction == PW_DIRECTION_INPUT)
		return PW_DIRECTION_OUTPUT;
	else if (direction == PW_DIRECTION_OUTPUT)
		return PW_DIRECTION_INPUT;
	return direction;
}

/** Get the currently running version */
SPA_EXPORT
const char* pw_get_library_version(void)
{
	return pw_get_headers_version();
}

static const struct spa_type_info type_info[] = {
	{ SPA_ID_INVALID, SPA_ID_INVALID, "spa_types", spa_types },
	{ 0, 0, NULL, NULL },
};

SPA_EXPORT
const struct spa_type_info * pw_type_info(void)
{
	return type_info;
}
