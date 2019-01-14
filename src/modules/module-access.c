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

#include <pipewire/pipewire.h>

static const struct spa_dict_item module_props[] = {
	{ PW_MODULE_PROP_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_MODULE_PROP_DESCRIPTION, "Perform access check" },
	{ PW_MODULE_PROP_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_core *core;
	struct pw_properties *properties;

	struct spa_hook core_listener;
	struct spa_hook module_listener;
};

static int check_cmdline(struct pw_client *client, const struct ucred *ucred, const char *str)
{
	char path[2048];
	int fd;

	sprintf(path, "/proc/%u/cmdline", ucred->pid);

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;

	if (read(fd, path, 1024) <= 0)
		return -EIO;

	if (strcmp(path, str) == 0)
		return 1;

	return 0;
}

static int check_flatpak(struct pw_client *client, const struct ucred *ucred)
{
	char root_path[2048];
	int root_fd, info_fd, res;
	struct stat stat_buf;

	sprintf(root_path, "/proc/%u/root", ucred->pid);
	root_fd = openat (AT_FDCWD, root_path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
	if (root_fd == -1) {
		/* Not able to open the root dir shouldn't happen. Probably the app died and
		 * we're failing due to /proc/$pid not existing. In that case fail instead
		 * of treating this as privileged. */
		res = -errno;
		pw_log_error("failed to open \"%s\": %m", root_path);
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
		close(info_fd);
		pw_log_error("error fstat .flatpak-info: %m");
	}
	return 1;
}

static void
core_check_access(void *data, struct pw_client *client)
{
	struct impl *impl = data;
	const struct ucred *ucred;
	struct pw_permission permissions[1];
	struct spa_dict_item items[2];
	const char *str;
	int res;

	ucred = pw_client_get_ucred(client);
	if (!ucred) {
		pw_log_info("no trusted pid found, assuming not sandboxed\n");
		goto granted;
	} else {
		pw_log_info("client has trusted pid %d", ucred->pid);
	}


	if (impl->properties && (str = pw_properties_get(impl->properties, "blacklisted")) != NULL) {
		res = check_cmdline(client, ucred, str);
		if (res == 0)
			goto granted;
		if (res > 0)
			res = -EACCES;
		items[0] = SPA_DICT_ITEM_INIT("pipewire.access", "blacklisted");
		goto blacklisted;
	}

	if (impl->properties && (str = pw_properties_get(impl->properties, "restricted")) != NULL) {
		res = check_cmdline(client, ucred, str);
		if (res == 0)
			goto granted;
		if (res < 0) {
			pw_log_warn("module %p: client %p restricted check failed: %s",
				impl, client, spa_strerror(res));
		}
		else if (res > 0) {
			pw_log_debug("module %p: restricted client %p added", impl, client);
		}
		items[0] = SPA_DICT_ITEM_INIT("pipewire.access", "restricted");
		goto wait_permissions;
	}

	res = check_flatpak(client, ucred);
	if (res != 0) {
		if (res < 0) {
			pw_log_warn("module %p: client %p sandbox check failed: %s",
				impl, client, spa_strerror(res));
		}
		else if (res > 0) {
			pw_log_debug("module %p: sandboxed client %p added", impl, client);
		}
		items[0] = SPA_DICT_ITEM_INIT("pipewire.access", "flatpak");
		goto wait_permissions;
	}

      granted:
	pw_log_debug("module %p: client %p access granted", impl, client);
	permissions[0] = PW_PERMISSION_INIT(-1, PW_PERM_RWX);
	pw_client_update_permissions(client, 1, permissions);
	return;

      wait_permissions:
	pw_log_debug("module %p: client %p wait for permissions", impl, client);
	pw_client_update_properties(client, &SPA_DICT_INIT(items, 1));
	pw_client_set_busy(client, true);
	return;

      blacklisted:
	pw_resource_error(pw_client_get_core_resource(client), 0, res, "blacklisted");
	pw_client_update_properties(client, &SPA_DICT_INIT(items, 1));
	return;


}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.check_access = core_check_access,
};

static void module_destroy(void *data)
{
	struct impl *impl = data;

	spa_hook_remove(&impl->core_listener);
	spa_hook_remove(&impl->module_listener);

	if (impl->properties)
		pw_properties_free(impl->properties);

	free(impl);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.destroy = module_destroy,
};

int pipewire__module_init(struct pw_module *module, const char *args)
{
	struct pw_core *core = pw_module_get_core(module);
	struct pw_properties *props;
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

	pw_log_debug("module %p: new %s", impl, args);

	if (args)
		props = pw_properties_new_string(args);
	else
		props = NULL;

	impl->core = core;
	impl->properties = props;

	pw_core_add_listener(core, &impl->core_listener, &core_events, impl);
	pw_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
}
