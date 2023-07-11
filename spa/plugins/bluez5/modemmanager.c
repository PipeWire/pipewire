/* Spa Bluez5 ModemManager proxy */
/* SPDX-FileCopyrightText: Copyright © 2022 Collabora */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <spa/utils/string.h>

#include <ModemManager.h>

#include "dbus-helpers.h"
#include "modemmanager.h"

#define DBUS_INTERFACE_OBJECTMANAGER "org.freedesktop.DBus.ObjectManager"

struct modem {
	char *path;
	bool network_has_service;
	unsigned int signal_strength;
};

struct impl {
	struct spa_log *log;
	DBusConnection *conn;

	char *allowed_modem_device;
	bool filters_added;
	DBusPendingCall *pending;
	DBusPendingCall *voice_pending;

	const struct mm_ops *ops;
	void *user_data;

	struct modem modem;
	struct spa_list call_list;
};

struct dbus_cmd_data {
	struct impl *this;
	struct call *call;
	void *user_data;
};

static int mm_state_to_clcc(struct impl *this, MMCallState state)
{
	switch (state) {
	case MM_CALL_STATE_DIALING:
		return CLCC_DIALING;
	case MM_CALL_STATE_RINGING_OUT:
		return CLCC_ALERTING;
	case MM_CALL_STATE_RINGING_IN:
		return CLCC_INCOMING;
	case MM_CALL_STATE_ACTIVE:
		return CLCC_ACTIVE;
	case MM_CALL_STATE_HELD:
		return CLCC_HELD;
	case MM_CALL_STATE_WAITING:
		return CLCC_WAITING;
	case MM_CALL_STATE_TERMINATED:
	case MM_CALL_STATE_UNKNOWN:
	default:
		return -1;
	}
}

static void mm_call_state_changed(struct impl *this)
{
	struct call *call;
	bool call_indicator = false;
	enum call_setup call_setup_indicator = CIND_CALLSETUP_NONE;

	spa_list_for_each(call, &this->call_list, link) {
		call_indicator |= (call->state == CLCC_ACTIVE);

		if (call->state == CLCC_INCOMING && call_setup_indicator < CIND_CALLSETUP_INCOMING)
			call_setup_indicator = CIND_CALLSETUP_INCOMING;
		else if (call->state == CLCC_DIALING && call_setup_indicator < CIND_CALLSETUP_DIALING)
			call_setup_indicator = CIND_CALLSETUP_DIALING;
		else if (call->state == CLCC_ALERTING && call_setup_indicator < CIND_CALLSETUP_ALERTING)
			call_setup_indicator = CIND_CALLSETUP_ALERTING;
	}

	if (this->ops->set_call_active)
		this->ops->set_call_active(call_indicator, this->user_data);

	if (this->ops->set_call_setup)
		this->ops->set_call_setup(call_setup_indicator, this->user_data);
}

