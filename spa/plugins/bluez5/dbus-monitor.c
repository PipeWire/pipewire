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
#include <gio/gio.h>

#include <spa/utils/defs.h>
#include <spa/utils/string.h>
#include <spa/support/log.h>

#include "dbus-monitor.h"


static void on_clear(struct dbus_monitor *monitor, GDBusProxy *proxy)
{
	const struct dbus_monitor_proxy_type *p;

	for (p = monitor->proxy_types; p && p->proxy_type != G_TYPE_INVALID ; ++p) {
		if (G_TYPE_CHECK_INSTANCE_TYPE(proxy, p->proxy_type)) {
			if (p->on_clear)
				p->on_clear(monitor, G_DBUS_INTERFACE(proxy));
		}
	}
}

static void on_g_properties_changed(GDBusProxy *proxy,
		GVariant *changed_properties, char **invalidated_properties,
		gpointer user_data)
{
	struct dbus_monitor *monitor = user_data;
	GDBusInterfaceInfo *info = g_dbus_interface_get_info(G_DBUS_INTERFACE(proxy));
	const char *name = info ? info->name : NULL;
	const struct dbus_monitor_proxy_type *p;

	spa_log_trace(monitor->log, "%p: dbus object updated path=%s, name=%s",
			monitor, g_dbus_proxy_get_object_path(proxy), name ? name : "<null>");

	for (p = monitor->proxy_types; p && p->proxy_type != G_TYPE_INVALID ; ++p) {
		if (G_TYPE_CHECK_INSTANCE_TYPE(proxy, p->proxy_type)) {
			if (p->on_object_update)
				p->on_object_update(monitor, G_DBUS_INTERFACE(proxy));
		}
	}

}

static void on_interface_added(GDBusObjectManager *self, GDBusObject *object,
		GDBusInterface *iface, gpointer user_data)
{
	struct dbus_monitor *monitor = user_data;
	GDBusInterfaceInfo *info = g_dbus_interface_get_info(iface);
	const char *name = info ? info->name : NULL;

	spa_log_trace(monitor->log, "%p: dbus interface added path=%s, name=%s",
			monitor, g_dbus_object_get_object_path(object), name ? name : "<null>");

	if (!g_object_get_data(G_OBJECT(iface), "dbus-monitor-signals-connected")) {
		g_object_set_data(G_OBJECT(iface), "dbus-monitor-signals-connected", GUINT_TO_POINTER(1));
		g_signal_connect(iface, "g-properties-changed",
				G_CALLBACK(on_g_properties_changed),
				monitor);
	}

	on_g_properties_changed(G_DBUS_PROXY(iface),
			NULL, NULL, monitor);
}

static void on_object_added(GDBusObjectManager *self, GDBusObject *object,
		gpointer user_data)
{
	struct dbus_monitor *monitor = user_data;
	GList *interfaces = g_dbus_object_get_interfaces(object);

	/*
	 * on_interface_added won't necessarily be called on objects on
	 * name owner changes, so we have to call it here for all interfaces.
	 */
	for (GList *lli = g_list_first(interfaces); lli; lli = lli->next) {
		on_interface_added(dbus_monitor_manager(monitor),
				object, G_DBUS_INTERFACE(lli->data), monitor);
	}

	g_list_free_full(interfaces, g_object_unref);
}

static void on_notify(GObject *gobject, GParamSpec *pspec, gpointer user_data)
{
	struct dbus_monitor *monitor = user_data;

	if (spa_streq(pspec->name, "name-owner") && monitor->on_name_owner_change)
		monitor->on_name_owner_change(monitor);
}

static GType get_proxy_type(GDBusObjectManagerClient *manager, const gchar *object_path,
		const gchar *interface_name, gpointer user_data)
{
	struct dbus_monitor *monitor = user_data;
	const struct dbus_monitor_proxy_type *p;

	for (p = monitor->proxy_types; p && p->proxy_type != G_TYPE_INVALID; ++p) {
		if (spa_streq(p->interface_name, interface_name))
			return p->proxy_type;
	}

	return G_TYPE_DBUS_PROXY;
}

