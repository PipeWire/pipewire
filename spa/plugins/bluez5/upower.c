/* Spa Bluez5 UPower proxy */
/* SPDX-FileCopyrightText: Copyright © 2022 Collabora */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <spa/utils/string.h>

#include "dbus-helpers.h"
#include "upower.h"

#define UPOWER_SERVICE "org.freedesktop.UPower"
#define UPOWER_DEVICE_INTERFACE UPOWER_SERVICE ".Device"
#define UPOWER_DISPLAY_DEVICE_OBJECT "/org/freedesktop/UPower/devices/DisplayDevice"

struct impl {
	struct spa_bt_monitor *monitor;

	struct spa_log *log;
	DBusConnection *conn;

	DBusPendingCall *pending_get_call;

	bool filters_added;

	void *user_data;
	void (*set_battery_level)(unsigned int level, void *user_data);
};

static DBusHandlerResult upower_parse_percentage(struct impl *this, DBusMessageIter *variant_i)
{
	double percentage;
	unsigned int battery_level;

	dbus_message_iter_get_basic(variant_i, &percentage);
	spa_log_debug(this->log, "Battery level: %f %%", percentage);

	battery_level = (unsigned int) round(percentage / 20.0);
	this->set_battery_level(battery_level, this->user_data);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static void upower_get_percentage_properties_reply(DBusPendingCall *pending, void *user_data)
{
	struct impl *backend = user_data;
	DBusMessageIter i, variant_i;

	spa_assert(backend->pending_get_call == pending);
	spa_autoptr(DBusMessage) r = steal_reply_and_unref(&backend->pending_get_call);
	if (r == NULL)
		return;

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(backend->log, "Failed to get percentage from UPower: %s",
				dbus_message_get_error_name(r));
		return;
	}

	if (!dbus_message_iter_init(r, &i) || !spa_streq(dbus_message_get_signature(r), "v")) {
		spa_log_error(backend->log, "Invalid arguments in Get() reply");
		return;
	}

	dbus_message_iter_recurse(&i, &variant_i);
	upower_parse_percentage(backend, &variant_i);
}

static int update_battery_percentage(struct impl *this)
{
	cancel_and_unref(&this->pending_get_call);

	spa_autoptr(DBusMessage) m = dbus_message_new_method_call(UPOWER_SERVICE,
								  UPOWER_DISPLAY_DEVICE_OBJECT,
								  DBUS_INTERFACE_PROPERTIES,
								  "Get");
	if (!m)
		return -ENOMEM;

	dbus_message_append_args(m,
				 DBUS_TYPE_STRING, &(const char *){ UPOWER_DEVICE_INTERFACE },
				 DBUS_TYPE_STRING, &(const char *){ "Percentage" },
				 DBUS_TYPE_INVALID);
	dbus_message_set_auto_start(m, false);

	this->pending_get_call = send_with_reply(this->conn, m, upower_get_percentage_properties_reply, this);
	if (!this->pending_get_call)
		return -EIO;

	return 0;
}

static void upower_clean(struct impl *this)
{
	this->set_battery_level(0, this->user_data);
}