static void mm_get_call_properties_reply(DBusPendingCall *pending, void *user_data)
{
	struct call *call = user_data;
	struct impl *this = call->this;
	DBusMessageIter arg_i, element_i;
	MMCallDirection direction;
	MMCallState state;

	spa_assert(call->pending == pending);
	spa_autoptr(DBusMessage) r = steal_reply_and_unref(&call->pending);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(this->log, "ModemManager D-Bus Call not available");
		return;
	}
	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(this->log, "GetAll() failed: %s", dbus_message_get_error_name(r));
		return;
	}

	if (!dbus_message_iter_init(r, &arg_i) || !spa_streq(dbus_message_get_signature(r), "a{sv}")) {
		spa_log_error(this->log, "Invalid arguments in GetAll() reply");
		return;
	}

	spa_log_debug(this->log, "Call path: %s", call->path);

	dbus_message_iter_recurse(&arg_i, &element_i);
	while (dbus_message_iter_get_arg_type(&element_i) != DBUS_TYPE_INVALID) {
		DBusMessageIter i, value_i;
		const char *key;

		dbus_message_iter_recurse(&element_i, &i);

		dbus_message_iter_get_basic(&i, &key);
		dbus_message_iter_next(&i);
		dbus_message_iter_recurse(&i, &value_i);

		if (spa_streq(key, MM_CALL_PROPERTY_DIRECTION)) {
			dbus_message_iter_get_basic(&value_i, &direction);
			spa_log_debug(this->log, "Call direction: %u", direction);
			call->direction = (direction == MM_CALL_DIRECTION_INCOMING) ? CALL_INCOMING : CALL_OUTGOING;
		} else if (spa_streq(key, MM_CALL_PROPERTY_NUMBER)) {
			char *number;

			dbus_message_iter_get_basic(&value_i, &number);
			spa_log_debug(this->log, "Call number: %s", number);
			if (call->number)
				free(call->number);
			call->number = strdup(number);
		} else if (spa_streq(key, MM_CALL_PROPERTY_STATE)) {
			int clcc_state;

			dbus_message_iter_get_basic(&value_i, &state);
			spa_log_debug(this->log, "Call state: %u", state);
			clcc_state = mm_state_to_clcc(this, state);
			if (clcc_state < 0) {
				spa_log_debug(this->log, "Unsupported modem state: %s, state=%d", call->path, call->state);
			} else {
				call->state = clcc_state;
				mm_call_state_changed(this);
			}
		}

		dbus_message_iter_next(&element_i);
	}
}

static DBusHandlerResult mm_parse_voice_properties(struct impl *this, DBusMessageIter *props_i)
{
	while (dbus_message_iter_get_arg_type(props_i) != DBUS_TYPE_INVALID) {
		DBusMessageIter i, value_i, element_i;
		const char *key;

		dbus_message_iter_recurse(props_i, &i);

		dbus_message_iter_get_basic(&i, &key);
		dbus_message_iter_next(&i);
		dbus_message_iter_recurse(&i, &value_i);

		if (spa_streq(key, MM_MODEM_VOICE_PROPERTY_CALLS)) {
			spa_log_debug(this->log, "Voice properties");
			dbus_message_iter_recurse(&value_i, &element_i);

			while (dbus_message_iter_get_arg_type(&element_i) == DBUS_TYPE_OBJECT_PATH) {
				const char *call_object;

				dbus_message_iter_get_basic(&element_i, &call_object);
				spa_log_debug(this->log, "  Call: %s", call_object);

				dbus_message_iter_next(&element_i);
			}
		}

		dbus_message_iter_next(props_i);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
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
				if (this->ops->set_modem_own_number)
					this->ops->set_modem_own_number(number, this->user_data);
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
			spa_log_debug(this->log, "Found Modem interface %s, path %s", interface, path);
			if (this->modem.path == NULL) {
				if (this->allowed_modem_device) {
					DBusMessageIter i;

					dbus_message_iter_recurse(&iface_i, &i);
					while (dbus_message_iter_get_arg_type(&i) != DBUS_TYPE_INVALID) {
						DBusMessageIter key_i, value_i;
						const char *key;

						dbus_message_iter_recurse(&i, &key_i);

						dbus_message_iter_get_basic(&key_i, &key);
						dbus_message_iter_next(&key_i);
						dbus_message_iter_recurse(&key_i, &value_i);

						if (spa_streq(key, MM_MODEM_PROPERTY_DEVICE)) {
							char *device;

							dbus_message_iter_get_basic(&value_i, &device);
							if (!spa_streq(this->allowed_modem_device, device)) {
								spa_log_debug(this->log, "Modem not allowed: %s", device);
								goto next;
							}
						}
						dbus_message_iter_next(&i);
					}
				}
				this->modem.path = strdup(path);
			} else if (!spa_streq(this->modem.path, path)) {
				spa_log_debug(this->log, "A modem is already registered");
				goto next;
			}
			mm_parse_modem_properties(this, &props_i);
		} else if (spa_streq(interface, MM_DBUS_INTERFACE_MODEM_MODEM3GPP)) {
			if (spa_streq(this->modem.path, path)) {
				spa_log_debug(this->log, "Found Modem3GPP interface %s, path %s", interface, path);
				mm_parse_modem3gpp_properties(this, &props_i);
			}
		} else if (spa_streq(interface, MM_DBUS_INTERFACE_MODEM_VOICE)) {
			if (spa_streq(this->modem.path, path)) {
				spa_log_debug(this->log, "Found Voice interface %s, path %s", interface, path);
				mm_parse_voice_properties(this, &props_i);
			}
		}

next:
		dbus_message_iter_next(&element_i);
	}

	return DBUS_HANDLER_RESULT_HANDLED;
}

