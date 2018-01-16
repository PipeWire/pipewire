/* PipeWire
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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

#include <spa/support/dbus.h>

#include "pipewire/core.h"
#include "pipewire/interfaces.h"
#include "pipewire/link.h"
#include "pipewire/log.h"
#include "pipewire/module.h"
#include "pipewire/utils.h"

struct impl {
	struct pw_core *core;
	struct pw_type *type;
	struct pw_properties *properties;

	struct spa_dbus_connection *conn;
	DBusConnection *bus;

	struct spa_hook core_listener;
	struct spa_hook module_listener;

	struct spa_list client_list;
};

struct resource;

struct client_info {
	struct spa_list link;
	struct impl *impl;
	struct pw_client *client;
	struct spa_hook client_listener;
	bool is_sandboxed;
        struct spa_list resources;
	struct resource *core_resource;
	struct spa_list async_pending;
};

struct resource {
        struct spa_list link;
	struct client_info *cinfo;
	struct pw_resource *resource;
	struct spa_hook override;
};

struct async_pending {
	struct spa_list link;
	struct resource *resource;
	bool handled;
	char *handle;
	char *factory_name;
	uint32_t type;
	uint32_t version;
	struct pw_properties *properties;
	uint32_t new_id;
};

static struct client_info *find_client_info(struct impl *impl, struct pw_client *client)
{
	struct client_info *info;

	spa_list_for_each(info, &impl->client_list, link) {
		if (info->client == client)
			return info;
	}
	return NULL;
}

static void close_request(struct async_pending *p)
{
	DBusMessage *m = NULL;
	struct impl *impl = p->resource->cinfo->impl;

	pw_log_debug("pending %p: handle %s", p, p->handle);

	if (!(m = dbus_message_new_method_call("org.freedesktop.portal.Request",
					       p->handle,
					       "org.freedesktop.portal.Request", "Close"))) {
		pw_log_error("Failed to create message");
		return;
	}

	if (!dbus_connection_send(impl->bus, m, NULL))
		pw_log_error("Failed to send message");

	dbus_message_unref(m);
}

static struct async_pending *find_pending(struct client_info *cinfo, const char *handle)
{
	struct async_pending *p;

	spa_list_for_each(p, &cinfo->async_pending, link) {
		if (strcmp(p->handle, handle) == 0)
			return p;
	}
	return NULL;
}

static void free_pending(struct async_pending *p)
{
	if (!p->handled)
		close_request(p);

	pw_log_debug("pending %p: handle %s", p, p->handle);
	spa_list_remove(&p->link);
	free(p->handle);
	free(p->factory_name);
	if (p->properties)
		pw_properties_free(p->properties);
	free(p);
}

static void free_resource(struct resource *r)
{
	spa_list_remove(&r->link);
	free(r);
}

static void client_info_free(struct client_info *cinfo)
{
	struct async_pending *p, *tp;
	struct resource *r, *tr;

	spa_list_for_each_safe(p, tp, &cinfo->async_pending, link)
		free_pending(p);
	spa_list_for_each_safe(r, tr, &cinfo->resources, link)
		free_resource(r);

	spa_hook_remove(&cinfo->client_listener);
	spa_list_remove(&cinfo->link);
	free(cinfo);
}

static bool check_sandboxed(struct client_info *cinfo, char **error)
{
	char root_path[2048];
	int root_fd, info_fd;
	const struct ucred *ucred;
	struct stat stat_buf;

	ucred = pw_client_get_ucred(cinfo->client);

	cinfo->is_sandboxed = true;

	if (ucred) {
		pw_log_info("client has trusted pid %d", ucred->pid);
	} else {
		cinfo->is_sandboxed = false;
		pw_log_info("no trusted pid found, assuming not sandboxed\n");
		return true;
	}

	sprintf(root_path, "/proc/%u/root", ucred->pid);
	root_fd = openat (AT_FDCWD, root_path, O_RDONLY | O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOCTTY);
	if (root_fd == -1) {
		/* Not able to open the root dir shouldn't happen. Probably the app died and
		 * we're failing due to /proc/$pid not existing. In that case fail instead
		 * of treating this as privileged. */
		asprintf(error, "failed to open \"%s\": %m", root_path);
		return false;
	}
	info_fd = openat (root_fd, ".flatpak-info", O_RDONLY | O_CLOEXEC | O_NOCTTY);
	close (root_fd);
	if (info_fd == -1) {
		if (errno == ENOENT) {
			pw_log_debug("no .flatpak-info, client on the host");
			cinfo->is_sandboxed = false;
			/* No file => on the host */
			return true;
		}
		asprintf(error, "error opening .flatpak-info: %m");
		return false;
        }
	if (fstat (info_fd, &stat_buf) != 0 || !S_ISREG (stat_buf.st_mode)) {
		/* Some weird fd => failure */
		close(info_fd);
		asprintf(error, "error fstat .flatpak-info: %m");
		return false;
	}
	return true;
}

