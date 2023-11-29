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
#include <pipewire/cleanup.h>

#include "flatpak-utils.h"

/** \page page_module_access Access
 *
 *
 * The `access` module performs access checks on clients. The access check
 * is only performed once per client, subsequent checks return the same
 * resolution.
 *
 * Permissions assigned to a client are configured as arguments to this
 * module, see below. Permission management beyond unrestricted access
 * is delegated to an external agent, usually the session manager.
 *
 * This module sets the \ref PW_KEY_ACCESS as follows:
 *
 * - If `access.legacy` module option is not enabled:

 *   The value defined for the socket in `access.socket` module option, or
 *   `"default"` if no value is defined.
 *
 * - If `access.legacy` is enabled, the value is:
 *
 *     - `"flatpak"`: if client is a Flatpak client
 *     - `"unrestricted"`: if \ref PW_KEY_CLIENT_ACCESS client property is set to `"allowed"`
 *     - Value of \ref PW_KEY_CLIENT_ACCESS client property, if set
 *     - `"unrestricted"`: otherwise
 *
 * If the resulting \ref PW_KEY_ACCESS value is `"unrestricted"`, this module
 * will give the client all permissions to access all resources.  Otherwise, the
 * client will be forced to wait until an external actor, such as the session
 * manager, updates the client permissions.
 *
 * For connections from applications running inside Flatpak, and not mediated by
 * other clients (eg. portal or pipewire-pulse), the
 * `pipewire.access.portal.app_id` property is to the Flatpak application ID, if
 * found. In addition, `pipewire.sec.flatpak` is set to `true`.
 *
 * ## Module Name
 *
 * `libpipewire-module-access`
 *
 * ## Module Options
 *
 * Options specific to the behavior of this module
 *
 * - `access.socket = { "socket-name" = "access-value", ... }`:
 *
 *   Socket-specific access permissions. Has the default value
 *   `{ "CORENAME-manager": "unrestricted" }`
 *   where `CORENAME` is the name of the PipeWire core, usually `pipewire-0`.
 *
 * - `access.legacy = true`: enable backward-compatible access mode.  Cannot be
 *   enabled when using socket-based permissions.
 *
 *   If `access.socket` is not specified, has the default value `true`
 *   otherwise `false`.
 *
 *   \warning The legacy mode is deprecated. The default value is subject to
 *            change and the legacy mode may be removed in future PipeWire
 *            releases.
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
 *          # Use separate socket for session manager applications,
 *          # and pipewire-0 for usual applications.
 *          access.socket = {
 *              pipewire-0 = "default",
 *              pipewire-0-manager = "unrestricted",
 *          }
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

#define MODULE_USAGE	"( access.socket={ <socket>=<access>, ... } ) " \
			"( access.legacy=true ) "

#define ACCESS_UNRESTRICTED	"unrestricted"
#define ACCESS_FLATPAK		"flatpak"
#define ACCESS_DEFAULT		"default"

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Perform access check" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_context *context;

	struct pw_properties *socket_access;

	struct spa_hook context_listener;
	struct spa_hook module_listener;

	unsigned int legacy:1;
};

static void
context_check_access(void *data, struct pw_impl_client *client)
{
	struct impl *impl = data;
	struct pw_permission permissions[1];
	struct spa_dict_item items[3];
	const struct pw_properties *props;
	const char *str;
	const char *access;
	const char *socket;
	spa_autofree char *flatpak_app_id = NULL;
	int nitems = 0;
	bool sandbox_flatpak;
	int pid, res;

	/* Get client properties */

	pid = -EINVAL;
	socket = NULL;
	sandbox_flatpak = false;

	if ((props = pw_impl_client_get_properties(client)) != NULL) {
		if ((str = pw_properties_get(props, PW_KEY_ACCESS)) != NULL) {
			pw_log_info("client %p: has already access: '%s'", client, str);
			return;
		}
		 pw_properties_fetch_int32(props, PW_KEY_SEC_PID, &pid);
		socket = pw_properties_get(props, PW_KEY_SEC_SOCKET);
	}

	if (pid < 0) {
		pw_log_info("client %p: no trusted pid found, assuming not sandboxed", client);
	} else {
		pw_log_info("client %p has trusted pid %d", client, pid);

		res = pw_check_flatpak(pid, &flatpak_app_id, NULL);
		if (res != 0) {
			if (res < 0)
				pw_log_warn("%p: client %p flatpak check failed: %s",
						impl, client, spa_strerror(res));

			pw_log_info("client %p is from flatpak", client);
			sandbox_flatpak = true;
		}
	}

	/* Apply rules */

	if (!impl->legacy) {
		if ((str = pw_properties_get(impl->socket_access, socket)) != NULL)
			access = str;
		else
			access = ACCESS_DEFAULT;
	} else {
		if (sandbox_flatpak) {
			access = ACCESS_FLATPAK;
		} else if ((str = pw_properties_get(props, PW_KEY_CLIENT_ACCESS)) != NULL) {
			if (spa_streq(str, "allowed"))
				access = ACCESS_UNRESTRICTED;
			else
				access = str;
		} else {
			access = ACCESS_UNRESTRICTED;
		}
	}

	/* Handle resolution */

	if (sandbox_flatpak) {
		items[nitems++] = SPA_DICT_ITEM_INIT("pipewire.access.portal.app_id",
				flatpak_app_id);
		items[nitems++] = SPA_DICT_ITEM_INIT("pipewire.sec.flatpak", "true");
	}

	if (spa_streq(access, ACCESS_UNRESTRICTED)) {
		pw_log_info("%p: client %p '%s' access granted", impl, client, access);
		items[nitems++] = SPA_DICT_ITEM_INIT(PW_KEY_ACCESS, access);
		pw_impl_client_update_properties(client, &SPA_DICT_INIT(items, nitems));

		permissions[0] = PW_PERMISSION_INIT(PW_ID_ANY, PW_PERM_ALL);
		pw_impl_client_update_permissions(client, 1, permissions);
	} else {
		pw_log_info("%p: client %p wait for '%s' permissions",
				impl, client, access);
		items[nitems++] = SPA_DICT_ITEM_INIT(PW_KEY_ACCESS, access);
		pw_impl_client_update_properties(client, &SPA_DICT_INIT(items, nitems));
	}
}