static void mm_get_managed_objects_reply(DBusPendingCall *pending, void *user_data)
{
	struct impl *this = user_data;
	DBusMessageIter i, array_i;

	spa_assert(this->pending == pending);
	spa_autoptr(DBusMessage) r = steal_reply_and_unref(&this->pending);
	if (r == NULL)
		return;

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(this->log, "Failed to get a list of endpoints from ModemManager: %s",
				dbus_message_get_error_name(r));
		return;
	}

	if (!dbus_message_iter_init(r, &i) || !spa_streq(dbus_message_get_signature(r), "a{oa{sa{sv}}}")) {
		spa_log_error(this->log, "Invalid arguments in GetManagedObjects() reply");
		return;
	}

	dbus_message_iter_recurse(&i, &array_i);
	while (dbus_message_iter_get_arg_type(&array_i) != DBUS_TYPE_INVALID) {
			DBusMessageIter dict_i;

			dbus_message_iter_recurse(&array_i, &dict_i);
			mm_parse_interfaces(this, &dict_i);
			dbus_message_iter_next(&array_i);
	}
}

static void call_free(struct call *call)
{
	spa_list_remove(&call->link);

	cancel_and_unref(&call->pending);

	if (call->number)
		free(call->number);
	if (call->path)
		free(call->path);
	free(call);
}

