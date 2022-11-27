/* Spa midi dbus
 *
 * Copyright © 2022 Pauli Virtanen
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

#endif DBUS_MONITOR_H_
