/* Spa Bluez5 Telephony D-Bus service */
/* SPDX-FileCopyrightText: Copyright © 2024 Collabora Ltd. */
/* SPDX-License-Identifier: MIT */

#include "telephony.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <dbus/dbus.h>
#include <spa-private/dbus-helpers.h>

#include <spa/utils/list.h>
#include <spa/utils/string.h>

#define PW_TELEPHONY_SERVICE "org.pipewire.Telephony"

#define PW_TELEPHONY_OBJECT_PATH "/org/pipewire/Telephony"

#define PW_TELEPHONY_AG_IFACE "org.pipewire.Telephony.AudioGateway1"
#define PW_TELEPHONY_AG_TRANSPORT_IFACE "org.pipewire.Telephony.AudioGatewayTransport1"
#define PW_TELEPHONY_CALL_IFACE "org.pipewire.Telephony.Call1"

#define OFONO_MANAGER_IFACE "org.ofono.Manager"
#define OFONO_VOICE_CALL_MANAGER_IFACE "org.ofono.VoiceCallManager"
#define OFONO_VOICE_CALL_IFACE "org.ofono.VoiceCall"

#define DBUS_OBJECT_MANAGER_IFACE_INTROSPECT_XML				\
	" <interface name='" DBUS_INTERFACE_OBJECT_MANAGER "'>"			\
	"  <method name='GetManagedObjects'>"		 			\
	"   <arg name='objects' direction='out' type='a{oa{sa{sv}}}'/>"		\
	"  </method>"								\
	"  <signal name='InterfacesAdded'>"					\
	"   <arg name='object' type='o'/>"					\
	"   <arg name='interfaces' type='a{sa{sv}}'/>"				\
	"  </signal>"								\
	"  <signal name='InterfacesRemoved'>"					\
	"   <arg name='object' type='o'/>"					\
	"   <arg name='interfaces' type='as'/>"	 				\
	"  </signal>"								\
	" </interface>"

#define DBUS_PROPERTIES_IFACE_INTROSPECT_XML					\
	" <interface name='" DBUS_INTERFACE_PROPERTIES "'>"			\
	"  <method name='Get'>"							\
	"   <arg name='interface' type='s' direction='in' />"			\
	"   <arg name='name' type='s' direction='in' />"			\
	"   <arg name='value' type='v' direction='out' />"			\
	"  </method>"								\
	"  <method name='Set'>"							\
	"   <arg name='interface' type='s' direction='in' />"			\
	"   <arg name='name' type='s' direction='in' />"			\
	"   <arg name='value' type='v' direction='in' />"			\
	"  </method>"								\
	"  <method name='GetAll'>"						\
	"   <arg name='interface' type='s' direction='in' />"			\
	"   <arg name='properties' type='a{sv}' direction='out' />"		\
	"  </method>"								\
	"  <signal name='PropertiesChanged'>"					\
	"   <arg name='interface' type='s' />"					\
	"   <arg name='changed_properties' type='a{sv}' />"			\
	"   <arg name='invalidated_properties' type='as' />"			\
	"  </signal>"								\
	" </interface>"

#define DBUS_INTROSPECTABLE_IFACE_INTROSPECT_XML				\
	" <interface name='" DBUS_INTERFACE_INTROSPECTABLE "'>"			\
	"  <method name='Introspect'>"						\
	"   <arg name='xml' type='s' direction='out'/>"				\
	"  </method>"								\
	" </interface>"

#define PW_TELEPHONY_MANAGER_INTROSPECT_XML \
	DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE				\
	"<node>"		 						\
	" <interface name='" OFONO_MANAGER_IFACE "'>"				\
	"  <method name='GetModems'>"		 				\
	"   <arg name='objects' direction='out' type='a{oa{sv}}'/>"		\
	"  </method>"								\
	"  <signal name='ModemAdded'>"						\
	"   <arg name='path' type='o'/>"					\
	"   <arg name='properties' type='a{sv}'/>"				\
	"  </signal>"								\
	"  <signal name='ModemRemoved'>"					\
	"   <arg name='path' type='o'/>"					\
	"  </signal>"								\
	" </interface>"								\
	DBUS_OBJECT_MANAGER_IFACE_INTROSPECT_XML				\
	DBUS_INTROSPECTABLE_IFACE_INTROSPECT_XML				\
	"</node>"

#define PW_TELEPHONY_AG_COMMON_INTROSPECT_XML					\
	"  <method name='Dial'>"						\
	"   <arg name='number' direction='in' type='s'/>"			\
	"  </method>"								\
	"  <method name='SwapCalls'>"						\
	"  </method>"								\
	"  <method name='ReleaseAndAnswer'>"					\
	"  </method>"								\
	"  <method name='ReleaseAndSwap'>"					\
	"  </method>"								\
	"  <method name='HoldAndAnswer'>"					\
	"  </method>"								\
	"  <method name='HangupAll'>"						\
	"  </method>"								\
	"  <method name='CreateMultiparty'>"					\
	"  </method>"								\
	"  <method name='SendTones'>"						\
	"   <arg name='tones' direction='in' type='s'/>"			\
	"  </method>"

#define PW_TELEPHONY_AG_INTROSPECT_XML \
	DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE				\
	"<node>"		 						\
	" <interface name='" PW_TELEPHONY_AG_IFACE "'>"				\
	PW_TELEPHONY_AG_COMMON_INTROSPECT_XML					\
	"  <property name='Address' type='s' access='read'>"			\
	"    <annotation name='org.freedesktop.DBus.Property.EmitsChangedSignal' value='const'/>" \
	"  </property>"								\
	"  <property name='SpeakerVolume' type='y' access='readwrite'/>"	\
	"  <property name='MicrophoneVolume' type='y' access='readwrite'/>"	\
	" </interface>"								\
	" <interface name='" PW_TELEPHONY_AG_TRANSPORT_IFACE "'>"		\
	"  <property name='State' type='s' access='read'/>"			\
	"  <property name='Codec' type='y' access='read'/>"			\
	"  <property name='RejectSCO' type='b' access='readwrite'/>"		\
	"  <method name='Activate'/>"						\
	" </interface>"								\
	" <interface name='" OFONO_VOICE_CALL_MANAGER_IFACE "'>"		\
	PW_TELEPHONY_AG_COMMON_INTROSPECT_XML					\
	"  <method name='GetCalls'>"		 				\
	"   <arg name='objects' direction='out' type='a{oa{sv}}'/>"		\
	"  </method>"								\
	"  <signal name='CallAdded'>"						\
	"   <arg name='path' type='o'/>"					\
	"   <arg name='properties' type='a{sv}'/>"				\
	"  </signal>"								\
	"  <signal name='CallRemoved'>"						\
	"   <arg name='path' type='o'/>"					\
	"  </signal>"								\
	" </interface>"								\
	DBUS_OBJECT_MANAGER_IFACE_INTROSPECT_XML				\
	DBUS_PROPERTIES_IFACE_INTROSPECT_XML					\
	DBUS_INTROSPECTABLE_IFACE_INTROSPECT_XML				\
	"</node>"

#define PW_TELEPHONY_CALL_COMMON_INTROSPECT_XML					\
	"  <method name='Answer'>"						\
	"  </method>"								\
	"  <method name='Hangup'>"						\
	"  </method>"

#define PW_TELEPHONY_CALL_INTROSPECT_XML \
	DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE				\
	"<node>"								\
	" <interface name='" PW_TELEPHONY_CALL_IFACE "'>"			\
	PW_TELEPHONY_CALL_COMMON_INTROSPECT_XML					\
	"  <property name='LineIdentification' type='s' access='read'/>"	\
	"  <property name='IncomingLine' type='s' access='read'/>"		\
	"  <property name='Name' type='s' access='read'/>"			\
	"  <property name='Multiparty' type='b' access='read'/>"		\
	"  <property name='State' type='s' access='read'/>"			\
	" </interface>"								\
	" <interface name='" OFONO_VOICE_CALL_IFACE "'>"			\
	PW_TELEPHONY_CALL_COMMON_INTROSPECT_XML					\
	"  <method name='GetProperties'>"					\
	"   <arg name='properties' type='a{sv}' direction='out' />"		\
	"  </method>"								\
	"  <signal name='PropertyChanged'>"					\
	"   <arg name='property' type='s' />"					\
	"   <arg name='value' type='v' />"					\
	"  </signal>"								\
	" </interface>"								\
	DBUS_PROPERTIES_IFACE_INTROSPECT_XML					\
	DBUS_INTROSPECTABLE_IFACE_INTROSPECT_XML				\
	"</node>"

SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.bluez5.telephony");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

struct callimpl;

struct impl {
	struct spa_bt_telephony this;

	struct spa_log *log;
	struct spa_dbus *dbus;

	struct spa_dbus_connection *dbus_connection;
	DBusConnection *conn;

	const char *path;
	struct spa_list ag_list;

	bool default_reject_sco;
};

struct agimpl {
	struct spa_bt_telephony_ag this;
	struct spa_list link;
	char *path;
	struct spa_callbacks callbacks;
	void *user_data;