static void mm_clean_voice(struct impl *this)
{
	struct call *call;

	spa_list_consume(call, &this->call_list, link)
		call_free(call);

	cancel_and_unref(&this->voice_pending);

	if (this->ops->set_call_setup)
		this->ops->set_call_setup(CIND_CALLSETUP_NONE, this->user_data);
	if (this->ops->set_call_active)
		this->ops->set_call_active(false, this->user_data);
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

		if (spa_streq(name, MM_DBUS_SERVICE)) {
			if (old_owner && *old_owner) {
				spa_log_debug(this->log, "ModemManager daemon disappeared (%s)", old_owner);
				mm_clean_voice(this);
				mm_clean_modem3gpp(this);
				mm_clean_modem(this);
			}

			if (new_owner && *new_owner)
				spa_log_debug(this->log, "ModemManager daemon appeared (%s)", new_owner);
		}
	} else if (dbus_message_is_signal(m, DBUS_INTERFACE_OBJECTMANAGER, DBUS_SIGNAL_INTERFACES_ADDED)) {
		DBusMessageIter arg_i;

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
		if (!spa_streq(this->modem.path, path))
			goto finish;

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
				} else if (spa_streq(iface, MM_DBUS_INTERFACE_MODEM_VOICE)) {
					spa_log_debug(this->log, "Voice interface %s removed, path %s", iface, path);
					mm_clean_voice(this);
				}

				dbus_message_iter_next(&element_i);
		}
	} else if (dbus_message_is_signal(m, DBUS_INTERFACE_PROPERTIES, DBUS_SIGNAL_PROPERTIES_CHANGED)) {
		const char *path;
		DBusMessageIter iface_i, props_i;
		const char *interface;

		path = dbus_message_get_path(m);
		if (!spa_streq(this->modem.path, path))
			goto finish;

		if (!dbus_message_iter_init(m, &iface_i) || !spa_streq(dbus_message_get_signature(m), "sa{sv}as")) {
				spa_log_error(this->log, "Invalid signature found in PropertiesChanged");
				goto finish;
		}

		dbus_message_iter_get_basic(&iface_i, &interface);
		dbus_message_iter_next(&iface_i);
		spa_assert(dbus_message_iter_get_arg_type(&iface_i) == DBUS_TYPE_ARRAY);

		dbus_message_iter_recurse(&iface_i, &props_i);

		if (spa_streq(interface, MM_DBUS_INTERFACE_MODEM)) {
			spa_log_debug(this->log, "Properties changed on %s", path);
			mm_parse_modem_properties(this, &props_i);
		} else if (spa_streq(interface, MM_DBUS_INTERFACE_MODEM_MODEM3GPP)) {
			spa_log_debug(this->log, "Properties changed on %s", path);
			mm_parse_modem3gpp_properties(this, &props_i);
		} else if (spa_streq(interface, MM_DBUS_INTERFACE_MODEM_VOICE)) {
			spa_log_debug(this->log, "Properties changed on %s", path);
			mm_parse_voice_properties(this, &props_i);
		}
	} else if (dbus_message_is_signal(m, MM_DBUS_INTERFACE_MODEM_VOICE, MM_MODEM_VOICE_SIGNAL_CALLADDED)) {
		DBusMessageIter iface_i;
		const char *path;
		struct call *call_object;
		const char *mm_call_interface = MM_DBUS_INTERFACE_CALL;
		spa_autoptr(DBusMessage) m = NULL;

		if (!spa_streq(this->modem.path, dbus_message_get_path(m)))
			goto finish;

		if (!dbus_message_iter_init(m, &iface_i) || !spa_streq(dbus_message_get_signature(m), "o")) {
				spa_log_error(this->log, "Invalid signature found in %s", MM_MODEM_VOICE_SIGNAL_CALLADDED);
				goto finish;
		}

		dbus_message_iter_get_basic(&iface_i, &path);
		spa_log_debug(this->log, "New call: %s", path);

		call_object = calloc(1, sizeof(struct call));
		if (call_object == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		call_object->this = this;
		call_object->path = strdup(path);
		spa_list_append(&this->call_list, &call_object->link);

		m = dbus_message_new_method_call(MM_DBUS_SERVICE, path, DBUS_INTERFACE_PROPERTIES, "GetAll");
		if (m == NULL)
			goto finish;
		dbus_message_append_args(m, DBUS_TYPE_STRING, &mm_call_interface, DBUS_TYPE_INVALID);

		call_object->pending = send_with_reply(this->conn, m, mm_get_call_properties_reply, call_object);
		if (!call_object->pending) {
			spa_log_error(this->log, "dbus call failure");
			goto finish;
		}
	} else if (dbus_message_is_signal(m, MM_DBUS_INTERFACE_MODEM_VOICE, MM_MODEM_VOICE_SIGNAL_CALLDELETED)) {
		const char *path;
		DBusMessageIter iface_i;
		struct call *call, *call_tmp;

		if (!spa_streq(this->modem.path, dbus_message_get_path(m)))
			goto finish;

		if (!dbus_message_iter_init(m, &iface_i) || !spa_streq(dbus_message_get_signature(m), "o")) {
				spa_log_error(this->log, "Invalid signature found in %s", MM_MODEM_VOICE_SIGNAL_CALLDELETED);
				goto finish;
		}

		dbus_message_iter_get_basic(&iface_i, &path);
		spa_log_debug(this->log, "Call ended: %s", path);

		spa_list_for_each_safe(call, call_tmp, &this->call_list, link) {
			if (spa_streq(call->path, path))
				call_free(call);
		}
		mm_call_state_changed(this);
	} else if (dbus_message_is_signal(m, MM_DBUS_INTERFACE_CALL, MM_CALL_SIGNAL_STATECHANGED)) {
		const char *path;
		DBusMessageIter iface_i;
		MMCallState old, new;
		MMCallStateReason reason;
		struct call *call = NULL, *call_tmp;
		int clcc_state;

		if (!dbus_message_iter_init(m, &iface_i) || !spa_streq(dbus_message_get_signature(m), "iiu")) {
				spa_log_error(this->log, "Invalid signature found in %s", MM_CALL_SIGNAL_STATECHANGED);
				goto finish;
		}

		path = dbus_message_get_path(m);

		dbus_message_iter_get_basic(&iface_i, &old);
		dbus_message_iter_next(&iface_i);
		dbus_message_iter_get_basic(&iface_i, &new);
		dbus_message_iter_next(&iface_i);
		dbus_message_iter_get_basic(&iface_i, &reason);

		spa_log_debug(this->log, "Call state %s changed to %d (old = %d, reason = %u)", path, new, old, reason);

		spa_list_for_each(call_tmp, &this->call_list, link) {
			if (spa_streq(call_tmp->path, path)) {
				call = call_tmp;
				break;
			}
		}

		if (call == NULL) {
			spa_log_warn(this->log, "No call reference for %s", path);
			goto finish;
		}

		clcc_state = mm_state_to_clcc(this, new);
		if (clcc_state < 0) {
			spa_log_debug(this->log, "Unsupported modem state: %s, state=%d", call->path, call->state);
		} else {
			call->state = clcc_state;
			mm_call_state_changed(this);
		}
	}

finish:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static int add_filters(struct impl *this)
{
	if (this->filters_added)
		return 0;

	if (!dbus_connection_add_filter(this->conn, mm_filter_cb, this, NULL)) {
		spa_log_error(this->log, "failed to add filter function");
		return -EIO;
	}

	spa_auto(DBusError) err = DBUS_ERROR_INIT;

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
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" MM_DBUS_SERVICE "',"
			"interface='" MM_DBUS_INTERFACE_MODEM_VOICE "',member='" MM_MODEM_VOICE_SIGNAL_CALLADDED "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" MM_DBUS_SERVICE "',"
			"interface='" MM_DBUS_INTERFACE_MODEM_VOICE "',member='" MM_MODEM_VOICE_SIGNAL_CALLDELETED "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" MM_DBUS_SERVICE "',"
			"interface='" MM_DBUS_INTERFACE_CALL "',member='" MM_CALL_SIGNAL_STATECHANGED "'", &err);

	this->filters_added = true;

	return 0;
}