static void init_done(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	struct dbus_monitor *monitor = user_data;
	GError *error = NULL;
	GList *objects;
	GObject *ret;

	g_clear_object(&monitor->call);

	ret = g_async_initable_new_finish(G_ASYNC_INITABLE(source_object), res, &error);
	if (!ret) {
		spa_log_error(monitor->log, "%p: creating DBus object monitor failed: %s",
				monitor, error->message);
		g_error_free(error);
		return;
	}
	monitor->manager = G_DBUS_OBJECT_MANAGER_CLIENT(ret);

	spa_log_debug(monitor->log, "%p: DBus monitor started", monitor);

	g_signal_connect(monitor->manager, "interface-added",
			G_CALLBACK(on_interface_added), monitor);
	g_signal_connect(monitor->manager, "object-added",
			G_CALLBACK(on_object_added), monitor);
	g_signal_connect(monitor->manager, "notify",
			G_CALLBACK(on_notify), monitor);

	/* List all objects now */
	objects = g_dbus_object_manager_get_objects(dbus_monitor_manager(monitor));
	for (GList *llo = g_list_first(objects); llo; llo = llo->next) {
		GList *interfaces = g_dbus_object_get_interfaces(G_DBUS_OBJECT(llo->data));

		for (GList *lli = g_list_first(interfaces); lli; lli = lli->next) {
			on_interface_added(dbus_monitor_manager(monitor),
					G_DBUS_OBJECT(llo->data), G_DBUS_INTERFACE(lli->data),
					monitor);
		}
		g_list_free_full(interfaces, g_object_unref);
	}
	g_list_free_full(objects, g_object_unref);
}

void dbus_monitor_init(struct dbus_monitor *monitor,
		GType client_type,
		struct spa_log *log, GDBusConnection *conn,
		const char *name, const char *object_path,
		const struct dbus_monitor_proxy_type *proxy_types,
		void (*on_name_owner_change)(struct dbus_monitor *monitor))
{
	GDBusObjectManagerClientFlags flags = G_DBUS_OBJECT_MANAGER_CLIENT_FLAGS_DO_NOT_AUTO_START;
	size_t i;

	spa_zero(*monitor);

	monitor->log = log;
	monitor->call = g_cancellable_new();
	monitor->on_name_owner_change = on_name_owner_change;

	spa_zero(monitor->proxy_types);

	for (i = 0; proxy_types && proxy_types[i].proxy_type != G_TYPE_INVALID; ++i) {
		spa_assert(i < DBUS_MONITOR_MAX_TYPES);
		monitor->proxy_types[i] = proxy_types[i];
	}

	g_async_initable_new_async(client_type, G_PRIORITY_DEFAULT,
			monitor->call, init_done, monitor,
			"flags", flags, "name", name, "connection", conn,
			"object-path", object_path,
			"get-proxy-type-func", get_proxy_type,
			"get-proxy-type-user-data", monitor,
			NULL);
}

void dbus_monitor_clear(struct dbus_monitor *monitor)
{
	g_cancellable_cancel(monitor->call);
	g_clear_object(&monitor->call);

	if (monitor->manager) {
		/* Indicate all objects should stop now.
		 *
		 * We need a separate hook, because the proxy finalizers
		 * may be called later asynchronously via e.g. DBus callbacks.
		 */
		GList *objects = g_dbus_object_manager_get_objects(dbus_monitor_manager(monitor));
		for (GList *llo = g_list_first(objects); llo; llo = llo->next) {
			GList *interfaces = g_dbus_object_get_interfaces(G_DBUS_OBJECT(llo->data));
			for (GList *lli = g_list_first(interfaces); lli; lli = lli->next)
				on_clear(monitor, G_DBUS_PROXY(lli->data));
			g_list_free_full(interfaces, g_object_unref);
		}
		g_list_free_full(objects, g_object_unref);
	}

	g_clear_object(&monitor->manager);
	spa_zero(*monitor);
}
