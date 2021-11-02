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

#include "config.h"

#include <unistd.h>
#include <limits.h>
#include <stdio.h>
#ifndef __FreeBSD__
#include <sys/prctl.h>
#endif
#include <pwd.h>
#include <errno.h>
#include <dlfcn.h>
#include <pthread.h>

#include <locale.h>
#include <libintl.h>

#include <valgrind/valgrind.h>

#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/support/cpu.h>
#include <spa/support/i18n.h>

#include "pipewire.h"
#include "private.h"

#define MAX_SUPPORT	32

#define SUPPORTLIB	"support/libspa-support"

PW_LOG_TOPIC_EXTERN(log_context);
#define PW_LOG_TOPIC_DEFAULT log_context

static char *prgname;

static struct spa_i18n *_pipewire_i18n = NULL;

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
	struct spa_handle handle SPA_ALIGNED(8);
};

struct registry {
	struct spa_list plugins;
};

struct support {
	char **categories;
	const char *plugin_dir;
	const char *support_lib;
	struct registry registry;
	char *i18n_domain;
	struct spa_interface i18n_iface;
	struct spa_support support[MAX_SUPPORT];
	uint32_t n_support;
	unsigned int initialized:1;
	unsigned int in_valgrind:1;
	unsigned int no_color:1;
	unsigned int no_config:1;
};

static pthread_mutex_t init_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t support_lock = PTHREAD_MUTEX_INITIALIZER;
static struct support global_support;

static struct plugin *
find_plugin(struct registry *registry, const char *filename)
{
	struct plugin *p;
	spa_list_for_each(p, &registry->plugins, link) {
		if (spa_streq(p->filename, filename))
			return p;
	}
	return NULL;
}

static struct plugin *
open_plugin(struct registry *registry,
	    const char *path, size_t len, const char *lib)
{
	struct plugin *plugin;
	char filename[PATH_MAX];
	void *hnd;
	spa_handle_factory_enum_func_t enum_func;
	int res;

        if ((res = spa_scnprintf(filename, sizeof(filename), "%.*s/%s.so", (int)len, path, lib)) < 0)
		goto error_out;

	if ((plugin = find_plugin(registry, filename)) != NULL) {
		plugin->ref++;
		return plugin;
	}

        if ((hnd = dlopen(filename, RTLD_NOW)) == NULL) {
		res = -ENOENT;
		pw_log_debug("can't load %s: %s", filename, dlerror());
		goto error_out;
        }
        if ((enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		res = -ENOSYS;
		pw_log_debug("can't find enum function: %s", dlerror());
		goto error_dlclose;
        }

	if ((plugin = calloc(1, sizeof(struct plugin))) == NULL) {
		res = -errno;
		goto error_dlclose;
	}

	pw_log_debug("loaded plugin:'%s'", filename);
	plugin->ref = 1;
	plugin->filename = strdup(filename);
	plugin->hnd = hnd;
	plugin->enum_func = enum_func;
	spa_list_init(&plugin->handles);

	spa_list_append(&registry->plugins, &plugin->link);

	return plugin;

error_dlclose:
	dlclose(hnd);
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
		if (!global_support.in_valgrind)
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
		if (factory->version < 1) {
			pw_log_warn("factory version %d < 1 not supported",
					factory->version);
			continue;
		}
                if (spa_streq(factory->name, factory_name))
                        return factory;
	}
	res = -ENOENT;
out:
	pw_log_debug("can't find factory %s: %s", factory_name, spa_strerror(res));
	errno = -res;
	return NULL;
}

static void unref_handle(struct handle *handle)
{
	if (--handle->ref == 0) {
		spa_list_remove(&handle->link);
		pw_log_debug("clear handle '%s'", handle->factory_name);
		pthread_mutex_unlock(&support_lock);
		spa_handle_clear(&handle->handle);
		pthread_mutex_lock(&support_lock);
		unref_plugin(handle->plugin);
		free(handle->factory_name);
		free(handle);
	}
}

