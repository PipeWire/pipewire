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
#include <sys/prctl.h>
#include <pwd.h>
#include <errno.h>
#include <dlfcn.h>

#include <spa/support/cpu.h>

#include "pipewire.h"
#include "private.h"

#define MAX_SUPPORT	32

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
	const char *factory_name;
	int ref;
	struct spa_handle handle;
};

struct registry {
	struct spa_list plugins;
};

struct support {
	char **categories;
	const char *plugin_dir;
	struct plugin *support_plugin;
	struct spa_support support[MAX_SUPPORT];
	struct registry *registry;
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

        if (asprintf(&filename, "%s/%s.so", path, lib) < 0)
		goto no_filename;

	if ((plugin = find_plugin(registry, filename)) != NULL) {
		free(filename);
		plugin->ref++;
		return plugin;
	}

        if ((hnd = dlopen(filename, RTLD_NOW)) == NULL) {
                fprintf(stderr, "can't load %s: %s\n", filename, dlerror());
                goto open_failed;
        }
        if ((enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
                fprintf(stderr, "can't find enum function\n");
                goto no_symbol;
        }

	if ((plugin = calloc(1, sizeof(struct plugin))) == NULL)
		goto alloc_failed;

	plugin->ref = 1;
	plugin->filename = filename;
	plugin->hnd = hnd;
	plugin->enum_func = enum_func;
	spa_list_init(&plugin->handles);

	spa_list_append(&registry->plugins, &plugin->link);

	return plugin;

      alloc_failed:
      no_symbol:
	dlclose(hnd);
      open_failed:
        free(filename);
      no_filename:
	return NULL;
}

static void
unref_plugin(struct plugin *plugin)
{
	if (--plugin->ref == 0) {
		spa_list_remove(&plugin->link);
		dlclose(plugin->hnd);
		free(plugin->filename);
		free(plugin);
	}
}

static const struct spa_handle_factory *find_factory(struct plugin *plugin, const char *factory_name)
{
	int res;
	uint32_t index;
        const struct spa_handle_factory *factory;

        for (index = 0;;) {
                if ((res = plugin->enum_func(&factory, &index)) <= 0) {
                        if (res != 0)
                                fprintf(stderr, "can't enumerate factories: %s\n", spa_strerror(res));
                        break;
                }
                if (strcmp(factory->name, factory_name) == 0)
                        return factory;
	}
	return NULL;
}

static struct handle *
load_handle(struct plugin *plugin,
	    const char *factory_name,
	    const struct spa_dict *info,
	    uint32_t n_support,
	    struct spa_support support[n_support])
{
        int res;
        struct handle *handle;
        const struct spa_handle_factory *factory;

	factory = find_factory(plugin, factory_name);
	if (factory == NULL)
		goto not_found;

	handle = calloc(1, sizeof(struct handle) + spa_handle_factory_get_size(factory, info));
	if (handle == NULL)
		goto alloc_failed;

        if ((res = spa_handle_factory_init(factory,
                                           &handle->handle, info,
					   support, n_support)) < 0) {
                fprintf(stderr, "can't make factory instance %s: %d\n", factory_name, res);
                goto init_failed;
        }

	handle->ref = 1;
	handle->plugin = plugin;
	handle->factory_name = factory_name;
	spa_list_append(&plugin->handles, &handle->link);

	return handle;

