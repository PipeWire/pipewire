/* PipeWire
 *
 * Copyright © 2016 Wim Taymans <wim.taymans@gmail.com>
 * Copyright © 2019 Red Hat Inc.
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

#include <dbus/dbus.h>

#include <spa/utils/string.h>
#include <spa/support/dbus.h>

#include "pipewire/context.h"
#include "pipewire/impl-client.h"
#include "pipewire/log.h"
#include "pipewire/module.h"
#include "pipewire/utils.h"
#include "pipewire/private.h"

/** \page page_module_portal PipeWire Module: Portal
 *
 * The `portal` module performs access control management for clients started
 * inside an XDG portal.
 *
 * The module connects to the session DBus and subscribes to
 * `NameOwnerChanged` signals for the `org.freedesktop.portal.Desktop` name.
 * The PID of the DBus name owner is the portal.
 *
 * A client connection from the portal PID to PipeWire gets assigned a \ref
 * PW_KEY_ACCESS of `"portal"` and set to permissions ALL - it is the
 * responsibility of the portal to limit the permissions before passing the
 * connection on to the client. See \ref page_access for details on
 * permissions.
 *
 * Clients connecting from other PIDs are ignored by this module.
 *
 * ## Module Options
 *
 * There are no module-specific options.
 *
 * ## General options
 *
 * There are no general options for this module.
 *
 * ## Example configuration
 *\code{.unparsed}
 * context.modules = [
 *  {   name = libpipewire-portal }
 * ]
 *\endcode
 *
 */

#define NAME "portal"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct impl {
	struct pw_context *context;
	struct pw_properties *properties;

	struct spa_dbus_connection *conn;
	DBusConnection *bus;

	struct spa_hook context_listener;
	struct spa_hook module_listener;

	DBusPendingCall *portal_pid_pending;
	pid_t portal_pid;
};

static void
context_check_access(void *data, struct pw_impl_client *client)
{
	struct impl *impl = data;
	const struct pw_properties *props;
	struct pw_permission permissions[1];
	struct spa_dict_item items[1];
	pid_t pid;

	if (impl->portal_pid == 0)
		return;

	if ((props = pw_impl_client_get_properties(client)) == NULL)
		return;

	if (pw_properties_fetch_int32(props, PW_KEY_SEC_PID, &pid) < 0)
		return;

	if (pid != impl->portal_pid)
		return;

	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_ACCESS, "portal");
	pw_impl_client_update_properties(client, &SPA_DICT_INIT(items, 1));

	pw_log_info("%p: portal managed client %p added", impl, client);

	/* portal makes this connection and will change the permissions before
	 * handing this connection to the client */
	permissions[0] = PW_PERMISSION_INIT(PW_ID_ANY, PW_PERM_ALL);
	pw_impl_client_update_permissions(client, 1, permissions);
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

	if (impl->bus)
		dbus_connection_unref(impl->bus);
	spa_dbus_connection_destroy(impl->conn);

	pw_properties_free(impl->properties);

	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static void on_portal_pid_received(DBusPendingCall *pending,
				   void *user_data)
{
	struct impl *impl = user_data;
	DBusMessage *m;
	DBusError error;
	uint32_t portal_pid = 0;

	m = dbus_pending_call_steal_reply(pending);
	dbus_pending_call_unref(pending);
	impl->portal_pid_pending = NULL;

	if (!m) {
		pw_log_error("Failed to receive portal pid");
		return;
	}
	if (dbus_message_is_error(m, DBUS_ERROR_NAME_HAS_NO_OWNER)) {
		pw_log_info("Portal is not running");
		return;
	}
	if (dbus_message_get_type(m) == DBUS_MESSAGE_TYPE_ERROR) {
		const char *message = "unknown";
		dbus_message_get_args(m, NULL, DBUS_TYPE_STRING, &message, DBUS_TYPE_INVALID);
		pw_log_warn("Failed to receive portal pid: %s: %s",
				dbus_message_get_error_name(m), message);
		return;
	}

	dbus_error_init(&error);
	dbus_message_get_args(m, &error, DBUS_TYPE_UINT32, &portal_pid,
			      DBUS_TYPE_INVALID);
	dbus_message_unref(m);

	if (dbus_error_is_set(&error)) {
		impl->portal_pid = 0;
		pw_log_warn("Could not get portal pid: %s", error.message);
		dbus_error_free(&error);
	} else {
		pw_log_info("Got portal pid %d", portal_pid);
		impl->portal_pid = portal_pid;
	}
}