SPA_EXPORT
uint32_t pw_get_support(struct spa_support *support, uint32_t max_support)
{
	uint32_t i, n = SPA_MIN(global_support.n_support, max_support);
	for (i = 0; i < n; i++)
		support[i] = global_support.support[i];
	return n;
}

static struct spa_handle *load_spa_handle(const char *lib,
		const char *factory_name,
		const struct spa_dict *info,
		uint32_t n_support,
		const struct spa_support support[])
{
	struct support *sup = &global_support;
	struct plugin *plugin;
	struct handle *handle;
	const struct spa_handle_factory *factory;
	const char *state = NULL, *p;
	int res;
	size_t len;

	if (factory_name == NULL) {
		res = -EINVAL;
		goto error_out;
	}

	if (lib == NULL)
		lib = sup->support_lib;

	pw_log_debug("load lib:'%s' factory-name:'%s'", lib, factory_name);

	plugin = NULL;
	res = -ENOENT;

	if (sup->plugin_dir == NULL) {
		pw_log_error("load lib: plugin directory undefined, set SPA_PLUGIN_DIR");
		goto error_out;
	}
	while ((p = pw_split_walk(sup->plugin_dir, ":", &len, &state))) {
		if ((plugin = open_plugin(&sup->registry, p, len, lib)) != NULL)
			break;
		res = -errno;
	}
	if (plugin == NULL)
		goto error_out;

	pthread_mutex_unlock(&support_lock);

	factory = find_factory(plugin, factory_name);
	if (factory == NULL) {
		res = -errno;
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
		pw_log_debug("can't make factory instance '%s': %d (%s)",
				factory_name, res, spa_strerror(res));
		goto error_free_handle;
	}

	pthread_mutex_lock(&support_lock);
	handle->ref = 1;
	handle->plugin = plugin;
	handle->factory_name = strdup(factory_name);
	spa_list_append(&plugin->handles, &handle->link);

	return &handle->handle;

error_free_handle:
	free(handle);
error_unref_plugin:
	pthread_mutex_lock(&support_lock);
	unref_plugin(plugin);
error_out:
	errno = -res;
	return NULL;
}

SPA_EXPORT
struct spa_handle *pw_load_spa_handle(const char *lib,
		const char *factory_name,
		const struct spa_dict *info,
		uint32_t n_support,
		const struct spa_support support[])
{
	struct spa_handle *handle;
	pthread_mutex_lock(&support_lock);
	handle = load_spa_handle(lib, factory_name, info, n_support, support);
	pthread_mutex_unlock(&support_lock);
	return handle;
}

static struct handle *find_handle(struct spa_handle *handle)
{
	struct registry *registry = &global_support.registry;
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
	int res = 0;

	pthread_mutex_lock(&support_lock);
	if ((h = find_handle(handle)) == NULL)
		res = -ENOENT;
	else
		unref_handle(h);
	pthread_mutex_unlock(&support_lock);

	return res;
}

static void *add_interface(struct support *support,
		const char *factory_name,
		const char *type,
		const struct spa_dict *info)
{
	struct spa_handle *handle;
	void *iface = NULL;
	int res = -ENOENT;

	handle = load_spa_handle(support->support_lib,
			factory_name, info,
			support->n_support, support->support);
	if (handle == NULL)
		return NULL;

	pthread_mutex_unlock(&support_lock);
	res = spa_handle_get_interface(handle, type, &iface);
	pthread_mutex_lock(&support_lock);

	if (res < 0 || iface == NULL) {
		pw_log_error("can't get %s interface %d: %s", type, res,
				spa_strerror(res));
		return NULL;
	}

	support->support[support->n_support++] =
		SPA_SUPPORT_INIT(type, iface);
	return iface;
}

SPA_EXPORT
int pw_set_domain(const char *domain)
{
	struct support *support = &global_support;
	free(support->i18n_domain);
	if (domain == NULL)
		support->i18n_domain = NULL;
	else if ((support->i18n_domain = strdup(domain)) == NULL)
		return -errno;
	return 0;
}

SPA_EXPORT
const char *pw_get_domain(void)
{
	struct support *support = &global_support;
	return support->i18n_domain;
}

