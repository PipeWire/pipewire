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

#include <spa/utils/result.h>

#include <pipewire/impl.h>

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Perform access check" },
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
	char path[2048];
	int fd;

	sprintf(path, "/proc/%u/cmdline", pid);

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return -errno;

	if (read(fd, path, 1024) <= 0) {
		close(fd);
		return -EIO;
	}

	if (strcmp(path, str) == 0) {
		close(fd);
		return 1;
	}

	close(fd);
	return 0;
}

static int check_flatpak(struct pw_impl_client *client, int pid)
{
	char root_path[2048];
	int root_fd, info_fd, res;
	struct stat stat_buf;

	sprintf(root_path, "/proc/%u/root", pid);
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
context_check_access(void *data, struct pw_impl_client *client)
{
	struct impl *impl = data;
	struct pw_permission permissions[1];
	struct spa_dict_item items[2];
	const struct pw_properties *props;
	const char *str;
	int pid, res;

	pid = -EINVAL;
	if ((props = pw_impl_client_get_properties(client)) != NULL) {
		if ((str = pw_properties_get(props, PW_KEY_SEC_PID)) != NULL)
			pid = atoi(str);
	}

	if (pid < 0) {
		pw_log_info("client %p: no trusted pid found, assuming not sandboxed", client);
		goto granted;
	} else {
		pw_log_info("client %p has trusted pid %d", client, pid);
	}

	if (impl->properties && (str = pw_properties_get(impl->properties, "blacklisted")) != NULL) {
		res = check_cmdline(client, pid, str);
		if (res == 0)
			goto granted;
		if (res > 0)
			res = -EACCES;
		items[0] = SPA_DICT_ITEM_INIT(PW_KEY_ACCESS, "blacklisted");
		goto blacklisted;
	}

	if (impl->properties && (str = pw_properties_get(impl->properties, "restricted")) != NULL) {
		res = check_cmdline(client, pid, str);
		if (res == 0)
			goto granted;
		if (res < 0) {
			pw_log_warn("module %p: client %p restricted check failed: %s",
				impl, client, spa_strerror(res));
		}
		else if (res > 0) {
			pw_log_debug("module %p: restricted client %p added", impl, client);
		}
		items[0] = SPA_DICT_ITEM_INIT(PW_KEY_ACCESS, "restricted");
		goto wait_permissions;
	}

	res = check_flatpak(client, pid);
	if (res != 0) {
		if (res < 0) {
			pw_log_warn("module %p: client %p sandbox check failed: %s",
				impl, client, spa_strerror(res));
			if (res == -EACCES)
				goto granted;
		}
		else if (res > 0) {
			pw_log_debug("module %p: sandboxed client %p added", impl, client);
		}
		items[0] = SPA_DICT_ITEM_INIT(PW_KEY_ACCESS, "flatpak");
		goto wait_permissions;
	}

granted:
	pw_log_debug("module %p: client %p access granted", impl, client);
	permissions[0] = PW_PERMISSION_INIT(PW_ID_ANY, PW_PERM_RWX);
	pw_impl_client_update_permissions(client, 1, permissions);
	return;

wait_permissions:
	pw_log_debug("module %p: client %p wait for permissions", impl, client);
	pw_impl_client_update_properties(client, &SPA_DICT_INIT(items, 1));
	pw_impl_client_set_busy(client, true);
	return;

blacklisted:
	pw_resource_error(pw_impl_client_get_core_resource(client), res, "blacklisted");
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

	if (impl->properties)
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
