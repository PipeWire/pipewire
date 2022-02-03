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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

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
#include <pipewire/private.h>

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

#define MODULE_USAGE	"[ access.force=flatpak ] "		\
			"[ access.allowed=<cmd-line> ] "	\
			"[ access.rejected=<cmd-line> ] "	\
			"[ access.restricted=<cmd-line> ] "	\

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

static int check_cmdline(struct pw_impl_client *client, int pid, const char *str)
{
	char path[2048], key[1024];
	ssize_t len;
	int fd, res;
	struct spa_json it[2];

	sprintf(path, "/proc/%u/cmdline", pid);

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		res = -errno;
		goto exit;
	}
	if ((len = read(fd, path, sizeof(path)-1)) < 0) {
		res = -errno;
		goto exit_close;
	}
	path[len] = '\0';

	spa_json_init(&it[0], str, strlen(str));
	if ((res = spa_json_enter_array(&it[0], &it[1])) <= 0)
		goto exit_close;

	while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
		if (spa_streq(path, key)) {
			res = 1;
			goto exit_close;
		}
	}
	res = 0;
exit_close:
	close(fd);
exit:
	return res;
}

#if defined(__linux__)
static int check_flatpak(struct pw_impl_client *client, int pid)
{
	char root_path[2048];
	int root_fd, info_fd, res;
	struct stat stat_buf;

	sprintf(root_path, "/proc/%u/root", pid);
	root_fd = openat (AT_FDCWD, root_path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
	if (root_fd == -1) {
		res = -errno;
		if (res == -EACCES) {
			struct statfs buf;
			/* Access to the root dir isn't allowed. This can happen if the root is on a fuse
			 * filesystem, such as in a toolbox container. We will never have a fuse rootfs
			 * in the flatpak case, so in that case its safe to ignore this and
			 * continue to detect other types of apps. */
			if (statfs(root_path, &buf) == 0 &&
			    buf.f_type == 0x65735546) /* FUSE_SUPER_MAGIC */
				return 0;
		}
		/* Not able to open the root dir shouldn't happen. Probably the app died and
		 * we're failing due to /proc/$pid not existing. In that case fail instead
		 * of treating this as privileged. */
		pw_log_info("failed to open \"%s\": %s", root_path, spa_strerror(res));
		return res;
	}
	info_fd = openat (root_fd, ".flatpak-info", O_RDONLY | O_CLOEXEC | O_NOCTTY);
	close (root_fd);
	if (info_fd == -1) {
		if (errno == ENOENT) {
			pw_log_debug("no .flatpak-info, client on the host");
			/* No file => on the host */
			return 0;
		}
		res = -errno;
		pw_log_error("error opening .flatpak-info: %m");
		return res;
        }
	if (fstat (info_fd, &stat_buf) != 0 || !S_ISREG (stat_buf.st_mode)) {
		/* Some weird fd => failure, assume sandboxed */
		pw_log_error("error fstat .flatpak-info: %m");
	}
	close(info_fd);
	return 1;
}
#endif

static void
context_check_access(void *data, struct pw_impl_client *client)
{
	struct impl *impl = data;
	struct pw_permission permissions[1];
	struct spa_dict_item items[2];
	const struct pw_properties *props;
	const char *str, *access;
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
	}

	if (impl->properties && (str = pw_properties_get(impl->properties, "access.allowed")) != NULL) {
		res = check_cmdline(client, pid, str);
		if (res < 0) {
			pw_log_warn("%p: client %p allowed check failed: %s",
				impl, client, spa_strerror(res));
		} else if (res > 0) {
			access = "allowed";
			goto granted;
		}
	}

	if (impl->properties && (str = pw_properties_get(impl->properties, "access.rejected")) != NULL) {
		res = check_cmdline(client, pid, str);
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
		res = check_cmdline(client, pid, str);
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

#if defined(__linux__)
	res = check_flatpak(client, pid);
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
		goto wait_permissions;
	}
#endif
	if ((access = pw_properties_get(props, PW_KEY_CLIENT_ACCESS)) == NULL)
		access = "unrestricted";

	if (spa_streq(access, "unrestricted") || spa_streq(access, "allowed"))
		goto granted;
	else
		goto wait_permissions;

granted:
	pw_log_info("%p: client %p '%s' access granted", impl, client, access);
	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_ACCESS, access);
	pw_impl_client_update_properties(client, &SPA_DICT_INIT(items, 1));

	permissions[0] = PW_PERMISSION_INIT(PW_ID_ANY, PW_PERM_ALL);
	pw_impl_client_update_permissions(client, 1, permissions);
	return;

wait_permissions:
	pw_log_info("%p: client %p wait for '%s' permissions",
			impl, client, access);
	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_ACCESS, access);
	pw_impl_client_update_properties(client, &SPA_DICT_INIT(items, 1));
	return;

rejected:
	pw_resource_error(pw_impl_client_get_core_resource(client), res, access);
	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_ACCESS, access);
	pw_impl_client_update_properties(client, &SPA_DICT_INIT(items, 1));
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