static void update_portal_pid(struct impl *impl)
{
	DBusMessage *m;
	const char *name;
	DBusPendingCall *pending;

	impl->portal_pid = 0;

	m = dbus_message_new_method_call("org.freedesktop.DBus",
					 "/org/freedesktop/DBus",
					 "org.freedesktop.DBus",
					 "GetConnectionUnixProcessID");

	name = "org.freedesktop.portal.Desktop";
	dbus_message_append_args(m,
				 DBUS_TYPE_STRING, &name,
				 DBUS_TYPE_INVALID);

	dbus_connection_send_with_reply(impl->bus, m, &pending, -1);
	dbus_pending_call_set_notify(pending, on_portal_pid_received, impl, NULL);
	if (impl->portal_pid_pending != NULL) {
		dbus_pending_call_cancel(impl->portal_pid_pending);
		dbus_pending_call_unref(impl->portal_pid_pending);
	}
	impl->portal_pid_pending = pending;
}

static DBusHandlerResult name_owner_changed_handler(DBusConnection *connection,
						    DBusMessage *message,
						    void *user_data)
{
	struct impl *impl = user_data;
	const char *name;
	const char *old_owner;
	const char *new_owner;

	if (!dbus_message_is_signal(message, "org.freedesktop.DBus",
				   "NameOwnerChanged"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (!dbus_message_get_args(message, NULL,
				   DBUS_TYPE_STRING, &name,
				   DBUS_TYPE_STRING, &old_owner,
				   DBUS_TYPE_STRING, &new_owner,
				   DBUS_TYPE_INVALID)) {
		pw_log_error("Failed to get OwnerChanged args");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (!spa_streq(name, "org.freedesktop.portal.Desktop"))
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	if (spa_streq(new_owner, "")) {
		impl->portal_pid = 0;
		if (impl->portal_pid_pending != NULL) {
			dbus_pending_call_cancel(impl->portal_pid_pending);
			dbus_pending_call_unref(impl->portal_pid_pending);
		}
	}
	else {
		update_portal_pid(impl);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static int init_dbus_connection(struct impl *impl)
{
	DBusError error;

	impl->bus = spa_dbus_connection_get(impl->conn);
	if (impl->bus == NULL)
		return -EIO;

	/* XXX: we don't handle dbus reconnection yet, so ref the handle instead */
	dbus_connection_ref(impl->bus);

	dbus_error_init(&error);

	dbus_bus_add_match(impl->bus,
			   "type='signal',\
			   sender='org.freedesktop.DBus',\
			   interface='org.freedesktop.DBus',\
			   member='NameOwnerChanged'",
			   &error);
	if (dbus_error_is_set(&error)) {
		pw_log_error("Failed to add name owner changed listener: %s",
			     error.message);
		dbus_error_free(&error);
		return -EIO;
	}

	dbus_connection_add_filter(impl->bus, name_owner_changed_handler,
				   impl, NULL);

	update_portal_pid(impl);

	return 0;
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct impl *impl;
	struct spa_dbus *dbus;
	const struct spa_support *support;
	uint32_t n_support;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	support = pw_context_get_support(context, &n_support);

	dbus = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DBus);
        if (dbus == NULL)
                return -ENOTSUP;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -ENOMEM;

	pw_log_debug("module %p: new", impl);

	impl->context = context;
	impl->properties = args ? pw_properties_new_string(args) : NULL;

	impl->conn = spa_dbus_get_connection(dbus, SPA_DBUS_TYPE_SESSION);
	if (impl->conn == NULL) {
		res = -errno;
		goto error;
	}

	if ((res = init_dbus_connection(impl)) < 0)
		goto error;

	pw_context_add_listener(context, &impl->context_listener, &context_events, impl);
	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	return 0;

      error:
	free(impl);
	pw_log_error("Failed to connect to session bus: %s", spa_strerror(res));
	return res;
}