static bool
check_global_owner(struct pw_core *core, struct pw_client *client, struct pw_global *global)
{
	struct pw_client *owner;
	const struct ucred *owner_ucred, *client_ucred;

	if (global == NULL)
		return false;

	owner = pw_global_get_owner(global);
	if (owner == NULL)
		return true;

	owner_ucred = pw_client_get_ucred(owner);
	client_ucred = pw_client_get_ucred(client);

	if (owner_ucred == NULL || client_ucred == NULL)
		return true;

	return owner_ucred->uid == client_ucred->uid;
}

static uint32_t
do_permission(struct pw_global *global, struct pw_client *client, void *data)
{
	struct impl *impl = data;

	if (pw_global_get_type(global) == impl->type->link) {
		struct pw_link *link = pw_global_get_object(global);
		struct pw_port *port;
		struct pw_node *node;

		port = pw_link_get_output(link);
		node = pw_port_get_node(port);
		/* we must be able to see both nodes */
		if (port && node && !check_global_owner(impl->core, client, pw_node_get_global(node)))
			 return 0;

		port = pw_link_get_input(link);
		node = pw_port_get_node(port);
		if (port && node && !check_global_owner(impl->core, client, pw_node_get_global(node)))
			 return 0;
	}
	else if (!check_global_owner(impl->core, client, global))
		return 0;

	return PW_PERM_RWX;
}

static DBusHandlerResult
portal_response(DBusConnection *connection, DBusMessage *msg, void *user_data)
{
	struct client_info *cinfo = user_data;

	if (dbus_message_is_signal(msg, "org.freedesktop.portal.Request", "Response")) {
		uint32_t response = 2;
		DBusError error;
		struct async_pending *p;

		dbus_error_init(&error);

		dbus_connection_remove_filter(connection, portal_response, cinfo);

		if (!dbus_message_get_args
		    (msg, &error, DBUS_TYPE_UINT32, &response, DBUS_TYPE_INVALID)) {
			pw_log_error("failed to parse Response: %s", error.message);
			dbus_error_free(&error);
		}

		p = find_pending(cinfo, dbus_message_get_path(msg));
		if (p == NULL)
			return DBUS_HANDLER_RESULT_HANDLED;

		p->handled = true;

		pw_log_debug("portal check result: %d", response);

		if (response == 0) {
			pw_resource_do_parent(p->resource->resource,
					      &p->resource->override,
					      struct pw_core_proxy_methods,
					      create_object,
					      p->factory_name,
					      p->type,
					      p->version,
					      &p->properties->dict,
					      p->new_id);
		} else {
			pw_resource_error(p->resource->resource, -EPERM, "not allowed");

		}
		free_pending(p);
		pw_client_set_busy(cinfo->client, false);

		return DBUS_HANDLER_RESULT_HANDLED;
	}
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}