	struct {
		int volume[SPA_BT_VOLUME_ID_TERM];
		struct spa_bt_telephony_ag_transport transport;
	} prev;
};

struct callimpl {
	struct spa_bt_telephony_call this;
	char *path;
	struct spa_callbacks callbacks;
	void *user_data;

	/* previous values of properties */
	struct {
		char *line_identification;
		char *incoming_line;
		char *name;
		bool multiparty;
		enum spa_bt_telephony_call_state state;
	} prev;
};

#define ag_emit(ag,m,v,...) 			spa_callbacks_call(&ag->callbacks, struct spa_bt_telephony_ag_callbacks, m, v, ##__VA_ARGS__)
#define ag_emit_dial(s,n,m)			ag_emit(s,dial,0,n,m)
#define ag_emit_swap_calls(s,m)			ag_emit(s,swap_calls,0,m)
#define ag_emit_release_and_answer(s,m)		ag_emit(s,release_and_answer,0,m)
#define ag_emit_release_and_swap(s,m)		ag_emit(s,release_and_swap,0,m)
#define ag_emit_hold_and_answer(s,m)		ag_emit(s,hold_and_answer,0,m)
#define ag_emit_hangup_all(s,m)			ag_emit(s,hangup_all,0,m)
#define ag_emit_create_multiparty(s,m)		ag_emit(s,create_multiparty,0,m)
#define ag_emit_send_tones(s,t,m)		ag_emit(s,send_tones,0,t,m)
#define ag_emit_transport_activate(s,m) 	ag_emit(s,transport_activate,0,m)
#define ag_emit_set_speaker_volume(s,v,m) 	ag_emit(s,set_speaker_volume,0,v,m)
#define ag_emit_set_microphone_volume(s,v,m) 	ag_emit(s,set_microphone_volume,0,v,m)

#define call_emit(c,m,v,...) 	spa_callbacks_call(&c->callbacks, struct spa_bt_telephony_call_callbacks, m, v, ##__VA_ARGS__)
#define call_emit_answer(s,m)	call_emit(s,answer,0,m)
#define call_emit_hangup(s,m)	call_emit(s,hangup,0,m)

static void dbus_iter_append_ag_interfaces(DBusMessageIter *i, struct spa_bt_telephony_ag *ag);
static void dbus_iter_append_call_properties(DBusMessageIter *i, struct spa_bt_telephony_call *call, bool all);

#define PW_TELEPHONY_ERROR_FAILED "org.pipewire.Telephony.Error.Failed"
#define PW_TELEPHONY_ERROR_NOT_SUPPORTED "org.pipewire.Telephony.Error.NotSupported"
#define PW_TELEPHONY_ERROR_INVALID_FORMAT "org.pipewire.Telephony.Error.InvalidFormat"
#define PW_TELEPHONY_ERROR_INVALID_STATE "org.pipewire.Telephony.Error.InvalidState"
#define PW_TELEPHONY_ERROR_IN_PROGRESS "org.pipewire.Telephony.Error.InProgress"
#define PW_TELEPHONY_ERROR_CME "org.pipewire.Telephony.Error.CME"

static const char *telephony_error_to_dbus (enum spa_bt_telephony_error err)
{
	switch (err) {
	case BT_TELEPHONY_ERROR_FAILED:
		return PW_TELEPHONY_ERROR_FAILED;
	case BT_TELEPHONY_ERROR_NOT_SUPPORTED:
		return PW_TELEPHONY_ERROR_NOT_SUPPORTED;
	case BT_TELEPHONY_ERROR_INVALID_FORMAT:
		return PW_TELEPHONY_ERROR_INVALID_FORMAT;
	case BT_TELEPHONY_ERROR_INVALID_STATE:
		return PW_TELEPHONY_ERROR_INVALID_STATE;
	case BT_TELEPHONY_ERROR_IN_PROGRESS:
		return PW_TELEPHONY_ERROR_IN_PROGRESS;
	case BT_TELEPHONY_ERROR_CME:
		return PW_TELEPHONY_ERROR_CME;
	default:
		return "";
	}
}

static const char *telephony_error_to_description (enum spa_bt_telephony_error err, uint8_t cme_error)
{
	switch (err) {
	case BT_TELEPHONY_ERROR_FAILED:
		return "Method call failed";
	case BT_TELEPHONY_ERROR_NOT_SUPPORTED:
		return "Method is not supported on this Audio Gateway";
	case BT_TELEPHONY_ERROR_INVALID_FORMAT:
		return "Invalid phone number or tones";
	case BT_TELEPHONY_ERROR_INVALID_STATE:
		return "The current state does not allow this method call";
	case BT_TELEPHONY_ERROR_IN_PROGRESS:
		return "Command already in progress";
	case BT_TELEPHONY_ERROR_CME:
		switch (cme_error) {
		case 0: return "AG failure";
		case 1: return "no connection to phone";
		case 3: return "operation not allowed";
		case 4: return "operation not supported";
		case 5: return "PH-SIM PIN required";
		case 10: return "SIM not inserted";
		case 11: return "SIM PIN required";
		case 12: return "SIM PUK required";
		case 13: return "SIM failure";
		case 14: return "SIM busy";
		case 16: return "incorrect password";
		case 17: return "SIM PIN2 required";
		case 18: return "SIM PUK2 required";
		case 20: return "memory full";
		case 21: return "invalid index";
		case 23: return "memory failure";
		case 24: return "text string too long";
		case 25: return "invalid characters in text string";
		case 26: return "dial string too long";
		case 27: return "invalid characters in dial string";
		case 30: return "no network service";
		case 31: return "network Timeout";
		case 32: return "network not allowed - Emergency calls only";
		default: return "Unknown CME error";
		}
	default:
		return "";
	}
}

#define find_free_object_id(list, obj_type, link)	\
({						\
	int id = 1;				\
	obj_type *object;			\
	spa_list_for_each(object, list, link) {	\
		if (object->this.id <= id)		\
			id = object->this.id + 1;	\
	}					\
	id;					\
})

static DBusMessage *manager_introspect(struct impl *impl, DBusMessage *m)
{
	const char *xml = PW_TELEPHONY_MANAGER_INTROSPECT_XML;
	spa_autoptr(DBusMessage) r = NULL;
	if ((r = dbus_message_new_method_return(m)) == NULL)
		return NULL;
	if (!dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID))
		return NULL;
	return spa_steal_ptr(r);
}

static DBusMessage *manager_get_managed_objects(struct impl *impl, DBusMessage *m, bool ofono_compat)
{
	struct agimpl *agimpl;
	spa_autoptr(DBusMessage) r = NULL;
	DBusMessageIter iter, array1, entry1, props_dict;

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return NULL;

	dbus_message_iter_init_append(r, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
		ofono_compat ? "{oa{sv}}" : "{oa{sa{sv}}}", &array1);

	spa_list_for_each (agimpl, &impl->ag_list, link) {
		if (agimpl->path) {
			dbus_message_iter_open_container(&array1, DBUS_TYPE_DICT_ENTRY, NULL, &entry1);
			if (ofono_compat) {
				dbus_message_iter_append_basic(&entry1, DBUS_TYPE_OBJECT_PATH, &agimpl->path);
				dbus_message_iter_open_container(&entry1, DBUS_TYPE_ARRAY, "{sv}", &props_dict);
				dbus_message_iter_close_container(&entry1, &props_dict);
			} else {
				dbus_iter_append_ag_interfaces(&entry1, &agimpl->this);
			}
			dbus_message_iter_close_container(&array1, &entry1);
		}
	}
	dbus_message_iter_close_container(&iter, &array1);

	return spa_steal_ptr(r);
}

