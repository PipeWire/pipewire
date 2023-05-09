/* Spa midi dbus */
/* SPDX-FileCopyrightText: Copyright © 2022 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#ifndef DBUS_MONITOR_H_
#define DBUS_MONITOR_H_

#include <gio/gio.h>

#include <spa/utils/defs.h>
#include <spa/utils/string.h>
#include <spa/support/log.h>

#define DBUS_MONITOR_MAX_TYPES	16

struct dbus_monitor;

struct dbus_monitor_proxy_type
{
	/** Interface name to monitor, or NULL for object type */
	const char *interface_name;

	/** GObject type for the proxy */
	GType proxy_type;

	/** Hook called when object added or properties changed */
	void (*on_update)(struct dbus_monitor *monitor, GDBusInterface *iface);

	/** Hook called when object is removed (or on monitor shutdown) */
	void (*on_remove)(struct dbus_monitor *monitor, GDBusInterface *iface);
};

struct dbus_monitor
{
	GDBusObjectManagerClient *manager;
	struct spa_log *log;
	GCancellable *call;
	struct dbus_monitor_proxy_type proxy_types[DBUS_MONITOR_MAX_TYPES+1];
	void (*on_name_owner_change)(struct dbus_monitor *monitor);
	void *user_data;
};

static inline GDBusObjectManager *dbus_monitor_manager(struct dbus_monitor *monitor)
{
	return G_DBUS_OBJECT_MANAGER(monitor->manager);
}

/**
 * Create a DBus object monitor, with a given interface to proxy type map.
 *
 * \param proxy_types	Mapping between interface names and watched proxy
 * 	types, terminated by G_TYPE_INVALID.
 * \param on_object_update	Called for all objects and interfaces on
 *	startup, and when object properties are modified.
 */
void dbus_monitor_init(struct dbus_monitor *monitor,
		GType client_type, struct spa_log *log, GDBusConnection *conn,
		const char *name, const char *object_path,
		const struct dbus_monitor_proxy_type *proxy_types,
		void (*on_name_owner_change)(struct dbus_monitor *monitor));

void dbus_monitor_clear(struct dbus_monitor *monitor);

#endif // DBUS_MONITOR_H_