static void do_create_object(void *data,
			     const char *factory_name,
			     uint32_t type,
			     uint32_t version,
			     const struct spa_dict *props,
			     uint32_t new_id)
{
	struct resource *resource = data;
	struct client_info *cinfo = resource->cinfo;
	struct impl *impl = cinfo->impl;
	struct pw_client *client = cinfo->client;
	DBusMessage *m = NULL, *r = NULL;
	DBusError error;
	pid_t pid;
	DBusMessageIter msg_iter;
	DBusMessageIter dict_iter;
	const char *handle;
	const char *device;
	struct async_pending *p;

	if (!cinfo->is_sandboxed) {
		pw_resource_do_parent(resource->resource,
				      &resource->override,
				      struct pw_core_proxy_methods,
				      create_object,
				      factory_name,
				      type,
				      version,
				      props,
				      new_id);
		return;
	}
	if (strcmp(factory_name, "client-node") != 0) {
		pw_log_error("can only allow client-node");
		goto not_allowed;
	}

	pw_log_info("ask portal for client %p", cinfo->client);
	pw_client_set_busy(client, true);

	dbus_error_init(&error);

	if (!(m = dbus_message_new_method_call("org.freedesktop.portal.Desktop",
					       "/org/freedesktop/portal/desktop",
					       "org.freedesktop.portal.Device", "AccessDevice")))
		goto no_method_call;

	device = "camera";

	pid = pw_client_get_ucred(cinfo->client)->pid;
	if (!dbus_message_append_args(m, DBUS_TYPE_UINT32, &pid, DBUS_TYPE_INVALID))
		goto message_failed;

	dbus_message_iter_init_append(m, &msg_iter);
	dbus_message_iter_open_container(&msg_iter, DBUS_TYPE_ARRAY, "s", &dict_iter);
	dbus_message_iter_append_basic(&dict_iter, DBUS_TYPE_STRING, &device);
	dbus_message_iter_close_container(&msg_iter, &dict_iter);

	dbus_message_iter_open_container(&msg_iter, DBUS_TYPE_ARRAY, "{sv}", &dict_iter);
	dbus_message_iter_close_container(&msg_iter, &dict_iter);

	if (!(r = dbus_connection_send_with_reply_and_block(impl->bus, m, -1, &error)))
		goto send_failed;

	dbus_message_unref(m);

	if (!dbus_message_get_args(r, &error, DBUS_TYPE_OBJECT_PATH, &handle, DBUS_TYPE_INVALID))
		goto parse_failed;

	dbus_message_unref(r);

	dbus_bus_add_match(impl->bus,
			   "type='signal',interface='org.freedesktop.portal.Request'", &error);
	dbus_connection_flush(impl->bus);
	if (dbus_error_is_set(&error))
		goto subscribe_failed;

	dbus_connection_add_filter(impl->bus, portal_response, cinfo, NULL);

	p = calloc(1, sizeof(struct async_pending));
	p->resource = resource;
	p->handle = strdup(handle);
	p->handled = false;
	p->factory_name = strdup(factory_name);
	p->type = type;
	p->version = version;
	p->properties = props ? pw_properties_new_dict(props) : NULL;
	p->new_id = new_id;

	pw_log_debug("pending %p: handle %s", p, handle);
	spa_list_append(&cinfo->async_pending, &p->link);

	return;

      no_method_call:
	pw_log_error("Failed to create message");
	goto not_allowed;
      message_failed:
	dbus_message_unref(m);
	goto not_allowed;
      send_failed:
	pw_log_error("Failed to call portal: %s", error.message);
	dbus_error_free(&error);
	dbus_message_unref(m);
	goto not_allowed;
      parse_failed:
	pw_log_error("Failed to parse AccessDevice result: %s", error.message);
	dbus_error_free(&error);
	dbus_message_unref(r);
	goto not_allowed;
      subscribe_failed:
	pw_log_error("Failed to subscribe to Request signal: %s", error.message);
	dbus_error_free(&error);
	goto not_allowed;
      not_allowed:
	pw_resource_error(cinfo->core_resource->resource, -EPERM, "not allowed");
	return;
}