static DBusHandlerResult manager_handler(DBusConnection *c, DBusMessage *m, void *userdata)
{
	struct impl *impl = userdata;

	spa_autoptr(DBusMessage) r = NULL;
	const char *path, *interface, *member;

	path = dbus_message_get_path(m);
	interface = dbus_message_get_interface(m);
	member = dbus_message_get_member(m);

	spa_log_debug(impl->log, "dbus: path=%s, interface=%s, member=%s", path, interface, member);

	if (dbus_message_is_method_call(m, DBUS_INTERFACE_INTROSPECTABLE, "Introspect")) {
		r = manager_introspect(impl, m);
	} else if (dbus_message_is_method_call(m, DBUS_INTERFACE_OBJECT_MANAGER, "GetManagedObjects")) {
		r = manager_get_managed_objects(impl, m, false);
	} else if (dbus_message_is_method_call(m, OFONO_MANAGER_IFACE, "GetModems")) {
		r = manager_get_managed_objects(impl, m, true);
	} else {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (r == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(impl->conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	return DBUS_HANDLER_RESULT_HANDLED;
}

struct spa_bt_telephony *
telephony_new(struct spa_log *log, struct spa_dbus *dbus, const struct spa_dict *info)
{
	struct impl *impl = NULL;
	spa_auto(DBusError) err = DBUS_ERROR_INIT;
	bool service_enabled = true;
	bool ofono_service_compat = false;
	enum spa_dbus_type bus_type = SPA_DBUS_TYPE_SESSION;
	int res;

	static const DBusObjectPathVTable vtable_manager = {
		.message_function = manager_handler,
	};

	spa_assert(log);
	spa_assert(dbus);

	spa_log_topic_init(log, &log_topic);

	if (info) {
		const char *str;
		if ((str = spa_dict_lookup(info, "bluez5.telephony-dbus-service")) != NULL) {
			service_enabled = spa_atob(str);
		}
		if ((str = spa_dict_lookup(info, "bluez5.telephony.use-system-bus")) != NULL) {
			bus_type = spa_atob(str) ? SPA_DBUS_TYPE_SYSTEM : SPA_DBUS_TYPE_SESSION;
		}
		if ((str = spa_dict_lookup(info, "bluez5.telephony.provide-ofono")) != NULL) {
			ofono_service_compat = spa_atob(str);
			bus_type = SPA_DBUS_TYPE_SYSTEM;
		}
	}

	if (!service_enabled) {
		spa_log_info(log, "Bluetooth Telephony service disabled by configuration");
		return NULL;
	}

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->log = log;
	impl->dbus = dbus;
	impl->ag_list = SPA_LIST_INIT(&impl->ag_list);

	impl->dbus_connection = spa_dbus_get_connection(impl->dbus, bus_type);
	if (impl->dbus_connection == NULL) {
		spa_log_warn(impl->log, "no session dbus connection");
		goto fail;
	}
	impl->conn = spa_dbus_connection_get(impl->dbus_connection);
	if (impl->conn == NULL) {
		spa_log_warn(impl->log, "failed to get session dbus connection");
		goto fail;
	}

	impl->default_reject_sco = false;
	if (info) {
		const char *str;
		if ((str = spa_dict_lookup(info, "bluez5.telephony.default-reject-sco")) != NULL) {
			impl->default_reject_sco = spa_atob(str);
		}
	}

	/* XXX: We should handle spa_dbus reconnecting, but we don't, so ref
	 * XXX: the handle so that we can keep it if spa_dbus unrefs it.
	 */
	dbus_connection_ref(impl->conn);

	res = dbus_bus_request_name(impl->conn,
				    ofono_service_compat ? OFONO_SERVICE : PW_TELEPHONY_SERVICE,
				    DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
	if (res < 0) {
		spa_log_warn(impl->log, "D-Bus RequestName() error: %s", err.message);
		goto fail;
	}
	if (res == DBUS_REQUEST_NAME_REPLY_EXISTS) {
		spa_log_warn(impl->log, "Bluetooth Telephony service is already registered by another connection");
		goto fail;
	}

	impl->path = ofono_service_compat ? "/" : PW_TELEPHONY_OBJECT_PATH;

	if (!dbus_connection_register_object_path(impl->conn, impl->path,
						  &vtable_manager, impl)) {
		goto fail;
	}

	return &impl->this;

fail:
	spa_log_info(impl->log, "Bluetooth Telephony service disabled due to failure");
	if (impl->conn)
		dbus_connection_unref(impl->conn);
	if (impl->dbus_connection)
		spa_dbus_connection_destroy(impl->dbus_connection);
	free(impl);
	return NULL;
}

void telephony_free(struct spa_bt_telephony *telephony)
{
	struct impl *impl = SPA_CONTAINER_OF(telephony, struct impl, this);
	struct agimpl *agimpl;

	spa_list_consume (agimpl, &impl->ag_list, link) {
		telephony_ag_destroy(&agimpl->this);
	}

	dbus_connection_unref(impl->conn);
	spa_dbus_connection_destroy(impl->dbus_connection);
	impl->dbus_connection = NULL;
	impl->conn = NULL;

	free(impl);
}

void telephony_send_dbus_method_reply(struct spa_bt_telephony *telephony, DBusMessage *m,
				enum spa_bt_telephony_error err, uint8_t cme_error)
{
	struct impl *impl = SPA_CONTAINER_OF(telephony, struct impl, this);
	spa_autoptr(DBusMessage) reply = NULL;

	if (err == BT_TELEPHONY_ERROR_NONE)
		reply = dbus_message_new_method_return(m);
	else
		reply = dbus_message_new_error(m, telephony_error_to_dbus (err),
			telephony_error_to_description (err, cme_error));

	dbus_connection_send(impl->conn, reply, NULL);
}

static void telephony_ag_commit_properties(struct spa_bt_telephony_ag *ag)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	for (int i = 0; i < SPA_BT_VOLUME_ID_TERM; ++i) {
		agimpl->prev.volume[i] = ag->volume[i];
	}
}

static void telephony_ag_transport_commit_properties(struct spa_bt_telephony_ag *ag)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	agimpl->prev.transport = ag->transport;
}

static const char * const * transport_state_to_string(int state)
{
	static const char * const state_str[] = {
		"error",
		"idle",
		"pending",
		"active",
	};
	if (state < -1 || state > 2)
		state = -1;
	return &state_str[state + 1];
}

static bool
dbus_iter_append_ag_properties(DBusMessageIter *i, struct spa_bt_telephony_ag *ag, bool all)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	DBusMessageIter dict, entry, variant;
	bool changed = false;

	dbus_message_iter_open_container(i, DBUS_TYPE_ARRAY, "{sv}", &dict);

	/* Address must be set before registering and never changes,
	   so there is no need to check for changes here */
	if (all) {
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
		const char *name = "Address";
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_STRING_AS_STRING,
						&variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &ag->address);
		dbus_message_iter_close_container(&entry, &variant);
		dbus_message_iter_close_container(&dict, &entry);
		changed = true;
	}

	if (all || ag->volume[SPA_BT_VOLUME_ID_RX] != agimpl->prev.volume[SPA_BT_VOLUME_ID_RX]) {
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
		const char *name = "SpeakerVolume";
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_BYTE_AS_STRING,
						&variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_BYTE, &ag->volume[SPA_BT_VOLUME_ID_RX]);
		dbus_message_iter_close_container(&entry, &variant);
		dbus_message_iter_close_container(&dict, &entry);
		changed = true;
	}

	if (all || ag->volume[SPA_BT_VOLUME_ID_TX] != agimpl->prev.volume[SPA_BT_VOLUME_ID_TX]) {
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
		const char *name = "MicrophoneVolume";
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_BYTE_AS_STRING,
						&variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_BYTE, &ag->volume[SPA_BT_VOLUME_ID_TX]);
		dbus_message_iter_close_container(&entry, &variant);
		dbus_message_iter_close_container(&dict, &entry);
		changed = true;
	}

	dbus_message_iter_close_container(i, &dict);
	return changed;
}

static bool
dbus_iter_append_ag_transport_properties(DBusMessageIter *i, struct spa_bt_telephony_ag *ag, bool all)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	DBusMessageIter dict, entry, variant;
	bool changed = false;

	dbus_message_iter_open_container(i, DBUS_TYPE_ARRAY, "{sv}", &dict);

	if (all || ag->transport.codec != agimpl->prev.transport.codec) {
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
		const char *name = "Codec";
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_BYTE_AS_STRING,
						&variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_BYTE, &ag->transport.codec);
		dbus_message_iter_close_container(&entry, &variant);
		dbus_message_iter_close_container(&dict, &entry);
		changed = true;
	}

	if (all || ag->transport.state != agimpl->prev.transport.state) {
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
		const char *name = "State";
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_STRING_AS_STRING,
						&variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING,
						transport_state_to_string(ag->transport.state));
		dbus_message_iter_close_container(&entry, &variant);
		dbus_message_iter_close_container(&dict, &entry);
		changed = true;
	}

	if (all || ag->transport.rejectSCO != agimpl->prev.transport.rejectSCO) {
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
		const char *name = "RejectSCO";
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_BOOLEAN_AS_STRING,
						&variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN,
						&ag->transport.rejectSCO);
		dbus_message_iter_close_container(&entry, &variant);
		dbus_message_iter_close_container(&dict, &entry);
		changed = true;
	}

	dbus_message_iter_close_container(i, &dict);
	return changed;
}

static void
dbus_iter_append_ag_interfaces(DBusMessageIter *i, struct spa_bt_telephony_ag *ag)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	DBusMessageIter entry, dict;

	dbus_message_iter_append_basic(i, DBUS_TYPE_OBJECT_PATH, &agimpl->path);
	dbus_message_iter_open_container(i, DBUS_TYPE_ARRAY, "{sa{sv}}", &dict);

	const char *interface = PW_TELEPHONY_AG_IFACE;
	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &interface);
	dbus_iter_append_ag_properties(&entry, ag, true);
	dbus_message_iter_close_container(&dict, &entry);

	const char *interface2 = PW_TELEPHONY_AG_TRANSPORT_IFACE;
	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &interface2);
	dbus_iter_append_ag_transport_properties(&entry, ag, true);
	dbus_message_iter_close_container(&dict, &entry);

	dbus_message_iter_close_container(i, &dict);
}

static DBusMessage *ag_introspect(struct agimpl *agimpl, DBusMessage *m)
{
	const char *xml = PW_TELEPHONY_AG_INTROSPECT_XML;
	spa_autoptr(DBusMessage) r = NULL;
	if ((r = dbus_message_new_method_return(m)) == NULL)
		return NULL;
	if (!dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID))
		return NULL;
	return spa_steal_ptr(r);
}

