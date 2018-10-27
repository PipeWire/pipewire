/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "config.h"

#include "pipewire/core.h"
#include "pipewire/interfaces.h"
#include "pipewire/link.h"
#include "pipewire/log.h"
#include "pipewire/module.h"
#include "pipewire/utils.h"

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

static int check_sandboxed(struct pw_client *client)
{
	char root_path[2048];
	int root_fd, info_fd, res;
	const struct ucred *ucred;
	struct stat stat_buf;

	ucred = pw_client_get_ucred(client);

	if (ucred) {
		pw_log_info("client has trusted pid %d", ucred->pid);
	} else {
		pw_log_info("no trusted pid found, assuming not sandboxed\n");
		return 0;
	}

	if (check_cmdline(client, ucred, "paplay"))
		return 1;

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
core_global_added(void *data, struct pw_global *global)
{
	struct impl *impl = data;
	struct pw_client *client;
	int res;

	if (pw_global_get_type(global) != PW_TYPE_INTERFACE_Client)
		return;

	client = pw_global_get_object(global);

	res = check_sandboxed(client);
	if (res == 0) {
		pw_log_debug("module %p: non sandboxed client %p", impl, client);
		pw_client_set_permissions(client, PW_PERM_RWX);
		return;
	}

	if (res < 0) {
		pw_log_warn("module %p: client %p sandbox check failed: %s",
				impl, client, spa_strerror(res));
	}
	else {
		pw_log_debug("module %p: sandboxed client %p added", impl, client);
	}
	pw_client_set_busy(client, true);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.global_added = core_global_added,
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

static int module_init(struct pw_module *module, struct pw_properties *properties)
{
	struct pw_core *core = pw_module_get_core(module);
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

	pw_log_debug("module %p: new", impl);

	impl->core = core;
	impl->properties = properties;

	pw_core_add_listener(core, &impl->core_listener, &core_events, impl);
	pw_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
}

int pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