static const char *i18n_text(void *object, const char *msgid)
{
	struct support *support = object;
	return dgettext(support->i18n_domain, msgid);
}

static const char *i18n_ntext(void *object, const char *msgid, const char *msgid_plural,
		unsigned long int n)
{
	struct support *support = object;
	return dngettext(support->i18n_domain, msgid, msgid_plural, n);
}

static void init_i18n(struct support *support)
{
	/* Load locale from the environment. */
	setlocale(LC_ALL, "");
	/* Set LC_NUMERIC to C so that floating point strings are consistently
	 * formatted and parsed across locales. */
	setlocale(LC_NUMERIC, "C");
	bindtextdomain(GETTEXT_PACKAGE, LOCALEDIR);
	bind_textdomain_codeset(GETTEXT_PACKAGE, "UTF-8");
	pw_set_domain(GETTEXT_PACKAGE);
}

static void *add_i18n(struct support *support)
{
	static const struct spa_i18n_methods i18n_methods = {
		SPA_VERSION_I18N_METHODS,
		.text = i18n_text,
		.ntext = i18n_ntext,
	};

	support->i18n_iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_I18N,
			SPA_VERSION_I18N,
			&i18n_methods, support);
	_pipewire_i18n = (struct spa_i18n*) &support->i18n_iface;

	support->support[support->n_support++] =
		SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_I18N, _pipewire_i18n);

	return 0;
}

SPA_EXPORT
const char *pw_gettext(const char *msgid)
{
	return spa_i18n_text(_pipewire_i18n, msgid);
}
SPA_EXPORT
const char *pw_ngettext(const char *msgid, const char *msgid_plural, unsigned long int n)
{
	return spa_i18n_ntext(_pipewire_i18n, msgid, msgid_plural, n);
}

#ifdef HAVE_SYSTEMD
static struct spa_log *load_journal_logger(struct support *support,
					   const struct spa_dict *info)
{
	struct spa_handle *handle;
	void *iface = NULL;
	int res = -ENOENT;
	uint32_t i;

	/* is the journal even available? */
	if (access("/run/systemd/journal/socket", F_OK) != 0)
		return NULL;

	handle = load_spa_handle("support/libspa-journal",
				    SPA_NAME_SUPPORT_LOG, info,
				    support->n_support, support->support);
	if (handle == NULL)
		return NULL;

	pthread_mutex_unlock(&support_lock);
	res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Log, &iface);
	pthread_mutex_lock(&support_lock);

	if (res < 0 || iface == NULL) {
		pw_log_error("can't get log interface %d: %s", res,
				spa_strerror(res));
		return NULL;
	}

	/* look for an existing logger, and
	 * replace it with the journal logger */
	for (i = 0; i < support->n_support; i++) {
		if (spa_streq(support->support[i].type, SPA_TYPE_INTERFACE_Log)) {
			support->support[i].data = iface;
			break;
		}
	}
	return (struct spa_log *) iface;
}
#endif

static enum spa_log_level
parse_log_level(const char *str)
{
	enum spa_log_level l = SPA_LOG_LEVEL_NONE;
	switch(str[0]) {
		case 'X': l = SPA_LOG_LEVEL_NONE; break;
		case 'E': l = SPA_LOG_LEVEL_ERROR; break;
		case 'W': l = SPA_LOG_LEVEL_WARN; break;
		case 'I': l = SPA_LOG_LEVEL_INFO; break;
		case 'D': l = SPA_LOG_LEVEL_DEBUG; break;
		case 'T': l = SPA_LOG_LEVEL_TRACE; break;
		default:
			  l = atoi(str);
			  break;
	}
	return l;
}