static DBusMessage *ag_get_managed_objects(struct agimpl *agimpl, DBusMessage *m, bool ofono_compat)
{
	struct callimpl *callimpl;
	spa_autoptr(DBusMessage) r = NULL;
	DBusMessageIter iter, array1, entry1, array2, entry2;
	const char *interface = PW_TELEPHONY_CALL_IFACE;

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return NULL;

	dbus_message_iter_init_append(r, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
		ofono_compat ? "{oa{sv}}" : "{oa{sa{sv}}}", &array1);

	spa_list_for_each (callimpl, &agimpl->this.call_list, this.link) {
		dbus_message_iter_open_container(&array1, DBUS_TYPE_DICT_ENTRY, NULL, &entry1);
		dbus_message_iter_append_basic(&entry1, DBUS_TYPE_OBJECT_PATH, &callimpl->path);
		if (ofono_compat) {
			dbus_iter_append_call_properties(&entry1, &callimpl->this, true);
		} else {
			dbus_message_iter_open_container(&entry1, DBUS_TYPE_ARRAY, "{sa{sv}}", &array2);
			dbus_message_iter_open_container(&array2, DBUS_TYPE_DICT_ENTRY, NULL, &entry2);
			dbus_message_iter_append_basic(&entry2, DBUS_TYPE_STRING, &interface);
			dbus_iter_append_call_properties(&entry2, &callimpl->this, true);
			dbus_message_iter_close_container(&array2, &entry2);
			dbus_message_iter_close_container(&entry1, &array2);
		}
		dbus_message_iter_close_container(&array1, &entry1);
	}
	dbus_message_iter_close_container(&iter, &array1);

	return spa_steal_ptr(r);
}

static DBusMessage *ag_properties_get(struct agimpl *agimpl, DBusMessage *m)
{
	const char *iface, *name;
	DBusMessage *r;
	DBusMessageIter i, v;

	if (!dbus_message_get_args(m, NULL,
				DBUS_TYPE_STRING, &iface,
				DBUS_TYPE_STRING, &name,
				DBUS_TYPE_INVALID))
		return NULL;

	if (spa_streq(iface, PW_TELEPHONY_AG_IFACE)) {
		if (spa_streq(name, "Address")) {
			r = dbus_message_new_method_return(m);
			if (r == NULL)
				return NULL;
			dbus_message_iter_init_append(r, &i);
			dbus_message_iter_open_container(&i, DBUS_TYPE_VARIANT,
					DBUS_TYPE_STRING_AS_STRING, &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING,
					&agimpl->this.address);
			dbus_message_iter_close_container(&i, &v);
			return r;
		} else if (spa_streq(name, "SpeakerVolume")) {
			r = dbus_message_new_method_return(m);
			if (r == NULL)
				return NULL;
			dbus_message_iter_init_append(r, &i);
			dbus_message_iter_open_container(&i, DBUS_TYPE_VARIANT,
					DBUS_TYPE_BYTE_AS_STRING, &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_BYTE,
					&agimpl->this.volume[SPA_BT_VOLUME_ID_RX]);
			dbus_message_iter_close_container(&i, &v);
			return r;
		} else if (spa_streq(name, "MicrophoneVolume")) {
			r = dbus_message_new_method_return(m);
			if (r == NULL)
				return NULL;
			dbus_message_iter_init_append(r, &i);
			dbus_message_iter_open_container(&i, DBUS_TYPE_VARIANT,
					DBUS_TYPE_BYTE_AS_STRING, &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_BYTE,
					&agimpl->this.volume[SPA_BT_VOLUME_ID_TX]);
			dbus_message_iter_close_container(&i, &v);
			return r;
		}
	} else if (spa_streq(iface, PW_TELEPHONY_AG_TRANSPORT_IFACE)) {
		if (spa_streq(name, "Codec")) {
			r = dbus_message_new_method_return(m);
			if (r == NULL)
				return NULL;
			dbus_message_iter_init_append(r, &i);
			dbus_message_iter_open_container(&i, DBUS_TYPE_VARIANT,
					DBUS_TYPE_BYTE_AS_STRING, &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_BYTE,
					&agimpl->this.transport.codec);
			dbus_message_iter_close_container(&i, &v);
			return r;
		} else if (spa_streq(name, "State")) {
			r = dbus_message_new_method_return(m);
			if (r == NULL)
				return NULL;
			dbus_message_iter_init_append(r, &i);
			dbus_message_iter_open_container(&i, DBUS_TYPE_VARIANT,
					DBUS_TYPE_STRING_AS_STRING, &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING,
					transport_state_to_string(agimpl->this.transport.state));
			dbus_message_iter_close_container(&i, &v);
			return r;
		} else if (spa_streq(name, "RejectSCO")) {
			r = dbus_message_new_method_return(m);
			if (r == NULL)
				return NULL;
			dbus_message_iter_init_append(r, &i);
			dbus_message_iter_open_container(&i, DBUS_TYPE_VARIANT,
					DBUS_TYPE_BOOLEAN_AS_STRING, &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN,
					&agimpl->this.transport.rejectSCO);
			dbus_message_iter_close_container(&i, &v);
			return r;
		}
	} else {
		return dbus_message_new_error(m, DBUS_ERROR_UNKNOWN_INTERFACE,
				"No such interface");
	}

	return dbus_message_new_error(m, DBUS_ERROR_UNKNOWN_PROPERTY,
			"No such property");
}

static DBusMessage *ag_properties_get_all(struct agimpl *agimpl, DBusMessage *m)
{
	DBusMessage *r;
	DBusMessageIter i;
	const char *iface;

	if (!dbus_message_get_args(m, NULL,
				DBUS_TYPE_STRING, &iface,
				DBUS_TYPE_INVALID))
		return NULL;

	if (spa_streq(iface, PW_TELEPHONY_AG_IFACE)) {
		r = dbus_message_new_method_return(m);
		if (r == NULL)
			return NULL;
		dbus_message_iter_init_append(r, &i);
		dbus_iter_append_ag_properties(&i, &agimpl->this, true);
		return r;
	} else if (spa_streq(iface, PW_TELEPHONY_AG_TRANSPORT_IFACE)) {
		r = dbus_message_new_method_return(m);
		if (r == NULL)
			return NULL;
		dbus_message_iter_init_append(r, &i);
		dbus_iter_append_ag_transport_properties(&i, &agimpl->this, true);
		return r;
	} else {
		return dbus_message_new_error(m, DBUS_ERROR_UNKNOWN_INTERFACE,
				"No such interface");
	}
}

static DBusMessage *ag_properties_set(struct agimpl *agimpl, DBusMessage *m)
{
	const char *iface, *name;
	DBusMessageIter i, variant;

	if (!dbus_message_get_args(m, NULL,
				DBUS_TYPE_STRING, &iface,
				DBUS_TYPE_STRING, &name,
				DBUS_TYPE_INVALID))
		return NULL;

	if (spa_streq(iface, PW_TELEPHONY_AG_IFACE)) {
		if (spa_streq(name, "SpeakerVolume")) {
			dbus_message_iter_init(m, &i);
			dbus_message_iter_next(&i); /* skip iface */
			dbus_message_iter_next(&i); /* skip name */
			dbus_message_iter_recurse(&i, &variant); /* value */
			dbus_message_iter_get_basic(&variant, &agimpl->this.volume[SPA_BT_VOLUME_ID_RX]);

			return ag_emit_set_speaker_volume(agimpl, agimpl->this.volume[SPA_BT_VOLUME_ID_RX], m) ? NULL :
				dbus_message_new_error(m, telephony_error_to_dbus (BT_TELEPHONY_ERROR_FAILED),
					telephony_error_to_description (BT_TELEPHONY_ERROR_FAILED, 0));

		} else if (spa_streq(name, "MicrophoneVolume")) {
			dbus_message_iter_init(m, &i);
			dbus_message_iter_next(&i); /* skip iface */
			dbus_message_iter_next(&i); /* skip name */
			dbus_message_iter_recurse(&i, &variant); /* value */
			dbus_message_iter_get_basic(&variant, &agimpl->this.volume[SPA_BT_VOLUME_ID_TX]);

			return ag_emit_set_microphone_volume(agimpl, agimpl->this.volume[SPA_BT_VOLUME_ID_TX], m) ? NULL :
				dbus_message_new_error(m, telephony_error_to_dbus (BT_TELEPHONY_ERROR_FAILED),
					telephony_error_to_description (BT_TELEPHONY_ERROR_FAILED, 0));
		}
	} else if (spa_streq(iface, PW_TELEPHONY_AG_TRANSPORT_IFACE)) {
		if (spa_streq(name, "RejectSCO")) {
			dbus_message_iter_init(m, &i);
			dbus_message_iter_next(&i); /* skip iface */
			dbus_message_iter_next(&i); /* skip name */
			dbus_message_iter_recurse(&i, &variant); /* value */
			dbus_message_iter_get_basic(&variant, &agimpl->this.transport.rejectSCO);
			return dbus_message_new_method_return(m);
		}
	}

	return dbus_message_new_error(m, DBUS_ERROR_PROPERTY_READ_ONLY,
			"Property not writable");
}