static const struct pw_core_proxy_methods core_override = {
	PW_VERSION_CORE_PROXY_METHODS,
	.create_object = do_create_object,
};

static void client_resource_impl(void *data, struct pw_resource *resource)
{
	struct client_info *cinfo = data;
	struct impl *impl = cinfo->impl;

	if (pw_resource_get_type(resource) == impl->type->core) {
		struct resource *r;

		r = calloc(1, sizeof(struct resource));
		r->cinfo = cinfo;
		r->resource = resource;
		spa_list_append(&cinfo->resources, &r->link);

		if (pw_resource_get_id(resource) == 0)
			cinfo->core_resource = r;

		pw_log_debug("module %p: add core override", impl);
		pw_resource_add_override(resource,
					 &r->override,
					 &core_override,
					 r);
	}
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.resource_impl = client_resource_impl,
};

static void
core_global_added(void *data, struct pw_global *global)
{
	struct impl *impl = data;

	if (pw_global_get_type(global) == impl->type->client) {
		struct pw_client *client = pw_global_get_object(global);
		struct client_info *cinfo;
		char *error;

		cinfo = calloc(1, sizeof(struct client_info));
		cinfo->impl = impl;
		cinfo->client = client;
		if (!check_sandboxed(cinfo, &error)) {
			pw_log_warn("module %p: client %p sandbox check failed: %s", impl, client, error);
			free(error);
		}
		spa_list_init(&cinfo->async_pending);
		spa_list_init(&cinfo->resources);

		pw_client_add_listener(client, &cinfo->client_listener, &client_events, cinfo);

		spa_list_append(&impl->client_list, &cinfo->link);

		pw_log_debug("module %p: client %p added", impl, client);
	}
}

static void
core_global_removed(void *data, struct pw_global *global)
{
	struct impl *impl = data;

	if (pw_global_get_type(global) == impl->type->client) {
		struct pw_client *client = pw_global_get_object(global);
		struct client_info *cinfo;

		if ((cinfo = find_client_info(impl, client)))
			client_info_free(cinfo);

		pw_log_debug("module %p: client %p removed", impl, client);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.global_added = core_global_added,
	.global_removed = core_global_removed,
};

static void module_destroy(void *data)
{
	struct impl *impl = data;
	struct client_info *info, *t;

	spa_hook_remove(&impl->core_listener);
	spa_hook_remove(&impl->module_listener);

	spa_dbus_connection_destroy(impl->conn);

	spa_list_for_each_safe(info, t, &impl->client_list, link)
		client_info_free(info);

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
	DBusError error;
	struct spa_dbus *dbus;
	const struct spa_support *support;
	uint32_t n_support;

	support = pw_core_get_support(core, &n_support);

	dbus = spa_support_find(support, n_support, SPA_TYPE__DBus);
        if (dbus == NULL)
                return -ENOTSUP;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

	pw_log_debug("module %p: new", impl);

	impl->core = core;
	impl->type = pw_core_get_type(core);
	impl->properties = properties;

	dbus_error_init(&error);

	impl->conn = spa_dbus_get_connection(dbus, DBUS_BUS_SESSION, &error);
	if (impl->conn == NULL)
		goto error;

	impl->bus = spa_dbus_connection_get(impl->conn);

	spa_list_init(&impl->client_list);

	pw_core_add_listener(core, &impl->core_listener, &core_events, impl);
	pw_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_core_set_permission_callback(core, do_permission, impl);

	return 0;

      error:
	free(impl);
	pw_log_error("Failed to connect to system bus: %s", error.message);
	dbus_error_free(&error);
	return -ENOMEM;
}

int pipewire__module_init(struct pw_module *module, const char *args)
{
	return module_init(module, NULL);
}