static const struct pw_context_events context_events = {
	PW_VERSION_CONTEXT_EVENTS,
	.check_access = context_check_access,
};

static void module_destroy(void *data)
{
	struct impl *impl = data;

	if (impl->context) {
		spa_hook_remove(&impl->context_listener);
		spa_hook_remove(&impl->module_listener);
	}

	pw_properties_free(impl->socket_access);

	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static const char *
get_server_name(const struct spa_dict *props)
{
	const char *name = NULL;

	name = getenv("PIPEWIRE_CORE");
	if (name == NULL && props != NULL)
		name = spa_dict_lookup(props, PW_KEY_CORE_NAME);
	if (name == NULL)
		name = PW_DEFAULT_REMOTE;
	return name;
}

static int parse_socket_args(struct impl *impl, const char *str)
{
	struct spa_json it[2];
	char socket[PATH_MAX];

	spa_json_init(&it[0], str, strlen(str));

	if (spa_json_enter_object(&it[0], &it[1]) <= 0)
		return -EINVAL;

	while (spa_json_get_string(&it[1], socket, sizeof(socket)) > 0) {
		char value[256];
		const char *val;
		int len;

		if ((len = spa_json_next(&it[1], &val)) <= 0)
			return -EINVAL;

		if (spa_json_parse_stringn(val, len, value, sizeof(value)) <= 0)
			return -EINVAL;

		pw_properties_set(impl->socket_access, socket, value);
	}

	return 0;
}

static int parse_args(struct impl *impl, const struct pw_properties *props, const char *args_str)
{
	spa_autoptr(pw_properties) args = NULL;
	const char *str;
	int res;

	if (args_str)
		args = pw_properties_new_string(args_str);
	else
		args = pw_properties_new(NULL, NULL);

	if ((str = pw_properties_get(args, "access.legacy")) != NULL) {
		impl->legacy = spa_atob(str);
	} else if (pw_properties_get(args, "access.socket")) {
		impl->legacy = false;
	} else {
		/* When time comes, we should change this to false */
		impl->legacy = true;
	}

	if (pw_properties_get(args, "access.force") ||
			pw_properties_get(args, "access.allowed") ||
			pw_properties_get(args, "access.rejected") ||
			pw_properties_get(args, "access.restricted")) {
		pw_log_warn("access.force/allowed/rejected/restricted are deprecated and ignored "
				"but imply legacy access mode");
		impl->legacy = true;
	}

	if ((str = pw_properties_get(args, "access.socket")) != NULL) {
		if (impl->legacy) {
			pw_log_error("access.socket and legacy mode cannot be both enabled");
			return -EINVAL;
		}

		if ((res = parse_socket_args(impl, str)) < 0) {
			pw_log_error("invalid access.socket value");
			return res;
		}
	} else {
		char def[PATH_MAX];

		spa_scnprintf(def, sizeof(def), "%s-manager", get_server_name(&props->dict));
		pw_properties_set(impl->socket_access, def, ACCESS_UNRESTRICTED);
	}

	if (impl->legacy)
		pw_log_info("Using backward-compatible legacy access mode.");

	return 0;
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	const struct pw_properties *props = pw_context_get_properties(context);
	struct impl *impl;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);

	impl->socket_access = pw_properties_new(NULL, NULL);

	if ((res = parse_args(impl, props, args)) < 0)
		goto error;

	impl->context = context;

	pw_context_add_listener(context, &impl->context_listener, &context_events, impl);
	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	module_destroy(impl);
	return res;
}