static bool validate_phone_number(const char *number)
{
	const char *c;
	int count = 0;

	if (!number)
		return false;
	for (c = number; *c != '\0'; c++) {
		if (!(*c >= '0' && *c <= '9') && !(*c >= 'A' && *c <= 'D') &&
			*c != '#' && *c != '*' && *c != '+' && *c != ',' )
			return false;
		count++;
	}
	if (count < 1 || count > 80)
		return false;
	return true;
}

static bool validate_tones(const char *tones)
{
	const char *c;
	if (!tones)
		return false;
	for (c = tones; *c != '\0'; c++) {
		if (!(*c >= '0' && *c <= '9') && !(*c >= 'A' && *c <= 'D') &&
				*c != '#' && *c != '*')
			return false;
	}
	return true;
}

static DBusMessage *ag_dial(struct agimpl *agimpl, DBusMessage *m)
{
	const char *number = NULL;
	enum spa_bt_telephony_error err = BT_TELEPHONY_ERROR_FAILED;

	if (!dbus_message_get_args(m, NULL,
				DBUS_TYPE_STRING, &number,
				DBUS_TYPE_INVALID))
		return NULL;

	if (!validate_phone_number(number)) {
		err = BT_TELEPHONY_ERROR_INVALID_FORMAT;
		goto failed;
	}

	if (ag_emit_dial(agimpl, number, m))
		return NULL;

failed:
	return dbus_message_new_error(m, telephony_error_to_dbus (err),
		telephony_error_to_description (err, 0));
}

static DBusMessage *ag_swap_calls(struct agimpl *agimpl, DBusMessage *m)
{
	return ag_emit_swap_calls(agimpl, m) ? NULL :
		dbus_message_new_error(m, telephony_error_to_dbus (BT_TELEPHONY_ERROR_FAILED),
			telephony_error_to_description (BT_TELEPHONY_ERROR_FAILED, 0));
}

static DBusMessage *ag_release_and_answer(struct agimpl *agimpl, DBusMessage *m)
{
	return ag_emit_release_and_answer(agimpl, m) ? NULL :
		dbus_message_new_error(m, telephony_error_to_dbus (BT_TELEPHONY_ERROR_FAILED),
			telephony_error_to_description (BT_TELEPHONY_ERROR_FAILED, 0));
}

static DBusMessage *ag_release_and_swap(struct agimpl *agimpl, DBusMessage *m)
{
	return ag_emit_release_and_swap(agimpl, m) ? NULL :
		dbus_message_new_error(m, telephony_error_to_dbus (BT_TELEPHONY_ERROR_FAILED),
			telephony_error_to_description (BT_TELEPHONY_ERROR_FAILED, 0));
}

static DBusMessage *ag_hold_and_answer(struct agimpl *agimpl, DBusMessage *m)
{
	return ag_emit_hold_and_answer(agimpl, m) ? NULL :
		dbus_message_new_error(m, telephony_error_to_dbus (BT_TELEPHONY_ERROR_FAILED),
			telephony_error_to_description (BT_TELEPHONY_ERROR_FAILED, 0));
}

static DBusMessage *ag_hangup_all(struct agimpl *agimpl, DBusMessage *m)
{
	return ag_emit_hangup_all(agimpl, m) ? NULL :
		dbus_message_new_error(m, telephony_error_to_dbus (BT_TELEPHONY_ERROR_FAILED),
			telephony_error_to_description (BT_TELEPHONY_ERROR_FAILED, 0));
}

static DBusMessage *ag_create_multiparty(struct agimpl *agimpl, DBusMessage *m)
{
	return ag_emit_create_multiparty(agimpl, m) ? NULL :
		dbus_message_new_error(m, telephony_error_to_dbus (BT_TELEPHONY_ERROR_FAILED),
			telephony_error_to_description (BT_TELEPHONY_ERROR_FAILED, 0));
}

static DBusMessage *ag_send_tones(struct agimpl *agimpl, DBusMessage *m)
{
	const char *tones = NULL;
	enum spa_bt_telephony_error err = BT_TELEPHONY_ERROR_FAILED;

	if (!dbus_message_get_args(m, NULL,
				DBUS_TYPE_STRING, &tones,
				DBUS_TYPE_INVALID))
		return NULL;

	if (!validate_tones(tones)) {
		err = BT_TELEPHONY_ERROR_INVALID_FORMAT;
		goto failed;
	}

	if (ag_emit_send_tones(agimpl, tones, m))
		return NULL;

failed:
	return dbus_message_new_error(m, telephony_error_to_dbus (err),
		telephony_error_to_description (err, 0));
}

static DBusMessage *ag_transport_activate(struct agimpl *agimpl, DBusMessage *m)
{
	return ag_emit_transport_activate(agimpl, m) ? NULL :
		dbus_message_new_error(m, telephony_error_to_dbus (BT_TELEPHONY_ERROR_FAILED),
			telephony_error_to_description (BT_TELEPHONY_ERROR_FAILED, 0));
}

static DBusHandlerResult ag_handler(DBusConnection *c, DBusMessage *m, void *userdata)
{
	struct agimpl *agimpl = userdata;
	struct impl *impl = SPA_CONTAINER_OF(agimpl->this.telephony, struct impl, this);

	spa_autoptr(DBusMessage) r = NULL;
	const char *path, *interface, *member;

	path = dbus_message_get_path(m);
	interface = dbus_message_get_interface(m);
	member = dbus_message_get_member(m);

	spa_log_debug(impl->log, "dbus: path=%s, interface=%s, member=%s", path, interface, member);

	if (dbus_message_is_method_call(m, DBUS_INTERFACE_INTROSPECTABLE, "Introspect")) {
		r = ag_introspect(agimpl, m);
	} else if (dbus_message_is_method_call(m, DBUS_INTERFACE_OBJECT_MANAGER, "GetManagedObjects")) {
		r = ag_get_managed_objects(agimpl, m, false);
	} else if (dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "Get")) {
		r = ag_properties_get(agimpl, m);
	} else if (dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "GetAll")) {
		r = ag_properties_get_all(agimpl, m);
	} else if (dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "Set")) {
		r = ag_properties_set(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "Dial") ||
		   dbus_message_is_method_call(m, OFONO_VOICE_CALL_MANAGER_IFACE, "Dial")) {
		r = ag_dial(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "SwapCalls") ||
		   dbus_message_is_method_call(m, OFONO_VOICE_CALL_MANAGER_IFACE, "SwapCalls")) {
		r = ag_swap_calls(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "ReleaseAndAnswer") ||
		   dbus_message_is_method_call(m, OFONO_VOICE_CALL_MANAGER_IFACE, "ReleaseAndAnswer")) {
		r = ag_release_and_answer(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "ReleaseAndSwap") ||
		   dbus_message_is_method_call(m, OFONO_VOICE_CALL_MANAGER_IFACE, "ReleaseAndSwap")) {
		r = ag_release_and_swap(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "HoldAndAnswer") ||
		   dbus_message_is_method_call(m, OFONO_VOICE_CALL_MANAGER_IFACE, "HoldAndAnswer")) {
		r = ag_hold_and_answer(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "HangupAll") ||
		   dbus_message_is_method_call(m, OFONO_VOICE_CALL_MANAGER_IFACE, "HangupAll")) {
		r = ag_hangup_all(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "CreateMultiparty") ||
		   dbus_message_is_method_call(m, OFONO_VOICE_CALL_MANAGER_IFACE, "CreateMultiparty")) {
		r = ag_create_multiparty(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "SendTones") ||
		   dbus_message_is_method_call(m, OFONO_VOICE_CALL_MANAGER_IFACE, "SendTones")) {
		r = ag_send_tones(agimpl, m);
	} else if (dbus_message_is_method_call(m, OFONO_VOICE_CALL_MANAGER_IFACE, "GetCalls")) {
		r = ag_get_managed_objects(agimpl, m, true);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_TRANSPORT_IFACE, "Activate")) {
		r = ag_transport_activate(agimpl, m);
	} else {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (r && !dbus_connection_send(impl->conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	return DBUS_HANDLER_RESULT_HANDLED;
}

struct spa_bt_telephony_ag *
telephony_ag_new(struct spa_bt_telephony *telephony, size_t user_data_size)
{
	struct impl *impl = SPA_CONTAINER_OF(telephony, struct impl, this);
	struct agimpl *agimpl;

	spa_assert(user_data_size < SIZE_MAX - sizeof(*agimpl));

	agimpl = calloc(1, sizeof(*agimpl) + user_data_size);
	if (agimpl == NULL)
		return NULL;

	agimpl->this.telephony = telephony;
	agimpl->this.id = find_free_object_id(&impl->ag_list, struct agimpl, link);
	spa_list_init(&agimpl->this.call_list);

	spa_list_append(&impl->ag_list, &agimpl->link);

	if (user_data_size > 0)
		agimpl->user_data = SPA_PTROFF(agimpl, sizeof(struct agimpl), void);

	agimpl->this.transport.rejectSCO = impl->default_reject_sco;

	return &agimpl->this;
}

void telephony_ag_destroy(struct spa_bt_telephony_ag *ag)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	struct callimpl *callimpl;

	spa_list_consume (callimpl, &agimpl->this.call_list, this.link) {
		telephony_call_destroy(&callimpl->this);
	}

	telephony_ag_unregister(ag);
	spa_list_remove(&agimpl->link);

	free(ag->address);

	free(agimpl);
}

void *telephony_ag_get_user_data(struct spa_bt_telephony_ag *ag)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	return agimpl->user_data;
}