bool mm_is_available(void *modemmanager)
{
	struct impl *this = modemmanager;

	if (this == NULL)
		return false;

	return this->modem.path != NULL;
}

unsigned int mm_supported_features(void)
{
	return SPA_BT_HFP_AG_FEATURE_REJECT_CALL | SPA_BT_HFP_AG_FEATURE_ENHANCED_CALL_STATUS;
}

static void mm_get_call_simple_reply(DBusPendingCall *pending, void *data)
{
	struct dbus_cmd_data *dbus_cmd_data = data;
	struct impl *this = dbus_cmd_data->this;
	struct call *call = dbus_cmd_data->call;
	void *user_data = dbus_cmd_data->user_data;

	free(data);

	spa_assert(call->pending == pending);
	spa_autoptr(DBusMessage) r = steal_reply_and_unref(&call->pending);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(this->log, "ModemManager D-Bus method not available");
		goto finish;
	}
	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(this->log, "ModemManager method failed: %s", dbus_message_get_error_name(r));
		goto finish;
	}

	this->ops->send_cmd_result(true, 0, user_data);
	return;

finish:
	this->ops->send_cmd_result(false, CMEE_AG_FAILURE, user_data);
}

static void mm_get_call_create_reply(DBusPendingCall *pending, void *data)
{
	struct dbus_cmd_data *dbus_cmd_data = data;
	struct impl *this = dbus_cmd_data->this;
	void *user_data = dbus_cmd_data->user_data;

	free(data);

	spa_assert(this->voice_pending == pending);
	spa_autoptr(DBusMessage) r = steal_reply_and_unref(&this->voice_pending);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(this->log, "ModemManager D-Bus method not available");
		goto finish;
	}
	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(this->log, "ModemManager method failed: %s", dbus_message_get_error_name(r));
		goto finish;
	}

	this->ops->send_cmd_result(true, 0, user_data);
	return;

