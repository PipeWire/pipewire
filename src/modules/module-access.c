/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include "config.h"

#ifdef HAVE_SYS_VFS_H
#include <sys/vfs.h>
#endif
#ifdef HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>

#include <pipewire/impl.h>

#include "flatpak-utils.h"

/** \page page_module_access PipeWire Module: Access
 *
 *
 * The `access` module performs access checks on clients. The access check
 * is only performed once per client, subsequent checks return the same
 * resolution.
 *
 * Permissions assigned to a client are configured as arguments to this
 * module, see the example configuration below. A special use-case is Flatpak
 * where the permission management is delegated.
 *
 * This module sets the \ref PW_KEY_ACCESS property to one of
 * - `allowed`: the client is explicitly allowed to access all resources
 * - `rejected`: the client does not have access to any resources and a
 *   resource error is generated
 * - `restricted`: the client is restricted, see note below
 * - `flatpak`: restricted, special case for clients running inside flatpak,
 *   see note below
 * - `$access.force`: the value of the `access.force` argument given in the
 *   module configuration.
 * - `unrestricted`: the client is allowed to access all resources. This is the
 *   default for clients not listed in any of the `access.*` options
 *   unless the client requested reduced permissions in \ref
 *   PW_KEY_CLIENT_ACCESS.
 *
 * \note Clients with a resolution other than `allowed` or `rejected` rely
 *       on an external actor to update that property once permission is
 *       granted or rejected.
 *
 * For connections from applications running inside Flatpak not mediated
 * by a portal, the `access` module itself sets the `pipewire.access.portal.app_id`
 * property to the Flatpak application ID.
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - ``access.allowed = []``: an array of paths of allowed applications
 * - ``access.rejected = []``: an array of paths of rejected applications
 * - ``access.restricted = []``: an array of paths of restricted applications
 * - ``access.force = <str>``: forces an external permissions check (e.g. a flatpak
 *   portal)
 *
 * ## General options
 *
 * Options with well-known behavior:
 *
 * - \ref PW_KEY_ACCESS
 * - \ref PW_KEY_CLIENT_ACCESS
 *
 * ## Example configuration
 *
 *\code{.unparsed}
 * context.modules = [
 *  {   name = libpipewire-module-access
 *      args = {
 *          access.allowed = [
 *              /usr/bin/pipewire-media-session
 *              /usr/bin/important-thing
 *          ]
 *
 *          access.rejected = [
 *              /usr/bin/microphone-snooper
 *          ]
 *
 *          #access.restricted = [ ]
 *
 *          # Anything not in the above lists gets assigned the
 *          # access.force permission.
 *          #access.force = flatpak
 *      }
 *  }
 *]
 *\endcode
 *
 * \see pw_resource_error
 * \see pw_impl_client_update_permissions
 */

#define NAME "access"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE	"( access.force=flatpak ) "		\
			"( access.allowed= [ <cmd-line>,.. ] ) "	\
			"( access.rejected= [ <cmd-line>,.. ] ) "	\
			"( access.restricted= [ <cmd-line>,.. ] ) "	\

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Perform access check" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_context *context;
	struct pw_properties *properties;

	struct spa_hook context_listener;
	struct spa_hook module_listener;
};

static int get_exe_name(int pid, char *buf, size_t buf_size)
{
	char path[256];
	struct stat s1, s2;
	int res;

	/*
	 * Find executable name, checking it is an existing file
	 * (in the current namespace).
	 */

#if defined(__linux__)
	spa_scnprintf(path, sizeof(path), "/proc/%u/exe", pid);
#elif defined(__FreeBSD__) || defined(__MidnightBSD__)
	spa_scnprintf(path, sizeof(path), "/proc/%u/file", pid);
#else
	return -ENOTSUP;
#endif

	res = readlink(path, buf, buf_size);
	if (res < 0)
		return -errno;
	if ((size_t)res >= buf_size)
		return -E2BIG;
	buf[res] = '\0';

	/* Check the file exists (= not deleted, and is in current namespace) */
	if (stat(path, &s1) != 0 || stat(buf, &s2) != 0)
		return -errno;
	if (s1.st_dev != s2.st_dev || s1.st_ino != s2.st_ino)
		return -ENXIO;

	return 0;
}

static int check_exe(struct pw_impl_client *client, const char *path, const char *str)
{
	char key[1024];
	int res;
	struct spa_json it[2];

	spa_json_init(&it[0], str, strlen(str));
	if ((res = spa_json_enter_array(&it[0], &it[1])) <= 0)
		return res;

	while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
		if (spa_streq(path, key))
			return 1;
	}

	return 0;
}