int telephony_ag_register(struct spa_bt_telephony_ag *ag)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	struct impl *impl = SPA_CONTAINER_OF(agimpl->this.telephony, struct impl, this);

	static const DBusObjectPathVTable vtable = {
		.message_function = ag_handler,
	};

	spa_autofree char *path = spa_aprintf(PW_TELEPHONY_OBJECT_PATH "/ag%d", agimpl->this.id);

	/* register object */
	if (!dbus_connection_register_object_path(impl->conn, path, &vtable, agimpl)) {
		spa_log_error(impl->log, "failed to register %s", path);
		return -EIO;
	}

	agimpl->path = spa_steal_ptr(path);

	/* notify on ObjectManager of the Manager object */
	{
		spa_autoptr(DBusMessage) msg = NULL;
		DBusMessageIter iter;

		msg = dbus_message_new_signal(impl->path, DBUS_INTERFACE_OBJECT_MANAGER,
						"InterfacesAdded");
		dbus_message_iter_init_append(msg, &iter);
		dbus_iter_append_ag_interfaces(&iter, ag);

		if (!dbus_connection_send(impl->conn, msg, NULL)) {
			spa_log_error(impl->log, "failed to send InterfacesAdded for %s", agimpl->path);
			telephony_ag_unregister(ag);
			return -EIO;
		}
	}

	/* emit ModemAdded on the Manager object */
	{
		spa_autoptr(DBusMessage) msg = NULL;
		DBusMessageIter iter, props_dict;

		msg = dbus_message_new_signal(impl->path, OFONO_MANAGER_IFACE,
						"ModemAdded");
		dbus_message_iter_init_append(msg, &iter);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &agimpl->path);
		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &props_dict);
		dbus_message_iter_close_container(&iter, &props_dict);

		if (!dbus_connection_send(impl->conn, msg, NULL)) {
			spa_log_error(impl->log, "failed to send ModemAdded for %s", agimpl->path);
			telephony_ag_unregister(ag);
			return -EIO;
		}
	}

	spa_log_debug(impl->log, "registered AudioGateway: %s", agimpl->path);

	return 0;
}

void telephony_ag_unregister(struct spa_bt_telephony_ag *ag)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	struct impl *impl = SPA_CONTAINER_OF(agimpl->this.telephony, struct impl, this);

	if (!agimpl->path)
		return;

	spa_log_debug(impl->log, "removing AudioGateway: %s", agimpl->path);

	{
		spa_autoptr(DBusMessage) msg = NULL;
		DBusMessageIter iter, entry;
		const char *interface = PW_TELEPHONY_AG_IFACE;
		const char *interface2 = PW_TELEPHONY_AG_TRANSPORT_IFACE;

		msg = dbus_message_new_signal(impl->path, DBUS_INTERFACE_OBJECT_MANAGER,
						"InterfacesRemoved");
		dbus_message_iter_init_append(msg, &iter);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &agimpl->path);
		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
						DBUS_TYPE_STRING_AS_STRING, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &interface);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &interface2);
		dbus_message_iter_close_container(&iter, &entry);

		if (!dbus_connection_send(impl->conn, msg, NULL)) {
			spa_log_warn(impl->log, "sending InterfacesRemoved failed");
		}
	}
	{
		spa_autoptr(DBusMessage) msg = NULL;
		DBusMessageIter iter;

		msg = dbus_message_new_signal(impl->path, OFONO_MANAGER_IFACE,
						"ModemRemoved");
		dbus_message_iter_init_append(msg, &iter);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &agimpl->path);

		if (!dbus_connection_send(impl->conn, msg, NULL)) {
			spa_log_warn(impl->log, "sending ModemRemoved failed");
		}
	}

	if (!dbus_connection_unregister_object_path(impl->conn, agimpl->path)) {
		spa_log_warn(impl->log, "failed to unregister %s", agimpl->path);
	}

	free(agimpl->path);
	agimpl->path = NULL;
}

/* send message to notify about volume property changes */
void telephony_ag_notify_updated_props(struct spa_bt_telephony_ag *ag)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	struct impl *impl = SPA_CONTAINER_OF(agimpl->this.telephony, struct impl, this);

	spa_autoptr(DBusMessage) msg = NULL;
	const char *interface = PW_TELEPHONY_AG_IFACE;
	DBusMessageIter i, a;

	msg = dbus_message_new_signal(agimpl->path,
				DBUS_INTERFACE_PROPERTIES,
				"PropertiesChanged");

	dbus_message_iter_init_append(msg, &i);
	dbus_message_iter_append_basic(&i, DBUS_TYPE_STRING, &interface);

	if (!dbus_iter_append_ag_properties(&i, ag, false))
		return;

	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY,
			DBUS_TYPE_STRING_AS_STRING, &a);
	dbus_message_iter_close_container(&i, &a);

	if (!dbus_connection_send(impl->conn, msg, NULL)){
		spa_log_warn(impl->log, "sending PropertiesChanged failed");
	}

	telephony_ag_commit_properties(ag);
}

/* send message to notify about transport property changes */
void telephony_ag_transport_notify_updated_props(struct spa_bt_telephony_ag *ag)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	struct impl *impl = SPA_CONTAINER_OF(agimpl->this.telephony, struct impl, this);

	spa_autoptr(DBusMessage) msg = NULL;
	const char *interface = PW_TELEPHONY_AG_TRANSPORT_IFACE;
	DBusMessageIter i, a;

	msg = dbus_message_new_signal(agimpl->path,
				DBUS_INTERFACE_PROPERTIES,
				"PropertiesChanged");

	dbus_message_iter_init_append(msg, &i);
	dbus_message_iter_append_basic(&i, DBUS_TYPE_STRING, &interface);

	if (!dbus_iter_append_ag_transport_properties(&i, ag, false))
		return;

	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY,
			DBUS_TYPE_STRING_AS_STRING, &a);
	dbus_message_iter_close_container(&i, &a);

	if (!dbus_connection_send(impl->conn, msg, NULL)){
		spa_log_warn(impl->log, "sending PropertiesChanged failed");
	}

	telephony_ag_transport_commit_properties(ag);
}

void telephony_ag_set_callbacks(struct spa_bt_telephony_ag *ag,
	const struct spa_bt_telephony_ag_callbacks *cbs,
	void *data)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	agimpl->callbacks.funcs = cbs;
	agimpl->callbacks.data = data;
}

struct spa_bt_telephony_call *
telephony_call_new(struct spa_bt_telephony_ag *ag, size_t user_data_size)
{
	struct callimpl *callimpl;

	spa_assert(user_data_size < SIZE_MAX - sizeof(*callimpl));

	callimpl = calloc(1, sizeof(*callimpl) + user_data_size);
	if (callimpl == NULL)
		return NULL;

	callimpl->this.ag = ag;
	callimpl->this.id = find_free_object_id(&ag->call_list, struct callimpl, this.link);

	spa_list_append(&ag->call_list, &callimpl->this.link);

	if (user_data_size > 0)
		callimpl->user_data = SPA_PTROFF(callimpl, sizeof(struct callimpl), void);

	return &callimpl->this;
}

void telephony_call_destroy(struct spa_bt_telephony_call *call)
{
	struct callimpl *callimpl = SPA_CONTAINER_OF(call, struct callimpl, this);

	telephony_call_unregister(call);
	spa_list_remove(&call->link);

	free(callimpl->prev.line_identification);
	free(callimpl->prev.incoming_line);
	free(callimpl->prev.name);

	free(call->line_identification);
	free(call->incoming_line);
	free(call->name);

	free(callimpl);
}

void *telephony_call_get_user_data(struct spa_bt_telephony_call *call)
{
	struct callimpl *callimpl = SPA_CONTAINER_OF(call, struct callimpl, this);
	return callimpl->user_data;
}

static void telephony_call_commit_properties(struct spa_bt_telephony_call *call)
{
	struct callimpl *callimpl = SPA_CONTAINER_OF(call, struct callimpl, this);

	if (!spa_streq (call->line_identification, callimpl->prev.line_identification)) {
		free(callimpl->prev.line_identification);
		callimpl->prev.line_identification = call->line_identification ?
			strdup (call->line_identification) : NULL;
	}
	if (!spa_streq (call->incoming_line, callimpl->prev.incoming_line)) {
		free(callimpl->prev.incoming_line);
		callimpl->prev.incoming_line = call->incoming_line ?
			strdup (call->incoming_line) : NULL;
	}
	if (!spa_streq (call->name, callimpl->prev.name)) {
		free(callimpl->prev.name);
		callimpl->prev.name = call->name ? strdup (call->name) : NULL;
	}
	callimpl->prev.multiparty = call->multiparty;
	callimpl->prev.state = call->state;
}