static char *
parse_pw_debug_env(void)
{
	const char *str;
	char **tokens;
	int n_tokens;
	size_t slen;
	char json[1024] = {0};
	char *pos = json;
	char *end = pos + sizeof(json) - 1;

	str = getenv("PIPEWIRE_DEBUG");

	if (!str || (slen = strlen(str)) == 0)
		return NULL;

	/* String format is PIPEWIRE_DEBUG=<glob>:<level>[,<glob>:<level>,...],
	 * converted into [{ conn.* = 0}, {glob = level}, {glob = level}, ....] ,
	 * with the connection namespace disabled by default.
	 */
	pos += spa_scnprintf(pos, end - pos, "[ { conn.* = %d },", SPA_LOG_LEVEL_NONE);

	/* We only have single-digit log levels, so any single-character
	 * string is of the form PIPEWIRE_DEBUG=<N> */
	if (slen == 1) {
		pw_log_set_level(parse_log_level(str));
		goto out;
	}

	tokens = pw_split_strv(str, ",", INT_MAX, &n_tokens);
	if (n_tokens > 0) {
		int i;
		for (i = 0; i < n_tokens; i++) {
			int n_tok;
			char **tok;
			char *pattern;
			enum spa_log_level lvl;

			tok = pw_split_strv(tokens[i], ":", 2, &n_tok);
			if (n_tok == 2) {
				pattern = tok[0];
				lvl = parse_log_level(tok[1]);

				pos += spa_scnprintf(pos, end - pos, "{ %s = %d },",
						     pattern, lvl);
			} else {
				pw_log_warn("Ignoring invalid format in PIPEWIRE_DEBUG: '%s'\n", tokens[i]);
			}

			pw_free_strv(tok);
		}
	}
	pw_free_strv(tokens);
out:
	pos += spa_scnprintf(pos, end - pos, "]");
	return strdup(json);
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
 */
SPA_EXPORT
void pw_init(int *argc, char **argv[])
{
	const char *str;
	struct spa_dict_item items[6];
	uint32_t n_items;
	struct spa_dict info;
	struct support *support = &global_support;
	struct spa_log *log;
	char level[32];

	pthread_mutex_lock(&init_lock);
	if (support->initialized)
		goto done;

	pthread_mutex_lock(&support_lock);
	support->in_valgrind = RUNNING_ON_VALGRIND;

	if (getenv("NO_COLOR") != NULL)
		support->no_color = true;

	if ((str = getenv("PIPEWIRE_NO_CONFIG")) != NULL)
		support->no_config = pw_properties_parse_bool(str);

	init_i18n(support);

	if ((str = getenv("SPA_PLUGIN_DIR")) == NULL)
		str = PLUGINDIR;
	support->plugin_dir = str;

	if ((str = getenv("SPA_SUPPORT_LIB")) == NULL)
		str = SUPPORTLIB;
	support->support_lib = str;

	spa_list_init(&support->registry.plugins);

	if (pw_log_is_default()) {
		char *patterns = NULL;

		n_items = 0;
		if (!support->no_color)
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_COLORS, "true");
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_TIMESTAMP, "true");
		if ((str = getenv("PIPEWIRE_LOG_LINE")) == NULL || spa_atob(str))
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_LINE, "true");
		snprintf(level, sizeof(level), "%d", pw_log_level);
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_LEVEL, level);
		if ((str = getenv("PIPEWIRE_LOG")) != NULL)
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_FILE, str);
		if ((patterns = parse_pw_debug_env()) != NULL)
			items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_LOG_PATTERNS, patterns);
		info = SPA_DICT_INIT(items, n_items);

		log = add_interface(support, SPA_NAME_SUPPORT_LOG, SPA_TYPE_INTERFACE_Log, &info);
		if (log)
			pw_log_set(log);

#ifdef HAVE_SYSTEMD
		if ((str = getenv("PIPEWIRE_LOG_SYSTEMD")) == NULL || spa_atob(str)) {
			log = load_journal_logger(support, &info);
			if (log)
				pw_log_set(log);
		}
#endif
		free(patterns);
	} else {
		support->support[support->n_support++] =
			SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Log, pw_log_get());
	}

	pw_log_init();

	n_items = 0;
	if ((str = getenv("PIPEWIRE_CPU")))
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_CPU_FORCE, str);
	if ((str = getenv("PIPEWIRE_VM")))
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_CPU_VM_TYPE, str);
	info = SPA_DICT_INIT(items, n_items);

	add_interface(support, SPA_NAME_SUPPORT_CPU, SPA_TYPE_INTERFACE_CPU, &info);

	add_i18n(support);

	pw_log_info("version %s", pw_get_library_version());
	support->initialized = true;
	pthread_mutex_unlock(&support_lock);
