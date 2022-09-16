/* Spa Bluez5 ModemManager proxy
 *
 * Copyright © 2022 Collabora
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

#include <errno.h>
#include <spa/utils/string.h>

#include <ModemManager.h>

#include "modemmanager.h"

#define DBUS_INTERFACE_OBJECTMANAGER "org.freedesktop.DBus.ObjectManager"

struct modem {
	char *path;
	bool network_has_service;
	unsigned int signal_strength;
};

struct impl {
	struct spa_bt_monitor *monitor;

	struct spa_log *log;
	DBusConnection *conn;

	bool filters_added;
	DBusPendingCall *pending;

	const struct mm_ops *ops;
	void *user_data;

	struct modem modem;
};

static bool mm_dbus_connection_send_with_reply(struct impl *this, DBusMessage *m, DBusPendingCall **pending_return,
                                               DBusPendingCallNotifyFunction function, void *user_data)
{
	dbus_bool_t dbus_ret;

	spa_assert(*pending_return == NULL);

	dbus_ret = dbus_connection_send_with_reply(this->conn, m, pending_return, -1);
	if (!dbus_ret || *pending_return == NULL) {
		spa_log_debug(this->log, "dbus call failure");
		return false;
	}

	dbus_ret = dbus_pending_call_set_notify(*pending_return, function, user_data, NULL);
	if (!dbus_ret) {
		spa_log_debug(this->log, "dbus set notify failure");
		dbus_pending_call_cancel(*pending_return);
		dbus_pending_call_unref(*pending_return);
		*pending_return = NULL;
		return false;
	}

	return true;
}

static DBusHandlerResult mm_parse_modem3gpp_properties(struct impl *this, DBusMessageIter *props_i)
{
	while (dbus_message_iter_get_arg_type(props_i) != DBUS_TYPE_INVALID) {
		DBusMessageIter i, value_i;
		const char *key;

		dbus_message_iter_recurse(props_i, &i);

		dbus_message_iter_get_basic(&i, &key);
		dbus_message_iter_next(&i);
		dbus_message_iter_recurse(&i, &value_i);

		if (spa_streq(key, MM_MODEM_MODEM3GPP_PROPERTY_OPERATORNAME)) {
			char *operator_name;

			dbus_message_iter_get_basic(&value_i, &operator_name);
			spa_log_debug(this->log, "Network operator code: %s", operator_name);
			if (this->ops->set_modem_operator_name)
				this->ops->set_modem_operator_name(operator_name, this->user_data);
		} else if (spa_streq(key, MM_MODEM_MODEM3GPP_PROPERTY_REGISTRATIONSTATE)) {
			MMModem3gppRegistrationState state;
			bool is_roaming;

			dbus_message_iter_get_basic(&value_i, &state);
			spa_log_debug(this->log, "Registration state: %d", state);

			if (state == MM_MODEM_3GPP_REGISTRATION_STATE_ROAMING ||
			      state == MM_MODEM_3GPP_REGISTRATION_STATE_ROAMING_CSFB_NOT_PREFERRED ||
				  state == MM_MODEM_3GPP_REGISTRATION_STATE_ROAMING_SMS_ONLY)
				is_roaming = true;
			else
				is_roaming = false;

			if (this->ops->set_modem_roaming)
				this->ops->set_modem_roaming(is_roaming, this->user_data);
		}

		dbus_message_iter_next(props_i);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult mm_parse_modem_properties(struct impl *this, DBusMessageIter *props_i)
{
	while (dbus_message_iter_get_arg_type(props_i) != DBUS_TYPE_INVALID) {
		DBusMessageIter i, value_i;
		const char *key;

		dbus_message_iter_recurse(props_i, &i);

		dbus_message_iter_get_basic(&i, &key);
		dbus_message_iter_next(&i);
		dbus_message_iter_recurse(&i, &value_i);

		if(spa_streq(key, MM_MODEM_PROPERTY_EQUIPMENTIDENTIFIER)) {
			char *imei;

			dbus_message_iter_get_basic(&value_i, &imei);
			spa_log_debug(this->log, "Modem IMEI: %s", imei);
		} else if(spa_streq(key, MM_MODEM_PROPERTY_MANUFACTURER)) {
			char *manufacturer;

			dbus_message_iter_get_basic(&value_i, &manufacturer);
			spa_log_debug(this->log, "Modem manufacturer: %s", manufacturer);
		} else if(spa_streq(key, MM_MODEM_PROPERTY_MODEL)) {
			char *model;

			dbus_message_iter_get_basic(&value_i, &model);
			spa_log_debug(this->log, "Modem model: %s", model);
		} else if (spa_streq(key, MM_MODEM_PROPERTY_OWNNUMBERS)) {
			char *number;
			DBusMessageIter array_i;

			dbus_message_iter_recurse(&value_i, &array_i);
			if (dbus_message_iter_get_arg_type(&array_i) == DBUS_TYPE_STRING) {
				dbus_message_iter_get_basic(&array_i, &number);
				spa_log_debug(this->log, "Modem own number: %s", number);
			}
		} else if(spa_streq(key, MM_MODEM_PROPERTY_REVISION)) {
			char *revision;

			dbus_message_iter_get_basic(&value_i, &revision);
			spa_log_debug(this->log, "Modem revision: %s", revision);
		} else if(spa_streq(key, MM_MODEM_PROPERTY_SIGNALQUALITY)) {
			unsigned int percentage, signal_strength;
			DBusMessageIter struct_i;

			dbus_message_iter_recurse(&value_i, &struct_i);
			if (dbus_message_iter_get_arg_type(&struct_i) == DBUS_TYPE_UINT32) {
				dbus_message_iter_get_basic(&struct_i, &percentage);
				signal_strength = (unsigned int) round(percentage / 20.0);
				spa_log_debug(this->log, "Network signal strength: %d (%d)", percentage, signal_strength);
				if(this->ops->set_modem_signal_strength)
					this->ops->set_modem_signal_strength(signal_strength, this->user_data);
			}
		} else if(spa_streq(key, MM_MODEM_PROPERTY_STATE)) {
			MMModemState state;
			bool has_service;

			dbus_message_iter_get_basic(&value_i, &state);
			spa_log_debug(this->log, "Network state: %d", state);

			has_service = (state >= MM_MODEM_STATE_REGISTERED);
			if (this->ops->set_modem_service)
				this->ops->set_modem_service(has_service, this->user_data);
		}

		dbus_message_iter_next(props_i);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult mm_parse_interfaces(struct impl *this, DBusMessageIter *dict_i)
{
	DBusMessageIter element_i, props_i;
	const char *path;

	spa_assert(this);
	spa_assert(dict_i);

	dbus_message_iter_get_basic(dict_i, &path);
	dbus_message_iter_next(dict_i);
	dbus_message_iter_recurse(dict_i, &element_i);

	while (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_DICT_ENTRY) {
		DBusMessageIter iface_i;
		const char *interface;

		dbus_message_iter_recurse(&element_i, &iface_i);
		dbus_message_iter_get_basic(&iface_i, &interface);
		dbus_message_iter_next(&iface_i);
		spa_assert(dbus_message_iter_get_arg_type(&iface_i) == DBUS_TYPE_ARRAY);

		dbus_message_iter_recurse(&iface_i, &props_i);

		if (spa_streq(interface, MM_DBUS_INTERFACE_MODEM)) {
			this->modem.path = strdup(path);
			spa_log_debug(this->log, "Found Modem interface %s, path %s", interface, path);
			mm_parse_modem_properties(this, &props_i);
		} else if (spa_streq(interface, MM_DBUS_INTERFACE_MODEM_MODEM3GPP)) {
			spa_log_debug(this->log, "Found Modem3GPP interface %s, path %s", interface, path);
			mm_parse_modem3gpp_properties(this, &props_i);
		}

		dbus_message_iter_next(&element_i);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static void mm_get_managed_objects_reply(DBusPendingCall *pending, void *user_data)
{
	struct impl *this = user_data;
	DBusMessage *r;
	DBusMessageIter i, array_i;

	spa_assert(this->pending == pending);
	dbus_pending_call_unref(pending);
	this->pending = NULL;

	r = dbus_pending_call_steal_reply(pending);
	if (r == NULL)
		return;

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(this->log, "Failed to get a list of endpoints from ModemManager: %s",
				dbus_message_get_error_name(r));
		goto finish;
	}

	if (!dbus_message_iter_init(r, &i) || !spa_streq(dbus_message_get_signature(r), "a{oa{sa{sv}}}")) {
		spa_log_error(this->log, "Invalid arguments in GetManagedObjects() reply");
		goto finish;
	}

	dbus_message_iter_recurse(&i, &array_i);
	while (dbus_message_iter_get_arg_type(&array_i) != DBUS_TYPE_INVALID) {
			DBusMessageIter dict_i;

			dbus_message_iter_recurse(&array_i, &dict_i);
			mm_parse_interfaces(this, &dict_i);
			dbus_message_iter_next(&array_i);
	}

finish:
	dbus_message_unref(r);
}

static void mm_clean_modem3gpp(struct impl *this)
{
	if (this->ops->set_modem_operator_name)
		this->ops->set_modem_operator_name(NULL, this->user_data);
	if (this->ops->set_modem_roaming)
		this->ops->set_modem_roaming(false, this->user_data);
}

static void mm_clean_modem(struct impl *this)
{
	if (this->modem.path) {
		free(this->modem.path);
		this->modem.path = NULL;
	}
	if(this->ops->set_modem_signal_strength)
		this->ops->set_modem_signal_strength(0, this->user_data);
	if (this->ops->set_modem_service)
		this->ops->set_modem_service(false, this->user_data);
	this->modem.network_has_service = false;
}

static DBusHandlerResult mm_filter_cb(DBusConnection *bus, DBusMessage *m, void *user_data)
{
	struct impl *this = user_data;
	DBusError err;

	dbus_error_init(&err);

	if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
		const char *name, *old_owner, *new_owner;

		spa_log_debug(this->log, "Name owner changed %s", dbus_message_get_path(m));

		if (!dbus_message_get_args(m, &err,
					   DBUS_TYPE_STRING, &name,
					   DBUS_TYPE_STRING, &old_owner,
					   DBUS_TYPE_STRING, &new_owner,
					   DBUS_TYPE_INVALID)) {
			spa_log_error(this->log, "Failed to parse org.freedesktop.DBus.NameOwnerChanged: %s", err.message);
			goto finish;
		}

		if (spa_streq(name, MM_DBUS_SERVICE)) {
			if (old_owner && *old_owner) {
				spa_log_debug(this->log, "ModemManager daemon disappeared (%s)", old_owner);
				mm_clean_modem3gpp(this);
				mm_clean_modem(this);
			}

			if (new_owner && *new_owner)
				spa_log_debug(this->log, "ModemManager daemon appeared (%s)", new_owner);
		}
	} else if (dbus_message_is_signal(m, DBUS_INTERFACE_OBJECTMANAGER, DBUS_SIGNAL_INTERFACES_ADDED)) {
		DBusMessageIter arg_i;

		spa_log_warn(this->log, "sender: %s", dbus_message_get_sender(m));

		if (!dbus_message_iter_init(m, &arg_i) || !spa_streq(dbus_message_get_signature(m), "oa{sa{sv}}")) {
				spa_log_error(this->log, "Invalid signature found in InterfacesAdded");
				goto finish;
		}

		mm_parse_interfaces(this, &arg_i);
	} else if (dbus_message_is_signal(m, DBUS_INTERFACE_OBJECTMANAGER, DBUS_SIGNAL_INTERFACES_REMOVED)) {
		const char *path;
		DBusMessageIter arg_i, element_i;

		if (!dbus_message_iter_init(m, &arg_i) || !spa_streq(dbus_message_get_signature(m), "oas")) {
				spa_log_error(this->log, "Invalid signature found in InterfacesRemoved");
				goto finish;
		}

		dbus_message_iter_get_basic(&arg_i, &path);
		dbus_message_iter_next(&arg_i);
		dbus_message_iter_recurse(&arg_i, &element_i);

		while (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_STRING) {
				const char *iface;

				dbus_message_iter_get_basic(&element_i, &iface);

				spa_log_debug(this->log, "Interface removed %s", path);
				if (spa_streq(iface, MM_DBUS_INTERFACE_MODEM)) {
					spa_log_debug(this->log, "Modem interface %s removed, path %s", iface, path);
					mm_clean_modem(this);
				} else if (spa_streq(iface, MM_DBUS_INTERFACE_MODEM_MODEM3GPP)) {
					spa_log_debug(this->log, "Modem3GPP interface %s removed, path %s", iface, path);
					mm_clean_modem3gpp(this);
				}

				dbus_message_iter_next(&element_i);
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

		if (spa_streq(interface, MM_DBUS_INTERFACE_MODEM)) {
			spa_log_debug(this->log, "Properties changed on %s", path);
			mm_parse_modem_properties(this, &props_i);
		} else if (spa_streq(interface, MM_DBUS_INTERFACE_MODEM_MODEM3GPP)) {
			spa_log_debug(this->log, "Properties changed on %s", path);
			mm_parse_modem3gpp_properties(this, &props_i);
		}
	}

finish:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int add_filters(struct impl *this)
{
	DBusError err;

	if (this->filters_added)
		return 0;

	dbus_error_init(&err);

	if (!dbus_connection_add_filter(this->conn, mm_filter_cb, this, NULL)) {
		spa_log_error(this->log, "failed to add filter function");
		goto fail;
	}

	dbus_bus_add_match(this->conn,
			"type='signal',sender='org.freedesktop.DBus',"
			"interface='org.freedesktop.DBus',member='NameOwnerChanged'," "arg0='" MM_DBUS_SERVICE "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" MM_DBUS_SERVICE "',"
			"interface='" DBUS_INTERFACE_OBJECTMANAGER "',member='" DBUS_SIGNAL_INTERFACES_ADDED "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" MM_DBUS_SERVICE "',"
			"interface='" DBUS_INTERFACE_OBJECTMANAGER "',member='" DBUS_SIGNAL_INTERFACES_REMOVED "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" MM_DBUS_SERVICE "',"
			"interface='" DBUS_INTERFACE_PROPERTIES "',member='" DBUS_SIGNAL_PROPERTIES_CHANGED "'", &err);

	this->filters_added = true;

	return 0;

fail:
	dbus_error_free(&err);
	return -EIO;
}

static bool is_dbus_service_available(struct impl *this, const char *service)
{
	DBusMessage *m, *r;
	DBusError err;
	bool success = false;

	m = dbus_message_new_method_call("org.freedesktop.DBus", "/org/freedesktop/DBus",
	                                  "org.freedesktop.DBus", "NameHasOwner");
	if (m == NULL)
		return false;
	dbus_message_append_args(m, DBUS_TYPE_STRING, &service, DBUS_TYPE_INVALID);

	dbus_error_init(&err);
	r = dbus_connection_send_with_reply_and_block(this->conn, m, -1, &err);
	dbus_message_unref(m);
	m = NULL;

	if (r == NULL) {
		spa_log_info(this->log, "NameHasOwner failed for %s", service);
		dbus_error_free(&err);
		goto finish;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(this->log, "NameHasOwner() returned error: %s", dbus_message_get_error_name(r));
		goto finish;
	}

	if (!dbus_message_get_args(r, &err,
				   DBUS_TYPE_BOOLEAN, &success,
				   DBUS_TYPE_INVALID)) {
		spa_log_error(this->log, "Failed to parse NameHasOwner() reply: %s", err.message);
		dbus_error_free(&err);
		goto finish;
	}

finish:
	if (r)
		dbus_message_unref(r);

	return success;
}

void *mm_register(struct spa_log *log, void *dbus_connection, const struct mm_ops *ops, void *user_data)
{
	struct impl *this;

	spa_assert(log);
	spa_assert(dbus_connection);

	this = calloc(1, sizeof(struct impl));
	if (this == NULL)
		return NULL;

	this->log = log;
	this->conn = dbus_connection;
	this->ops = ops;
	this->user_data = user_data;

	if (add_filters(this) < 0) {
		goto fail;
	}

	if (is_dbus_service_available(this, MM_DBUS_SERVICE)) {
		DBusMessage *m;

		m = dbus_message_new_method_call(MM_DBUS_SERVICE, "/org/freedesktop/ModemManager1",
		                                 DBUS_INTERFACE_OBJECTMANAGER, "GetManagedObjects");
		if (m == NULL)
			goto fail;

		if (!mm_dbus_connection_send_with_reply(this, m, &this->pending, mm_get_managed_objects_reply, this)) {
			spa_log_error(this->log, "dbus call failure");
			dbus_message_unref(m);
			goto fail;
		}
	}

    return this;

fail:
    free(this);
    return NULL;
}

void mm_unregister(void *data)
{
	struct impl *this = data;

	if (this->pending != NULL) {
		dbus_pending_call_cancel(this->pending);
		dbus_pending_call_unref(this->pending);
	}

	mm_clean_modem3gpp(this);
	mm_clean_modem(this);

	if (this->filters_added) {
		dbus_connection_remove_filter(this->conn, mm_filter_cb, this);
		this->filters_added = false;
	}

	free(this);
}