static const char * const call_state_to_string[] = {
	"active",
	"held",
	"dialing",
	"alerting",
	"incoming",
	"waiting",
	"disconnected",
};

static inline const void *safe_string(char **str)
{
	static const char *empty_string = "";
	return *str ? (const char **) str : &empty_string;
}

static void
dbus_iter_append_call_properties(DBusMessageIter *i, struct spa_bt_telephony_call *call, bool all)
{
	struct callimpl *callimpl = SPA_CONTAINER_OF(call, struct callimpl, this);
	DBusMessageIter dict, entry, variant;

	dbus_message_iter_open_container(i, DBUS_TYPE_ARRAY, "{sv}", &dict);

	if (all || !spa_streq (call->line_identification, callimpl->prev.line_identification)) {
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL,
						&entry);
		const char *line_identification = "LineIdentification";
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &line_identification);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_STRING_AS_STRING, &variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING,
						safe_string (&call->line_identification));
		dbus_message_iter_close_container(&entry, &variant);
		dbus_message_iter_close_container(&dict, &entry);
	}

	if (all || !spa_streq (call->incoming_line, callimpl->prev.incoming_line)) {
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
		const char *incoming_line = "IncomingLine";
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &incoming_line);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_STRING_AS_STRING,
						&variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING,
						safe_string (&call->incoming_line));
		dbus_message_iter_close_container(&entry, &variant);
		dbus_message_iter_close_container(&dict, &entry);
	}

	if (all || !spa_streq (call->name, callimpl->prev.name)) {
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
		const char *name = "Name";
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_STRING_AS_STRING,
						&variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING,
						safe_string (&call->name));
		dbus_message_iter_close_container(&entry, &variant);
		dbus_message_iter_close_container(&dict, &entry);
	}

	if (all || call->multiparty != callimpl->prev.multiparty) {
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
		const char *multiparty = "Multiparty";
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &multiparty);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_BOOLEAN_AS_STRING,
						&variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &call->multiparty);
		dbus_message_iter_close_container(&entry, &variant);
		dbus_message_iter_close_container(&dict, &entry);
	}

	if (all || call->state != callimpl->prev.state) {
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
		const char *state = "State";
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &state);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_STRING_AS_STRING,
						&variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING,
						&call_state_to_string[call->state]);
		dbus_message_iter_close_container(&entry, &variant);
		dbus_message_iter_close_container(&dict, &entry);
	}

	dbus_message_iter_close_container(i, &dict);
}

static DBusMessage *call_introspect(struct callimpl *callimpl, DBusMessage *m)
{
	const char *xml = PW_TELEPHONY_CALL_INTROSPECT_XML;
	spa_autoptr(DBusMessage) r = NULL;
	if ((r = dbus_message_new_method_return(m)) == NULL)
		return NULL;
	if (!dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID))
		return NULL;
	return spa_steal_ptr(r);
}

static DBusMessage *call_properties_get(struct callimpl *callimpl, DBusMessage *m)
{
	const char *iface, *name;
	DBusMessage *r;
	DBusMessageIter i, v;

	if (!dbus_message_get_args(m, NULL,
				DBUS_TYPE_STRING, &iface,
				DBUS_TYPE_STRING, &name,
				DBUS_TYPE_INVALID))
		return NULL;

	if (spa_streq(iface, PW_TELEPHONY_CALL_IFACE))
		return dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS,
				"No such interface");

	if (spa_streq(name, "Multiparty")) {
		r = dbus_message_new_method_return(m);
		if (r == NULL)
			return NULL;
		dbus_message_iter_init_append(r, &i);
		dbus_message_iter_open_container(&i, DBUS_TYPE_VARIANT,
				DBUS_TYPE_BOOLEAN_AS_STRING, &v);
		dbus_message_iter_append_basic(&v, DBUS_TYPE_BOOLEAN,
				&callimpl->this.multiparty);
		dbus_message_iter_close_container(&i, &v);
		return r;
	} else {
		const char * const *property = NULL;
		if (spa_streq(name, "LineIdentification")) {
			property = (const char * const *) &callimpl->this.line_identification;
		} else if (spa_streq(name, "IncomingLine")) {
			property = (const char * const *) &callimpl->this.incoming_line;
		} else if (spa_streq(name, "Name")) {
			property = (const char * const *) &callimpl->this.name;
		} else if (spa_streq(name, "State")) {
			property = &call_state_to_string[callimpl->this.state];
		}

		if (property) {
			r = dbus_message_new_method_return(m);
			if (r == NULL)
				return NULL;
			dbus_message_iter_init_append(r, &i);
			dbus_message_iter_open_container(&i, DBUS_TYPE_VARIANT,
					DBUS_TYPE_STRING_AS_STRING, &v);
			dbus_message_iter_append_basic(&v, DBUS_TYPE_STRING,
					property);
			dbus_message_iter_close_container(&i, &v);
			return r;
		}
	}

	return dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS,
			"No such property");
}

static DBusMessage *call_properties_get_all(struct callimpl *callimpl, DBusMessage *m, bool ofono_compat)
{
	DBusMessage *r;
	DBusMessageIter i;

	if (!ofono_compat) {
		const char *iface;

		if (!dbus_message_get_args(m, NULL,
					DBUS_TYPE_STRING, &iface,
					DBUS_TYPE_INVALID))
			return NULL;

		if (!spa_streq(iface, PW_TELEPHONY_CALL_IFACE))
			return dbus_message_new_error(m, DBUS_ERROR_UNKNOWN_INTERFACE,
					"No such interface");
	}

	r = dbus_message_new_method_return(m);
	if (r == NULL)
		return NULL;

	dbus_message_iter_init_append(r, &i);
	dbus_iter_append_call_properties(&i, &callimpl->this, true);
	return r;
}

static DBusMessage *call_properties_set(struct callimpl *callimpl, DBusMessage *m)
{
	return dbus_message_new_error(m, DBUS_ERROR_PROPERTY_READ_ONLY,
			"Property not writable");
}

static DBusMessage *call_answer(struct callimpl *callimpl, DBusMessage *m)
{
	return call_emit_answer(callimpl, m) ? NULL :
		dbus_message_new_error(m, telephony_error_to_dbus (BT_TELEPHONY_ERROR_FAILED),
			telephony_error_to_description (BT_TELEPHONY_ERROR_FAILED, 0));
}

static DBusMessage *call_hangup(struct callimpl *callimpl, DBusMessage *m)
{
	return call_emit_hangup(callimpl, m) ? NULL :
		dbus_message_new_error(m, telephony_error_to_dbus (BT_TELEPHONY_ERROR_FAILED),
			telephony_error_to_description (BT_TELEPHONY_ERROR_FAILED, 0));
}

static DBusHandlerResult call_handler(DBusConnection *c, DBusMessage *m, void *userdata)
{
	struct callimpl *callimpl = userdata;
	struct agimpl *agimpl = SPA_CONTAINER_OF(callimpl->this.ag, struct agimpl, this);
	struct impl *impl = SPA_CONTAINER_OF(agimpl->this.telephony, struct impl, this);

	spa_autoptr(DBusMessage) r = NULL;
	const char *path, *interface, *member;

	path = dbus_message_get_path(m);
	interface = dbus_message_get_interface(m);
	member = dbus_message_get_member(m);

	spa_log_debug(impl->log, "dbus: path=%s, interface=%s, member=%s", path, interface, member);

	if (dbus_message_is_method_call(m, DBUS_INTERFACE_INTROSPECTABLE, "Introspect")) {
		r = call_introspect(callimpl, m);
	} else if (dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "Get")) {
		r = call_properties_get(callimpl, m);
	} else if (dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "GetAll")) {
		r = call_properties_get_all(callimpl, m, false);
	} else if (dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "Set")) {
		r = call_properties_set(callimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_CALL_IFACE, "Answer") ||
		   dbus_message_is_method_call(m, OFONO_VOICE_CALL_IFACE, "Answer")) {
		r = call_answer(callimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_CALL_IFACE, "Hangup") ||
		   dbus_message_is_method_call(m, OFONO_VOICE_CALL_IFACE, "Hangup")) {
		r = call_hangup(callimpl, m);
	} else if (dbus_message_is_method_call(m, OFONO_VOICE_CALL_IFACE, "GetProperties")) {
		r = call_properties_get_all(callimpl, m, true);
	} else {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (r && !dbus_connection_send(impl->conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	return DBUS_HANDLER_RESULT_HANDLED;
}

int telephony_call_register(struct spa_bt_telephony_call *call)
{
	struct callimpl *callimpl = SPA_CONTAINER_OF(call, struct callimpl, this);
	struct agimpl *agimpl = SPA_CONTAINER_OF(callimpl->this.ag, struct agimpl, this);
	struct impl *impl = SPA_CONTAINER_OF(agimpl->this.telephony, struct impl, this);

	static const DBusObjectPathVTable vtable = {
		.message_function = call_handler,
	};

	spa_autofree char *path = spa_aprintf("%s/call%d", agimpl->path, callimpl->this.id);

	/* register object */
	if (!dbus_connection_register_object_path(impl->conn, path, &vtable, callimpl)) {
		spa_log_error(impl->log, "failed to register %s", path);
		return -EIO;
	}

	callimpl->path = spa_steal_ptr(path);

	/* notify on ObjectManager of the AudioGateway object */
	{
		spa_autoptr(DBusMessage) msg = NULL;
		DBusMessageIter iter, entry, dict;
		const char *interface = PW_TELEPHONY_CALL_IFACE;

		msg = dbus_message_new_signal(agimpl->path,
					DBUS_INTERFACE_OBJECT_MANAGER,
					"InterfacesAdded");
		dbus_message_iter_init_append(msg, &iter);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &callimpl->path);
		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sa{sv}}", &dict);
		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &interface);
		dbus_iter_append_call_properties(&entry, call, true);
		dbus_message_iter_close_container(&dict, &entry);
		dbus_message_iter_close_container(&iter, &dict);

		if (!dbus_connection_send(impl->conn, msg, NULL)) {
			spa_log_error(impl->log, "failed to send InterfacesAdded for %s", callimpl->path);
			telephony_call_unregister(call);
			return -EIO;
		}
	}

	/* emit CallAdded on the AudioGateway object */
	{
		spa_autoptr(DBusMessage) msg = NULL;
		DBusMessageIter iter;

		msg = dbus_message_new_signal(agimpl->path,
					OFONO_VOICE_CALL_MANAGER_IFACE,
					"CallAdded");
		dbus_message_iter_init_append(msg, &iter);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &callimpl->path);
		dbus_iter_append_call_properties(&iter, call, true);

		if (!dbus_connection_send(impl->conn, msg, NULL)) {
			spa_log_error(impl->log, "failed to send CallAdded for %s", callimpl->path);
			telephony_call_unregister(call);
			return -EIO;
		}
	}

	telephony_call_commit_properties(call);

	spa_log_debug(impl->log, "registered Call: %s", callimpl->path);

	return 0;
}

