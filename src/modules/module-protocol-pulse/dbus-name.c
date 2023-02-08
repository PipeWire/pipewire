/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2019 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>

#include <dbus/dbus.h>

#include <spa/support/dbus.h>
#include <spa/support/plugin.h>
#include <pipewire/context.h>

#include "log.h"
#include "dbus-name.h"

void *dbus_request_name(struct pw_context *context, const char *name)
{
	struct spa_dbus *dbus;
	struct spa_dbus_connection *conn;
	const struct spa_support *support;
	uint32_t n_support;
	DBusConnection *bus;
	DBusError error;

	support = pw_context_get_support(context, &n_support);

	dbus = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DBus);
	if (dbus == NULL) {
		errno = ENOTSUP;
		return NULL;
	}

	conn = spa_dbus_get_connection(dbus, SPA_DBUS_TYPE_SESSION);
	if (conn == NULL)
		return NULL;

	bus = spa_dbus_connection_get(conn);
	if (bus == NULL) {
		spa_dbus_connection_destroy(conn);
		return NULL;
	}

	dbus_error_init(&error);

	if (dbus_bus_request_name(bus, name,
			DBUS_NAME_FLAG_DO_NOT_QUEUE,
			&error) == DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER)
		return conn;

	if (dbus_error_is_set(&error))
		pw_log_error("Failed to acquire %s: %s: %s", name, error.name, error.message);
	else
		pw_log_error("D-Bus name %s already taken.", name);

	dbus_error_free(&error);

	spa_dbus_connection_destroy(conn);

	errno = EEXIST;
	return NULL;
}

void dbus_release_name(void *data)
{
	struct spa_dbus_connection *conn = data;
	spa_dbus_connection_destroy(conn);
}