finish:
	this->ops->send_cmd_result(false, CMEE_AG_FAILURE, user_data);
}

bool mm_answer_call(void *modemmanager, void *user_data, enum cmee_error *error)
{
	struct impl *this = modemmanager;
	struct call *call_object, *call_tmp;
	spa_autofree struct dbus_cmd_data *data = NULL;
	spa_autoptr(DBusMessage) m = NULL;

	call_object = NULL;
	spa_list_for_each(call_tmp, &this->call_list, link) {
		if (call_tmp->state == CLCC_INCOMING) {
			call_object = call_tmp;
			break;
		}
	}
	if (!call_object) {
		spa_log_debug(this->log, "No ringing in call");
		if (error)
			*error = CMEE_OPERATION_NOT_ALLOWED;
		return false;
	}

	data = malloc(sizeof(struct dbus_cmd_data));
	if (!data) {
		if (error)
			*error = CMEE_AG_FAILURE;
		return false;
	}
	data->this = this;
	data->call = call_object;
	data->user_data = user_data;

	m = dbus_message_new_method_call(MM_DBUS_SERVICE, call_object->path, MM_DBUS_INTERFACE_CALL, MM_CALL_METHOD_ACCEPT);
	if (m == NULL) {
		if (error)
			*error = CMEE_AG_FAILURE;
		return false;
	}

	call_object->pending = send_with_reply(this->conn, m, mm_get_call_simple_reply, data);
	if (!call_object->pending) {
		spa_log_error(this->log, "dbus call failure");
		if (error)
			*error = CMEE_AG_FAILURE;
		return false;
	}

	return spa_steal_ptr(data), true;
}

bool mm_hangup_call(void *modemmanager, void *user_data, enum cmee_error *error)
{
	struct impl *this = modemmanager;
	struct call *call_object, *call_tmp;
	spa_autofree struct dbus_cmd_data *data= NULL;
	spa_autoptr(DBusMessage) m = NULL;

	call_object = NULL;
	spa_list_for_each(call_tmp, &this->call_list, link) {
		if (call_tmp->state == CLCC_ACTIVE) {
			call_object = call_tmp;
			break;
		}
	}
	if (!call_object) {
		spa_list_for_each(call_tmp, &this->call_list, link) {
			if (call_tmp->state == CLCC_DIALING ||
				call_tmp->state == CLCC_ALERTING ||
				call_tmp->state == CLCC_INCOMING) {
				call_object = call_tmp;
				break;
			}
		}
	}
	if (!call_object) {
		spa_log_debug(this->log, "No call to reject or hang up");
		if (error)
			*error = CMEE_OPERATION_NOT_ALLOWED;
		return false;
	}

	data = malloc(sizeof(struct dbus_cmd_data));
	if (!data) {
		if (error)
			*error = CMEE_AG_FAILURE;
		return false;
	}
	data->this = this;
	data->call = call_object;
	data->user_data = user_data;

	m = dbus_message_new_method_call(MM_DBUS_SERVICE, call_object->path, MM_DBUS_INTERFACE_CALL, MM_CALL_METHOD_HANGUP);
	if (m == NULL) {
		if (error)
			*error = CMEE_AG_FAILURE;
		return false;
	}

	call_object->pending = send_with_reply(this->conn, m, mm_get_call_simple_reply, data);
	if (!call_object->pending) {
		spa_log_error(this->log, "dbus call failure");
		if (error)
			*error = CMEE_AG_FAILURE;
		return false;
	}

	return spa_steal_ptr(data), true;
}