void telephony_call_unregister(struct spa_bt_telephony_call *call)
{
	struct callimpl *callimpl = SPA_CONTAINER_OF(call, struct callimpl, this);
	struct agimpl *agimpl = SPA_CONTAINER_OF(callimpl->this.ag, struct agimpl, this);
	struct impl *impl = SPA_CONTAINER_OF(agimpl->this.telephony, struct impl, this);

	if (!callimpl->path)
		return;

	spa_log_debug(impl->log, "removing Call: %s", callimpl->path);

	{
		spa_autoptr(DBusMessage) msg = NULL;
		DBusMessageIter iter, entry;
		const char *interface = PW_TELEPHONY_CALL_IFACE;

		msg = dbus_message_new_signal(agimpl->path,
					DBUS_INTERFACE_OBJECT_MANAGER,
					"InterfacesRemoved");
		dbus_message_iter_init_append(msg, &iter);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &callimpl->path);
		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
						DBUS_TYPE_STRING_AS_STRING, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &interface);
		dbus_message_iter_close_container(&iter, &entry);

		if (!dbus_connection_send(impl->conn, msg, NULL)) {
			spa_log_warn(impl->log, "sending InterfacesRemoved failed");
		}
	}
	{
		spa_autoptr(DBusMessage) msg = NULL;
		DBusMessageIter iter;

		msg = dbus_message_new_signal(agimpl->path,
					OFONO_VOICE_CALL_MANAGER_IFACE,
					"CallRemoved");
		dbus_message_iter_init_append(msg, &iter);
		dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &callimpl->path);

		if (!dbus_connection_send(impl->conn, msg, NULL)) {
			spa_log_warn(impl->log, "sending CallRemoved failed");
		}
	}

	if (!dbus_connection_unregister_object_path(impl->conn, callimpl->path)) {
		spa_log_warn(impl->log, "failed to unregister %s", callimpl->path);
	}

	free(callimpl->path);
	callimpl->path = NULL;
}

/* send message to notify about property changes */
void telephony_call_notify_updated_props(struct spa_bt_telephony_call *call)
{
	struct callimpl *callimpl = SPA_CONTAINER_OF(call, struct callimpl, this);
	struct agimpl *agimpl = SPA_CONTAINER_OF(callimpl->this.ag, struct agimpl, this);
	struct impl *impl = SPA_CONTAINER_OF(agimpl->this.telephony, struct impl, this);

	{
		spa_autoptr(DBusMessage) msg = NULL;
		const char *interface = PW_TELEPHONY_CALL_IFACE;
		DBusMessageIter i, a;

		msg = dbus_message_new_signal(callimpl->path,
					DBUS_INTERFACE_PROPERTIES,
					"PropertiesChanged");

		dbus_message_iter_init_append(msg, &i);
		dbus_message_iter_append_basic(&i, DBUS_TYPE_STRING, &interface);

		dbus_iter_append_call_properties(&i, call, false);

		dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY,
				DBUS_TYPE_STRING_AS_STRING, &a);
		dbus_message_iter_close_container(&i, &a);

		if (!dbus_connection_send(impl->conn, msg, NULL)){
			spa_log_warn(impl->log, "sending PropertiesChanged failed");
		}
	}

	if (!spa_streq (call->line_identification, callimpl->prev.line_identification)) {
		spa_autoptr(DBusMessage) msg = NULL;
		DBusMessageIter entry, variant;

		msg = dbus_message_new_signal(callimpl->path,
					OFONO_VOICE_CALL_IFACE,
					"PropertyChanged");

		const char *line_identification = "LineIdentification";
		dbus_message_iter_init_append(msg, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &line_identification);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_STRING_AS_STRING, &variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING,
						safe_string (&call->line_identification));
		dbus_message_iter_close_container(&entry, &variant);

		if (!dbus_connection_send(impl->conn, msg, NULL)){
			spa_log_warn(impl->log, "sending PropertyChanged failed");
		}
	}

	if (!spa_streq (call->incoming_line, callimpl->prev.incoming_line)) {
		spa_autoptr(DBusMessage) msg = NULL;
		DBusMessageIter entry, variant;

		msg = dbus_message_new_signal(callimpl->path,
					OFONO_VOICE_CALL_IFACE,
					"PropertyChanged");

		const char *incoming_line = "IncomingLine";
		dbus_message_iter_init_append(msg, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &incoming_line);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_STRING_AS_STRING,
						&variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING,
						safe_string (&call->incoming_line));
		dbus_message_iter_close_container(&entry, &variant);

		if (!dbus_connection_send(impl->conn, msg, NULL)){
			spa_log_warn(impl->log, "sending PropertyChanged failed");
		}
	}

	if (!spa_streq (call->name, callimpl->prev.name)) {
		spa_autoptr(DBusMessage) msg = NULL;
		DBusMessageIter entry, variant;

		msg = dbus_message_new_signal(callimpl->path,
					OFONO_VOICE_CALL_IFACE,
					"PropertyChanged");

		const char *name = "Name";
		dbus_message_iter_init_append(msg, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_STRING_AS_STRING,
						&variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING,
						safe_string (&call->name));
		dbus_message_iter_close_container(&entry, &variant);

		if (!dbus_connection_send(impl->conn, msg, NULL)){
			spa_log_warn(impl->log, "sending PropertyChanged failed");
		}
	}

	if (call->multiparty != callimpl->prev.multiparty) {
		spa_autoptr(DBusMessage) msg = NULL;
		DBusMessageIter entry, variant;

		msg = dbus_message_new_signal(callimpl->path,
					OFONO_VOICE_CALL_IFACE,
					"PropertyChanged");

		const char *multiparty = "Multiparty";
		dbus_message_iter_init_append(msg, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &multiparty);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_BOOLEAN_AS_STRING,
						&variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &call->multiparty);
		dbus_message_iter_close_container(&entry, &variant);

		if (!dbus_connection_send(impl->conn, msg, NULL)){
			spa_log_warn(impl->log, "sending PropertyChanged failed");
		}
	}

	if (call->state != callimpl->prev.state) {
		spa_autoptr(DBusMessage) msg = NULL;
		DBusMessageIter entry, variant;

		msg = dbus_message_new_signal(callimpl->path,
					OFONO_VOICE_CALL_IFACE,
					"PropertyChanged");

		const char *state = "State";
		dbus_message_iter_init_append(msg, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &state);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
						DBUS_TYPE_STRING_AS_STRING,
						&variant);
		dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING,
						&call_state_to_string[call->state]);
		dbus_message_iter_close_container(&entry, &variant);

		if (!dbus_connection_send(impl->conn, msg, NULL)){
			spa_log_warn(impl->log, "sending PropertyChanged failed");
		}
	}

	telephony_call_commit_properties(call);
}

void telephony_call_set_callbacks(struct spa_bt_telephony_call *call,
	const struct spa_bt_telephony_call_callbacks *cbs,
	void *data)
{
	struct callimpl *callimpl = SPA_CONTAINER_OF(call, struct callimpl, this);
	callimpl->callbacks.funcs = cbs;
	callimpl->callbacks.data = data;
}