      init_failed:
	free(handle);
      alloc_failed:
      not_found:
	return NULL;
}

static void unref_handle(struct handle *handle)
{
	if (--handle->ref == 0) {
		spa_list_remove(&handle->link);
		spa_handle_clear(&handle->handle);
		unref_plugin(handle->plugin);
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

/** Get a support interface
 * \param type the interface type
 * \return the interface or NULL when not configured
 */
SPA_EXPORT
void *pw_get_support_interface(uint32_t type)
{
	return spa_support_find(global_support.support, global_support.n_support, type);
}

SPA_EXPORT
const struct spa_handle_factory *pw_get_support_factory(const char *factory_name)
{
	struct plugin *plugin = global_support.support_plugin;
	if (plugin == NULL)
		return NULL;
	return find_factory(plugin, factory_name);
}

SPA_EXPORT
const struct spa_support *pw_get_support(uint32_t *n_support)
{
	*n_support = global_support.n_support;
	return global_support.support;
}

SPA_EXPORT
struct spa_handle *pw_load_spa_handle(const char *lib,
		const char *factory_name,
		const struct spa_dict *info,
		uint32_t n_support,
		const struct spa_support support[])
{
	struct support *sup = &global_support;
	struct spa_support extra_support[MAX_SUPPORT];
	uint32_t extra_n_support;
	struct plugin *plugin;
	struct handle *handle;
	uint32_t i;

	extra_n_support = sup->n_support;
	memcpy(extra_support, sup->support,
			sizeof(struct spa_support) * sup->n_support);

	for (i = 0; i < n_support; i++) {
		extra_support[extra_n_support++] =
			SPA_SUPPORT_INIT(support[i].type, support[i].data);
	}
	pw_log_debug("load \"%s\", \"%s\"", lib, factory_name);
	if ((plugin = open_plugin(sup->registry, sup->plugin_dir, lib)) == NULL) {
		pw_log_warn("can't load '%s'", lib);
		return NULL;
	}

	handle = load_handle(plugin, factory_name, info, extra_n_support, extra_support);
	if (handle == NULL)
		return NULL;

	return &handle->handle;
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
	struct handle *handle;
	void *iface;
	struct support *support = &global_support;
	struct plugin *plugin;
	struct spa_dict info;
	struct spa_dict_item items[1];
	int res = 0;

	if ((str = getenv("PIPEWIRE_DEBUG")))
		configure_debug(support, str);

	if ((str = getenv("SPA_PLUGIN_DIR")) == NULL)
		str = PLUGINDIR;

	support->plugin_dir = str;
	spa_list_init(&global_registry.plugins);
	support->registry = &global_registry;

	if (support->n_support > 0)
		return;

	plugin = open_plugin(support->registry, support->plugin_dir, "support/libspa-support");
	if (plugin == NULL) {
		fprintf(stderr, "can't open support library");
		return;
	}

	support->support_plugin = plugin;

	items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_COLORS, "1");
	info = SPA_DICT_INIT(items, 1);

	handle = load_handle(plugin, "logger", &info, support->n_support, support->support);
	if (handle == NULL ||
	    (res = spa_handle_get_interface(&handle->handle,
					    SPA_TYPE_INTERFACE_Log, &iface)) < 0) {
			fprintf(stderr, "can't get Log interface %d\n", res);
	}
	else {
		support->support[support->n_support++] =
			SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Log, iface);
		pw_log_set(iface);
	}

	handle = load_handle(plugin, "cpu", &info, support->n_support, support->support);
	if (handle == NULL ||
	    (res = spa_handle_get_interface(&handle->handle,
					    SPA_TYPE_INTERFACE_CPU, &iface)) < 0) {
		fprintf(stderr, "can't get CPU interface %d\n", res);
	}
	else {
		struct spa_cpu *cpu = iface;
		if ((str = getenv("PIPEWIRE_CPU")))
			spa_cpu_force_flags(cpu, strtoul(str, NULL, 0));

		support->support[support->n_support++] =
			SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_CPU, iface);
	}
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
	return NULL;
}