done:
	pthread_mutex_unlock(&init_lock);
}

SPA_EXPORT
void pw_deinit(void)
{
	struct support *support = &global_support;
	struct registry *registry = &support->registry;
	struct plugin *p;

	pthread_mutex_lock(&init_lock);
	pthread_mutex_lock(&support_lock);
	pw_log_set(NULL);
	spa_list_consume(p, &registry->plugins, link) {
		struct handle *h;
		p->ref++;
		spa_list_consume(h, &p->handles, link)
			unref_handle(h);
		unref_plugin(p);
	}
	pw_free_strv(support->categories);
	free(support->i18n_domain);
	spa_zero(global_support);
	pthread_mutex_unlock(&support_lock);
	pthread_mutex_unlock(&init_lock);

}

/** Check if a debug category is enabled
 *
 * \param name the name of the category to check
 * \return true if enabled
 *
 * Debugging categories can be enabled by using the PIPEWIRE_DEBUG
 * environment variable
 *
 */
SPA_EXPORT
bool pw_debug_is_category_enabled(const char *name)
{
	int i;

	if (global_support.categories == NULL)
		return false;

	for (i = 0; global_support.categories[i]; i++) {
		if (spa_streq(global_support.categories[i], name))
			return true;
	}
	return false;
}

/** Get the application name */
SPA_EXPORT
const char *pw_get_application_name(void)
{
	errno = ENOTSUP;
	return NULL;
}

static void init_prgname(void)
{
	static char name[PATH_MAX];

	spa_memzero(name, sizeof(name));
#if defined(__linux__) || defined(__FreeBSD_kernel__)
	{
		if (readlink("/proc/self/exe", name, sizeof(name)-1) > 0) {
			prgname = strrchr(name, '/') + 1;
			return;
		}
	}
#endif
#if defined __FreeBSD__
	{
		ssize_t len;

		if ((len = readlink("/proc/curproc/file", name, sizeof(name)-1)) > 0) {
			prgname = strrchr(name, '/') + 1;
			return;
		}
	}
#endif
#ifndef __FreeBSD__
	{
		if (prctl(PR_GET_NAME, (unsigned long) name, 0, 0, 0) == 0) {
			prgname = name;
			return;
		}
	}
#endif
	snprintf(name, sizeof(name), "pid-%d", getpid());
	prgname = name;
}

/** Get the program name */
SPA_EXPORT
const char *pw_get_prgname(void)
{
	static pthread_once_t prgname_is_initialized = PTHREAD_ONCE_INIT;

	pthread_once(&prgname_is_initialized, init_prgname);
	return prgname;
}

/** Get the user name */
SPA_EXPORT
const char *pw_get_user_name(void)
{
	struct passwd *pw;

	if ((pw = getpwuid(getuid())))
		return pw->pw_name;

	return NULL;
}

/** Get the host name */
SPA_EXPORT
const char *pw_get_host_name(void)
{
	static char hname[256];

	if (gethostname(hname, 256) < 0)
		return NULL;

	hname[255] = 0;
	return hname;
}

SPA_EXPORT
bool pw_in_valgrind(void)
{
	return global_support.in_valgrind;
}

SPA_EXPORT
bool pw_check_option(const char *option, const char *value)
{
	if (spa_streq(option, "in-valgrind"))
		return global_support.in_valgrind == spa_atob(value);
	else if (spa_streq(option, "no-color"))
		return global_support.no_color == spa_atob(value);
	else if (spa_streq(option, "no-config"))
		return global_support.no_config == spa_atob(value);
	else
		return false;
}

/** Get the client name
 *
 * Make a new PipeWire client name that can be used to construct a remote.
 *
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
		return cname;
	}
}

/** Reverse the direction */
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
