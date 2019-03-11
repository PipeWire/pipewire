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

#include <spa/support/dbus.h>
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
	struct spa_handle *handle;
	struct spa_list interfaces;
	int ref;
};

struct interface {
	struct spa_list link;
	struct handle *handle;
	void *iface;
	uint32_t type;
	int ref;
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
        struct spa_handle *hnd;
        const struct spa_handle_factory *factory;

	factory = find_factory(plugin, factory_name);
	if (factory == NULL)
		goto not_found;

	hnd = calloc(1, spa_handle_factory_get_size(factory, NULL));
	if (hnd == NULL)
		goto alloc_failed;

        if ((res = spa_handle_factory_init(factory,
                                           hnd, info,
					   support, n_support)) < 0) {
                fprintf(stderr, "can't make factory instance %s: %d\n", factory_name, res);
                goto init_failed;
        }

	if ((handle = calloc(1, sizeof(struct handle))) == NULL)
		goto handle_failed;

	handle->ref = 1;
	handle->plugin = plugin;
	handle->factory_name = factory_name;
	handle->handle = hnd;
	spa_list_init(&handle->interfaces);

	spa_list_append(&plugin->handles, &handle->link);

	return handle;

      handle_failed:
	spa_handle_clear(hnd);
      init_failed:
	free(hnd);
      alloc_failed:
      not_found:
	return NULL;
}

static void unref_handle(struct handle *handle)
{
	if (--handle->ref == 0) {
		spa_list_remove(&handle->link);
		spa_handle_clear(handle->handle);
		free(handle->handle);
		unref_plugin(handle->plugin);
		free(handle);
	}
}

static struct interface *
load_interface(struct plugin *plugin,
	       const char *factory_name,
	       uint32_t type_id,
	       const struct spa_dict *info,
	       uint32_t n_support,
	       struct spa_support support[n_support])
{
        int res;
        struct handle *handle;
        void *ptr;
	struct interface *iface;

        handle = load_handle(plugin, factory_name, info, n_support, support);
	if (handle == NULL)
		goto not_found;

        if ((res = spa_handle_get_interface(handle->handle, type_id, &ptr)) < 0) {
                fprintf(stderr, "can't get %d interface %d\n", type_id, res);
                goto interface_failed;
        }

	if ((iface = calloc(1, sizeof(struct interface))) == NULL)
		goto alloc_failed;

	iface->ref = 1;
	iface->handle = handle;
	iface->type = type_id;
	iface->iface = ptr;
	spa_list_append(&handle->interfaces, &iface->link);

        return iface;

      alloc_failed:
      interface_failed:
	unref_handle(handle);
	free(handle);
      not_found:
	return NULL;
}

static void
unref_interface(struct interface *iface)
{
	if (--iface->ref == 0) {
		spa_list_remove(&iface->link);
		unref_handle(iface->handle);
		free(iface);
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
void *pw_load_spa_interface(const char *lib, const char *factory_name, uint32_t type,
			    const struct spa_dict *info,
			    uint32_t n_support,
			    const struct spa_support support[])
{
	struct support *sup = &global_support;
	struct spa_support extra_support[MAX_SUPPORT];
	uint32_t extra_n_support;
	struct plugin *plugin;
	struct interface *iface;
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

	if ((iface = load_interface(plugin, factory_name, type, info,
					extra_n_support, extra_support)) == NULL)
		return NULL;

	return iface->iface;
}

static struct interface *find_interface(void *iface)
{
	struct registry *registry = global_support.registry;
	struct plugin *p;
	struct handle *h;
	struct interface *i;

	spa_list_for_each(p, &registry->plugins, link) {
		spa_list_for_each(h, &p->handles, link) {
			spa_list_for_each(i, &h->interfaces, link) {
				if (i->iface == iface)
					return i;
			}
		}
	}
	return NULL;
}

SPA_EXPORT
int pw_unload_spa_interface(void *iface)
{
	struct interface *i;

	if ((i = find_interface(iface)) == NULL)
		return -ENOENT;

	unref_interface(i);

	return 0;
}

SPA_EXPORT
void *pw_load_spa_dbus_interface(struct pw_loop *loop)
{
	struct spa_support support = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_LoopUtils, loop->utils);

	return pw_load_spa_interface("support/libspa-dbus", "dbus", SPA_TYPE_INTERFACE_DBus,
			NULL, 1, &support);
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
	struct interface *iface;
	struct support *support = &global_support;
	struct plugin *plugin;
	struct spa_dict info;
	struct spa_dict_item items[1];

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

	items[0] = SPA_DICT_ITEM_INIT("log.colors", "1");
	info = SPA_DICT_INIT(items, 1);

	iface = load_interface(plugin, "logger", SPA_TYPE_INTERFACE_Log, &info,
			support->n_support, support->support);
	if (iface != NULL) {
		support->support[support->n_support++] =
			SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Log, iface->iface);
		pw_log_set(iface->iface);
	}
	iface = load_interface(plugin, "cpu", SPA_TYPE_INTERFACE_CPU, &info,
			support->n_support, support->support);
	if (iface != NULL) {
		struct spa_cpu *cpu = iface->iface;
		if ((str = getenv("PIPEWIRE_CPU")))
			spa_cpu_force_flags(cpu, strtoul(str, NULL, 0));

		support->support[support->n_support++] =
			SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_CPU, iface->iface);
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

	if (!pw_properties_get(properties, "application.name"))
		pw_properties_set(properties, "application.name", pw_get_client_name());

	if (!pw_properties_get(properties, "application.prgname"))
		pw_properties_set(properties, "application.prgname", pw_get_prgname());

	if (!pw_properties_get(properties, "application.language")) {
		pw_properties_set(properties, "application.language", getenv("LANG"));
	}
	if (!pw_properties_get(properties, "application.process.id")) {
		pw_properties_setf(properties, "application.process.id", "%zd", (size_t) getpid());
	}
	if (!pw_properties_get(properties, "application.process.user"))
		pw_properties_set(properties, "application.process.user", pw_get_user_name());

	if (!pw_properties_get(properties, "application.process.host"))
		pw_properties_set(properties, "application.process.host", pw_get_host_name());

	if (!pw_properties_get(properties, "application.process.session_id")) {
		pw_properties_set(properties, "application.process.session_id",
				  getenv("XDG_SESSION_ID"));
	}
	if (!pw_properties_get(properties, "window.x11.display")) {
		pw_properties_set(properties, "window.x11.display",
				  getenv("DISPLAY"));
	}
	pw_properties_set(properties, PW_CORE_PROP_VERSION, core->info.version);
	pw_properties_set(properties, PW_CORE_PROP_NAME, core->info.name);

	if ((val = pw_properties_get(core->properties, PW_CORE_PROP_DAEMON)))
		pw_properties_set(properties, PW_CORE_PROP_DAEMON, val);
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