static void append_basic_variant_dict_entry(DBusMessageIter *dict, const char* key, int variant_type_int, const char* variant_type_str, void* variant) {
	DBusMessageIter dict_entry_it, variant_it;
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &dict_entry_it);
	dbus_message_iter_append_basic(&dict_entry_it, DBUS_TYPE_STRING, &key);

	dbus_message_iter_open_container(&dict_entry_it, DBUS_TYPE_VARIANT, variant_type_str, &variant_it);
	dbus_message_iter_append_basic(&variant_it, variant_type_int, variant);
	dbus_message_iter_close_container(&dict_entry_it, &variant_it);
	dbus_message_iter_close_container(dict, &dict_entry_it);
}

static inline bool is_valid_dial_string_char(char c)
{
	return ('0' <= c && c <= '9')
		|| ('A' <= c && c <= 'C')
		|| c == '*'
		|| c == '#'
		|| c == '+';
}

bool mm_do_call(void *modemmanager, const char* number, void *user_data, enum cmee_error *error)
{
	struct impl *this = modemmanager;
	spa_autofree struct dbus_cmd_data *data = NULL;
	spa_autoptr(DBusMessage) m = NULL;
	DBusMessageIter iter, dict;

	for (size_t i = 0; number[i]; i++) {
		if (!is_valid_dial_string_char(number[i])) {
			spa_log_warn(this->log, "Call creation canceled, invalid character found in dial string: %c", number[i]);
			if (error)
				*error = CMEE_INVALID_CHARACTERS_DIAL_STRING;
			return false;
		}
	}

	data = malloc(sizeof(struct dbus_cmd_data));
	if (!data) {
		if (error)
			*error = CMEE_AG_FAILURE;
		return false;
	}
	data->this = this;
	data->user_data = user_data;

	m = dbus_message_new_method_call(MM_DBUS_SERVICE, this->modem.path, MM_DBUS_INTERFACE_MODEM_VOICE, MM_MODEM_VOICE_METHOD_CREATECALL);
	if (m == NULL) {
		if (error)
			*error = CMEE_AG_FAILURE;
		return false;
	}
	dbus_message_iter_init_append(m, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &dict);
	append_basic_variant_dict_entry(&dict, "number", DBUS_TYPE_STRING, "s", &number);
	dbus_message_iter_close_container(&iter, &dict);

	this->voice_pending = send_with_reply(this->conn, m, mm_get_call_create_reply, data);
	if (!this->voice_pending) {
		spa_log_error(this->log, "dbus call failure");
		if (error)
			*error = CMEE_AG_FAILURE;
		return false;
	}

	return spa_steal_ptr(data), true;
}

