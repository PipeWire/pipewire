/* PipeWire
 * Copyright (C) 2015 Wim Taymans <wim.taymans@gmail.com>
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

#include <unistd.h>
#include <limits.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <pwd.h>
#include <errno.h>
#include <dlfcn.h>

#include "pipewire/client/pipewire.h"

static char **categories = NULL;

static struct support_info {
	void *hnd;
	spa_handle_factory_enum_func_t enum_func;
	struct spa_support support[4];
	uint32_t n_support;
} support_info;

static bool
open_support(const char *lib,
	     struct support_info *info)
{
        if ((info->hnd = dlopen(lib, RTLD_NOW)) == NULL) {
                fprintf(stderr, "can't load %s: %s\n", lib, dlerror());
                return false;
        }
        if ((info->enum_func = dlsym(info->hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
                fprintf(stderr, "can't find enum function\n");
                goto no_symbol;
        }
	return true;

      no_symbol:
	dlclose(info->hnd);
	return false;
}

static void *
load_interface(struct support_info *info,
	       const char *factory_name,
	       const char *type)
{
        int res;
        struct spa_handle *handle;
        uint32_t type_id;
        const struct spa_handle_factory *factory;
        void *iface;
	struct spa_type_map *map = NULL;

	factory = pw_get_support_factory(factory_name);
	if (factory == NULL)
		goto not_found;

        handle = calloc(1, factory->size);
        if ((res = spa_handle_factory_init(factory,
                                           handle, NULL, info->support, info->n_support)) < 0) {
                fprintf(stderr, "can't make factory instance: %d\n", res);
                goto init_failed;
        }

	map = pw_get_support_interface(SPA_TYPE__TypeMap);
	type_id = map ? spa_type_map_get_id(map, type) : 0;

        if ((res = spa_handle_get_interface(handle, type_id, &iface)) < 0) {
                fprintf(stderr, "can't get %s interface %d\n", type, res);
                goto interface_failed;
        }
        return iface;

      interface_failed:
	spa_handle_clear(handle);
      init_failed:
	free(handle);
      not_found:
	return NULL;
}

static void configure_debug(const char *str)
{
	char **level;
	int n_tokens;

	level = pw_split_strv(str, ":", INT_MAX, &n_tokens);
	if (n_tokens > 0)
		pw_log_set_level(atoi(level[0]));

	if (n_tokens > 1)
		categories = pw_split_strv(level[1], ",", INT_MAX, &n_tokens);
}

static void configure_support(struct support_info *info)
{
	void *iface;

	iface = load_interface(info, "mapper", SPA_TYPE__TypeMap);
	if (iface != NULL) {
		info->support[info->n_support++] = SPA_SUPPORT_INIT(SPA_TYPE__TypeMap, iface);
	}

	iface = load_interface(info, "logger", SPA_TYPE__Log);
	if (iface != NULL) {
		info->support[info->n_support++] = SPA_SUPPORT_INIT(SPA_TYPE__Log, iface);
		pw_log_set(iface);
	}
}

/** Get a support interface
 * \param type the interface type
 * \return the interface or NULL when not configured
 */
void *pw_get_support_interface(const char *type)
{
	int i;

	for (i = 0; i < support_info.n_support; i++) {
		if (strcmp(support_info.support->type, type) == 0)
			return support_info.support->data;
	}
	return NULL;
}

const struct spa_handle_factory *pw_get_support_factory(const char *factory_name)
{
	int res;
	uint32_t index;
        const struct spa_handle_factory *factory;

        for (index = 0;; index++) {
                if ((res = support_info.enum_func(&factory, index)) < 0) {
                        if (res != SPA_RESULT_ENUM_END)
                                fprintf(stderr, "can't enumerate factories: %d\n", res);
                        break;
                }
                if (strcmp(factory->name, factory_name) == 0)
                        return factory;
	}
	return NULL;
}

const struct spa_support *pw_get_support(uint32_t *n_support)
{
	*n_support = support_info.n_support;
	return support_info.support;
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
void pw_init(int *argc, char **argv[])
{
	const char *str;

	if ((str = getenv("PIPEWIRE_DEBUG")))
		configure_debug(str);

	if (open_support("build/spa/plugins/support/libspa-support.so", &support_info))
		configure_support(&support_info);
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
bool pw_debug_is_category_enabled(const char *name)
{
	int i;

	if (categories == NULL)
		return false;

	for (i = 0; categories[i]; i++) {
		if (strcmp (categories[i], name) == 0)
			return true;
	}
	return false;
}

/** Get the application name \memberof pw_pipewire */
const char *pw_get_application_name(void)
{
	return NULL;
}

/** Get the program name \memberof pw_pipewire */
const char *pw_get_prgname(void)
{
	static char tcomm[16 + 1];
	spa_zero(tcomm);

	if (prctl(PR_GET_NAME, (unsigned long) tcomm, 0, 0, 0) == 0)
		return tcomm;

	return NULL;
}

/** Get the user name \memberof pw_pipewire */
const char *pw_get_user_name(void)
{
	struct passwd *pw;

	if ((pw = getpwuid(getuid())))
		return pw->pw_name;

	return NULL;
}

/** Get the host name \memberof pw_pipewire */
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
 * Make a new PipeWire client name that can be used to construct a context.
 *
 * \memberof pw_pipewire
 */
char *pw_get_client_name(void)
{
	char *c;
	const char *cc;

	if ((cc = pw_get_application_name()))
		return strdup(cc);
	else if ((cc = pw_get_prgname()))
		return strdup(cc);
	else {
		asprintf(&c, "pipewire-pid-%zd", (size_t) getpid());
		return c;
	}
}

/** Fill context properties
 * \param properties a \ref pw_properties
 *
 * Fill \a properties with a set of default context properties.
 *
 * \memberof pw_pipewire
 */
void pw_fill_context_properties(struct pw_properties *properties)
{
	if (!pw_properties_get(properties, "application.name"))
		pw_properties_set(properties, "application.name", pw_get_application_name());

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
}

/** Fill stream properties
 * \param properties a \ref pw_properties
 *
 * Fill \a properties with a set of default stream properties.
 *
 * \memberof pw_pipewire
 */
void pw_fill_stream_properties(struct pw_properties *properties)
{
}

/** Reverse the direction \memberof pw_pipewire */
enum pw_direction pw_direction_reverse(enum pw_direction direction)
{
	if (direction == PW_DIRECTION_INPUT)
		return PW_DIRECTION_OUTPUT;
	else if (direction == PW_DIRECTION_OUTPUT)
		return PW_DIRECTION_INPUT;
	return direction;
}