/** Get the program name \memberof pw_pipewire */
SPA_EXPORT
const char *pw_get_prgname(void)
{
	static char tcomm[16 + 1];
	spa_zero(tcomm);

	if (prctl(PR_GET_NAME, (unsigned long) tcomm, 0, 0, 0) == 0)
		return tcomm;

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

/** Fill remote properties
 * \param properties a \ref pw_properties
 *
 * Fill \a properties with a set of default remote properties.
 *
 * \memberof pw_pipewire
 */
SPA_EXPORT
void pw_fill_remote_properties(struct pw_core *core, struct pw_properties *properties)
{
	const char *val;

	if (!pw_properties_get(properties, PW_KEY_APP_NAME))
		pw_properties_set(properties, PW_KEY_APP_NAME, pw_get_client_name());

	if (!pw_properties_get(properties, PW_KEY_APP_PROCESS_BINARY))
		pw_properties_set(properties, PW_KEY_APP_PROCESS_BINARY, pw_get_prgname());

	if (!pw_properties_get(properties, PW_KEY_APP_LANGUAGE)) {
		pw_properties_set(properties, PW_KEY_APP_LANGUAGE, getenv("LANG"));
	}
	if (!pw_properties_get(properties, PW_KEY_APP_PROCESS_ID)) {
		pw_properties_setf(properties, PW_KEY_APP_PROCESS_ID, "%zd", (size_t) getpid());
	}
	if (!pw_properties_get(properties, PW_KEY_APP_PROCESS_USER))
		pw_properties_set(properties, PW_KEY_APP_PROCESS_USER, pw_get_user_name());

	if (!pw_properties_get(properties, PW_KEY_APP_PROCESS_HOST))
		pw_properties_set(properties, PW_KEY_APP_PROCESS_HOST, pw_get_host_name());

	if (!pw_properties_get(properties, PW_KEY_APP_PROCESS_SESSION_ID)) {
		pw_properties_set(properties, PW_KEY_APP_PROCESS_SESSION_ID,
				  getenv("XDG_SESSION_ID"));
	}
	if (!pw_properties_get(properties, PW_KEY_WINDOW_X11_DISPLAY)) {
		pw_properties_set(properties, PW_KEY_WINDOW_X11_DISPLAY,
				  getenv("DISPLAY"));
	}
	pw_properties_set(properties, PW_KEY_CORE_VERSION, core->info.version);
	pw_properties_set(properties, PW_KEY_CORE_NAME, core->info.name);

	if ((val = pw_properties_get(core->properties, PW_KEY_CORE_DAEMON)))
		pw_properties_set(properties, PW_KEY_CORE_DAEMON, val);
}

/** Fill stream properties
 * \param properties a \ref pw_properties
 *
 * Fill \a properties with a set of default stream properties.
 *
 * \memberof pw_pipewire
 */
SPA_EXPORT
void pw_fill_stream_properties(struct pw_core *core, struct pw_properties *properties)
{
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
	{ PW_TYPE_INTERFACE_Core, SPA_TYPE_Pointer, PW_TYPE_INFO_INTERFACE_BASE "Core", NULL },
	{ PW_TYPE_INTERFACE_Registry, SPA_TYPE_Pointer, PW_TYPE_INFO_INTERFACE_BASE "Registry", NULL },
	{ PW_TYPE_INTERFACE_Node, SPA_TYPE_Pointer, PW_TYPE_INFO_INTERFACE_BASE "Node", NULL },
	{ PW_TYPE_INTERFACE_Port, SPA_TYPE_Pointer, PW_TYPE_INFO_INTERFACE_BASE "Port", NULL },
	{ PW_TYPE_INTERFACE_Factory, SPA_TYPE_Pointer, PW_TYPE_INFO_INTERFACE_BASE "Factory", NULL },
	{ PW_TYPE_INTERFACE_Link, SPA_TYPE_Pointer, PW_TYPE_INFO_INTERFACE_BASE "Link", NULL },
	{ PW_TYPE_INTERFACE_Client, SPA_TYPE_Pointer, PW_TYPE_INFO_INTERFACE_BASE "Client", NULL },
	{ PW_TYPE_INTERFACE_Module, SPA_TYPE_Pointer, PW_TYPE_INFO_INTERFACE_BASE "Module", NULL },
	{ PW_TYPE_INTERFACE_ClientNode, SPA_TYPE_Pointer, PW_TYPE_INFO_INTERFACE_BASE "ClientNode", NULL },
	{ PW_TYPE_INTERFACE_Device, SPA_TYPE_Pointer, PW_TYPE_INFO_INTERFACE_BASE "Device", NULL },
	{ SPA_ID_INVALID, SPA_ID_INVALID, "spa_types", spa_types },
	{ 0, 0, NULL, NULL },
};

SPA_EXPORT
const struct spa_type_info * pw_type_info(void)
{
	return type_info;
}