static void
context_check_access(void *data, struct pw_impl_client *client)
{
	struct impl *impl = data;
	struct pw_permission permissions[1];
	struct spa_dict_item items[2];
	char exe_path[PATH_MAX];
	const struct pw_properties *props;
	const char *str, *access;
	char *flatpak_app_id = NULL;
	int nitems = 0;
	int pid, res;

	pid = -EINVAL;
	if ((props = pw_impl_client_get_properties(client)) != NULL) {
		if ((str = pw_properties_get(props, PW_KEY_ACCESS)) != NULL) {
			pw_log_info("client %p: has already access: '%s'", client, str);
			return;
		}
		 pw_properties_fetch_int32(props, PW_KEY_SEC_PID, &pid);
	}

	if (pid < 0) {
		pw_log_info("client %p: no trusted pid found, assuming not sandboxed", client);
		access = "no-pid";
		goto granted;
	} else {
		pw_log_info("client %p has trusted pid %d", client, pid);
		if ((res = get_exe_name(pid, exe_path, sizeof(exe_path))) >= 0) {
			pw_log_info("client %p has trusted exe path '%s'", client, exe_path);
		} else {
			pw_log_info("client %p has no trusted exe path: %s",
					client, spa_strerror(res));
			exe_path[0] = '\0';
		}
	}

	if (impl->properties && (str = pw_properties_get(impl->properties, "access.allowed")) != NULL) {
		res = check_exe(client, exe_path, str);
		if (res < 0) {
			pw_log_warn("%p: client %p allowed check failed: %s",
				impl, client, spa_strerror(res));
		} else if (res > 0) {
			access = "allowed";
			goto granted;
		}
	}

	if (impl->properties && (str = pw_properties_get(impl->properties, "access.rejected")) != NULL) {
		res = check_exe(client, exe_path, str);
		if (res < 0) {
			pw_log_warn("%p: client %p rejected check failed: %s",
				impl, client, spa_strerror(res));
		} else if (res > 0) {
			res = -EACCES;
			access = "rejected";
			goto rejected;
		}
	}

	if (impl->properties && (str = pw_properties_get(impl->properties, "access.restricted")) != NULL) {
		res = check_exe(client, exe_path, str);
		if (res < 0) {
			pw_log_warn("%p: client %p restricted check failed: %s",
				impl, client, spa_strerror(res));
		}
		else if (res > 0) {
			pw_log_debug(" %p: restricted client %p added", impl, client);
			access = "restricted";
			goto wait_permissions;
		}
	}
	if (impl->properties &&
	    (access = pw_properties_get(impl->properties, "access.force")) != NULL)
		goto wait_permissions;

	res = pw_check_flatpak(pid, &flatpak_app_id, NULL);
	if (res != 0) {
		if (res < 0) {
			if (res == -EACCES) {
				access = "unrestricted";
				goto granted;
			}
			pw_log_warn("%p: client %p sandbox check failed: %s",
				impl, client, spa_strerror(res));
		}
		else if (res > 0) {
			pw_log_debug(" %p: flatpak client %p added", impl, client);
		}
		access = "flatpak";
		items[nitems++] = SPA_DICT_ITEM_INIT("pipewire.access.portal.app_id",
				flatpak_app_id);
		goto wait_permissions;
	}

	if ((access = pw_properties_get(props, PW_KEY_CLIENT_ACCESS)) == NULL)
		access = "unrestricted";

	if (spa_streq(access, "unrestricted") || spa_streq(access, "allowed"))
		goto granted;
	else
		goto wait_permissions;

granted:
	pw_log_info("%p: client %p '%s' access granted", impl, client, access);
	items[nitems++] = SPA_DICT_ITEM_INIT(PW_KEY_ACCESS, access);
	pw_impl_client_update_properties(client, &SPA_DICT_INIT(items, nitems));

	permissions[0] = PW_PERMISSION_INIT(PW_ID_ANY, PW_PERM_ALL);
	pw_impl_client_update_permissions(client, 1, permissions);
	goto done;

wait_permissions:
	pw_log_info("%p: client %p wait for '%s' permissions",
			impl, client, access);
	items[nitems++] = SPA_DICT_ITEM_INIT(PW_KEY_ACCESS, access);
	pw_impl_client_update_properties(client, &SPA_DICT_INIT(items, nitems));
	goto done;

rejected:
	pw_resource_error(pw_impl_client_get_core_resource(client), res, access);
	items[nitems++] = SPA_DICT_ITEM_INIT(PW_KEY_ACCESS, access);
	pw_impl_client_update_properties(client, &SPA_DICT_INIT(items, nitems));
	goto done;

done:
	free(flatpak_app_id);
	return;
}

static const struct pw_context_events context_events = {
	PW_VERSION_CONTEXT_EVENTS,
	.check_access = context_check_access,
};

static void module_destroy(void *data)
{
	struct impl *impl = data;

	spa_hook_remove(&impl->context_listener);
	spa_hook_remove(&impl->module_listener);

	pw_properties_free(impl->properties);

	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props;
	struct impl *impl;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args)
		props = pw_properties_new_string(args);
	else
		props = NULL;

	impl->context = context;
	impl->properties = props;

	pw_context_add_listener(context, &impl->context_listener, &context_events, impl);
	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
}