static DBusHandlerResult upower_filter_cb(DBusConnection *bus, DBusMessage *m, void *user_data)
{
	struct impl *this = user_data;

	if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
		const char *name, *old_owner, *new_owner;
		spa_auto(DBusError) err = DBUS_ERROR_INIT;

		spa_log_debug(this->log, "Name owner changed %s", dbus_message_get_path(m));

		if (!dbus_message_get_args(m, &err,
					   DBUS_TYPE_STRING, &name,
					   DBUS_TYPE_STRING, &old_owner,
					   DBUS_TYPE_STRING, &new_owner,
					   DBUS_TYPE_INVALID)) {
			spa_log_error(this->log, "Failed to parse org.freedesktop.DBus.NameOwnerChanged: %s", err.message);
			goto finish;
		}

		if (spa_streq(name, UPOWER_SERVICE)) {
			if (old_owner && *old_owner) {
				spa_log_debug(this->log, "UPower daemon disappeared (%s)", old_owner);
				upower_clean(this);
			}

			if (new_owner && *new_owner) {
				spa_log_debug(this->log, "UPower daemon appeared (%s)", new_owner);
				update_battery_percentage(this);
			}
		}
	} else if (dbus_message_is_signal(m, DBUS_INTERFACE_PROPERTIES, DBUS_SIGNAL_PROPERTIES_CHANGED)) {
		const char *path;
		DBusMessageIter iface_i, props_i;
		const char *interface;

		if (!dbus_message_iter_init(m, &iface_i) || !spa_streq(dbus_message_get_signature(m), "sa{sv}as")) {
				spa_log_error(this->log, "Invalid signature found in PropertiesChanged");
				goto finish;
		}

		dbus_message_iter_get_basic(&iface_i, &interface);
		dbus_message_iter_next(&iface_i);
		spa_assert(dbus_message_iter_get_arg_type(&iface_i) == DBUS_TYPE_ARRAY);

		dbus_message_iter_recurse(&iface_i, &props_i);

		path = dbus_message_get_path(m);

		if (spa_streq(interface, UPOWER_DEVICE_INTERFACE)) {
			spa_log_debug(this->log, "Properties changed on %s", path);

			while (dbus_message_iter_get_arg_type(&props_i) != DBUS_TYPE_INVALID) {
				DBusMessageIter i, value_i;
				const char *key;

				dbus_message_iter_recurse(&props_i, &i);

				dbus_message_iter_get_basic(&i, &key);
				dbus_message_iter_next(&i);
				dbus_message_iter_recurse(&i, &value_i);

				if(spa_streq(key, "Percentage"))
					upower_parse_percentage(this, &value_i);

				dbus_message_iter_next(&props_i);
			}
		}
	}

finish:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int add_filters(struct impl *this)
{
	if (this->filters_added)
		return 0;

	if (!dbus_connection_add_filter(this->conn, upower_filter_cb, this, NULL)) {
		spa_log_error(this->log, "failed to add filter function");
		return -EIO;
	}

	spa_auto(DBusError) err = DBUS_ERROR_INIT;

	dbus_bus_add_match(this->conn,
			"type='signal',sender='org.freedesktop.DBus',"
			"interface='org.freedesktop.DBus',member='NameOwnerChanged'," "arg0='" UPOWER_SERVICE "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" UPOWER_SERVICE "',"
			"interface='" DBUS_INTERFACE_PROPERTIES "',member='" DBUS_SIGNAL_PROPERTIES_CHANGED "',"
			"path='" UPOWER_DISPLAY_DEVICE_OBJECT "',arg0='" UPOWER_DEVICE_INTERFACE "'", &err);

	this->filters_added = true;

	return 0;
}

void *upower_register(struct spa_log *log,
                      void *dbus_connection,
                      void (*set_battery_level)(unsigned int level, void *user_data),
                      void *user_data)
{
	struct impl *this;

	spa_assert(log);
	spa_assert(dbus_connection);
	spa_assert(set_battery_level);
	spa_assert(user_data);

	this = calloc(1, sizeof(struct impl));
	if (this == NULL)
		return NULL;

	this->log = log;
	this->conn = dbus_connection;
	this->set_battery_level = set_battery_level;
	this->user_data = user_data;

	if (add_filters(this) < 0)
		goto fail4;

	if (update_battery_percentage(this) < 0)
		goto fail4;

	return this;

fail4:
	free(this);
	return NULL;
}

void upower_unregister(void *data)
{
	struct impl *this = data;

	cancel_and_unref(&this->pending_get_call);

	if (this->filters_added) {
		dbus_connection_remove_filter(this->conn, upower_filter_cb, this);
		this->filters_added = false;
	}
	free(this);
}