bool mm_send_dtmf(void *modemmanager, const char *dtmf, void *user_data, enum cmee_error *error)
{
	struct impl *this = modemmanager;
	struct call *call_object, *call_tmp;
	spa_autofree struct dbus_cmd_data *data = NULL;
	spa_autoptr(DBusMessage) m = NULL;

	call_object = NULL;
	spa_list_for_each(call_tmp, &this->call_list, link) {
		if (call_tmp->state == CLCC_ACTIVE) {
			call_object = call_tmp;
			break;
		}
	}
	if (!call_object) {
		spa_log_debug(this->log, "No active call");
		if (error)
			*error = CMEE_OPERATION_NOT_ALLOWED;
		return false;
	}

	/* Allowed dtmf characters: 0-9, *, #, A-D */
	if (!((dtmf[0] >= '0' && dtmf[0] <= '9')
	    || (dtmf[0] == '*')
	    || (dtmf[0] == '#')
	    || (dtmf[0] >= 'A' && dtmf[0] <= 'D'))) {
		spa_log_debug(this->log, "Invalid DTMF character: %s", dtmf);
		if (error)
			*error = CMEE_INVALID_CHARACTERS_TEXT_STRING;
		return false;
	}

	data = malloc(sizeof(struct dbus_cmd_data));
	if (!data) {
		if (error)
			*error = CMEE_AG_FAILURE;
		return false;
	}
	data->this = this;
	data->call = call_object;
	data->user_data = user_data;

	m = dbus_message_new_method_call(MM_DBUS_SERVICE, call_object->path, MM_DBUS_INTERFACE_CALL, MM_CALL_METHOD_SENDDTMF);
	if (m == NULL) {
		if (error)
			*error = CMEE_AG_FAILURE;
		return false;
	}
	dbus_message_append_args(m, DBUS_TYPE_STRING, &dtmf, DBUS_TYPE_INVALID);

	call_object->pending = send_with_reply(this->conn, m, mm_get_call_simple_reply, data);
	if (!call_object->pending) {
		spa_log_error(this->log, "dbus call failure");
		if (error)
			*error = CMEE_AG_FAILURE;
		return false;
	}

	return spa_steal_ptr(data), true;
}

const char *mm_get_incoming_call_number(void *modemmanager)
{
	struct impl *this = modemmanager;
	struct call *call_object, *call_tmp;

	call_object = NULL;
	spa_list_for_each(call_tmp, &this->call_list, link) {
		if (call_tmp->state == CLCC_INCOMING) {
			call_object = call_tmp;
			break;
		}
	}
	if (!call_object) {
		spa_log_debug(this->log, "No ringing in call");
		return NULL;
	}

	return call_object->number;
}

struct spa_list *mm_get_calls(void *modemmanager)
{
	struct impl *this = modemmanager;

	return &this->call_list;
}

void *mm_register(struct spa_log *log, void *dbus_connection, const struct spa_dict *info,
                  const struct mm_ops *ops, void *user_data)
{
	const char *modem_device_str = NULL;
	bool modem_device_found = false;

	spa_assert(log);
	spa_assert(dbus_connection);

	if (info) {
		if ((modem_device_str = spa_dict_lookup(info, "bluez5.hfphsp-backend-native-modem")) != NULL) {
			if (!spa_streq(modem_device_str, "none"))
				modem_device_found = true;
		}
	}
	if (!modem_device_found) {
		spa_log_info(log, "No modem allowed, doesn't link to ModemManager");
		return NULL;
	}

	spa_autofree struct impl *this = calloc(1, sizeof(*this));
	if (this == NULL)
		return NULL;

	this->log = log;
	this->conn = dbus_connection;
	this->ops = ops;
	this->user_data = user_data;
	if (modem_device_str && !spa_streq(modem_device_str, "any"))
		this->allowed_modem_device = strdup(modem_device_str);
	spa_list_init(&this->call_list);

	if (add_filters(this) < 0)
		return NULL;

	spa_autoptr(DBusMessage) m = dbus_message_new_method_call(MM_DBUS_SERVICE,
								  "/org/freedesktop/ModemManager1",
								  DBUS_INTERFACE_OBJECTMANAGER,
								  "GetManagedObjects");
	if (m == NULL)
		return NULL;

	dbus_message_set_auto_start(m, false);

	this->pending = send_with_reply(this->conn, m, mm_get_managed_objects_reply, this);
	if (!this->pending) {
		spa_log_error(this->log, "dbus call failure");
		return NULL;
	}

	return spa_steal_ptr(this);
}

void mm_unregister(void *data)
{
	struct impl *this = data;

	cancel_and_unref(&this->pending);

	mm_clean_voice(this);
	mm_clean_modem3gpp(this);
	mm_clean_modem(this);

	if (this->filters_added) {
		dbus_connection_remove_filter(this->conn, mm_filter_cb, this);
		this->filters_added = false;
	}

	if (this->allowed_modem_device)
		free(this->allowed_modem_device);

	free(this);
}
