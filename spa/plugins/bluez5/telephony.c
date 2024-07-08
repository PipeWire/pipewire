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

#define PW_TELEPHONY_SERVICE "org.freedesktop.PipeWire.Telephony"

#define PW_TELEPHONY_OBJECT_PATH "/org/freedesktop/PipeWire/Telephony"

#define PW_TELEPHONY_AG_IFACE "org.freedesktop.PipeWire.Telephony.AudioGateway1"
#define PW_TELEPHONY_CALL_IFACE "org.freedesktop.PipeWire.Telephony.Call1"

#define PW_TELEPHONY_MANAGER_INTROSPECT_XML \
	DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE				\
	"<node>"		 						\
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
	" </interface>"								\
	" <interface name='" DBUS_INTERFACE_INTROSPECTABLE "'>"			\
	"  <method name='Introspect'>"						\
	"   <arg name='xml' type='s' direction='out'/>"				\
	"  </method>"								\
	" </interface>"			 					\
	"</node>"

#define PW_TELEPHONY_AG_INTROSPECT_XML \
	DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE				\
	"<node>"		 						\
	" <interface name='" PW_TELEPHONY_AG_IFACE "'>"				\
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
	"   <arg name='calls' direction='out' type='a{o}'/>"			\
	"  </method>"								\
	"  <method name='SendTones'>"						\
	"   <arg name='tones' direction='in' type='s'/>"			\
	"  </method>"								\
	" </interface>"								\
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
	" </interface>"								\
	" <interface name='" DBUS_INTERFACE_INTROSPECTABLE "'>"			\
	"  <method name='Introspect'>"						\
	"   <arg name='xml' type='s' direction='out'/>"				\
	"  </method>"								\
	" </interface>"			 					\
	"</node>"

#define PW_TELEPHONY_CALL_INTROSPECT_XML \
	DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE				\
	"<node>"								\
	" <interface name='" PW_TELEPHONY_CALL_IFACE "'>"			\
	"  <method name='Answer'>"						\
	"  </method>"								\
	"  <method name='Hangup'>"						\
	"  </method>"								\
	"  <property name='LineIdentification' type='s' access='read'/>"	\
	"  <property name='IncomingLine' type='s' access='read'/>"		\
	"  <property name='Name' type='s' access='read'/>"			\
	"  <property name='Multiparty' type='b' access='read'/>"		\
	"  <property name='State' type='s' access='read'/>"			\
	" </interface>"								\
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
	" </interface>"								\
	" <interface name='" DBUS_INTERFACE_INTROSPECTABLE "'>"			\
	"  <method name='Introspect'>"						\
	"   <arg name='xml' type='s' direction='out'/>"				\
	"  </method>"								\
	" </interface>"								\
	"</node>"

SPA_LOG_TOPIC_DEFINE_STATIC(log_topic, "spa.bluez5.telephony");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

struct callimpl;

struct impl {
	struct spa_bt_telephony this;

	struct spa_log *log;
	struct spa_dbus *dbus;

	/* session bus */
	struct spa_dbus_connection *dbus_connection;
	DBusConnection *conn;

	struct spa_list ag_list;
};

struct agimpl {
	struct spa_bt_telephony_ag this;
	struct spa_list link;
	int id;
	char *path;
	struct spa_list call_list;
	struct spa_hook_list listener_list;

	bool dial_in_progress;
	struct callimpl *dial_return;
};

struct callimpl {
	struct spa_bt_telephony_call this;
	struct spa_list link;
	int id;
	char *path;
	struct spa_hook_list listener_list;
};

#define ag_emit(ag,m,v,...) 		spa_hook_list_call(&ag->listener_list, struct spa_bt_telephony_ag_events, m, v, ##__VA_ARGS__)
#define ag_emit_dial(s,n)		ag_emit(s,dial,0,n)
#define ag_emit_swap_calls(s)		ag_emit(s,swap_calls,0)
#define ag_emit_release_and_answer(s)	ag_emit(s,release_and_answer,0)
#define ag_emit_release_and_swap(s)	ag_emit(s,release_and_swap,0)
#define ag_emit_hold_and_answer(s)	ag_emit(s,hold_and_answer,0)
#define ag_emit_hangup_all(s)		ag_emit(s,hangup_all,0)
#define ag_emit_create_multiparty(s)	ag_emit(s,create_multiparty,0)
#define ag_emit_send_tones(s,t)		ag_emit(s,send_tones,0,t)

#define call_emit(c,m,v,...) 	spa_hook_list_call(&c->listener_list, struct spa_bt_telephony_call_events, m, v, ##__VA_ARGS__)
#define call_emit_answer(s)	call_emit(s,answer,0)
#define call_emit_hangup(s)	call_emit(s,hangup,0)

static void dbus_iter_append_call_properties(DBusMessageIter *i, struct spa_bt_telephony_call *call);

#define find_free_object_id(list, obj_type)	\
({						\
	int id = 0;				\
	obj_type *object;			\
	spa_list_for_each(object, list, link) {	\
		if (object->id <= id)		\
			id = object->id + 1;	\
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

static DBusMessage *manager_get_managed_objects(struct impl *impl, DBusMessage *m)
{
	struct agimpl *agimpl;
	spa_autoptr(DBusMessage) r = NULL;
	DBusMessageIter iter, array1, entry1, array2, entry2, props_dict;
	const char *interface = PW_TELEPHONY_AG_IFACE;

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return NULL;

	dbus_message_iter_init_append(r, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &array1);

	spa_list_for_each (agimpl, &impl->ag_list, link) {
		if (agimpl->path) {
			dbus_message_iter_open_container(&array1, DBUS_TYPE_DICT_ENTRY, NULL, &entry1);
			dbus_message_iter_append_basic(&entry1, DBUS_TYPE_OBJECT_PATH, &agimpl->path);
			dbus_message_iter_open_container(&entry1, DBUS_TYPE_ARRAY, "{sa{sv}}", &array2);
			dbus_message_iter_open_container(&array2, DBUS_TYPE_DICT_ENTRY, NULL, &entry2);
			dbus_message_iter_append_basic(&entry2, DBUS_TYPE_STRING, &interface);
			dbus_message_iter_open_container(&entry2, DBUS_TYPE_ARRAY, "{sv}", &props_dict);
			dbus_message_iter_close_container(&entry2, &props_dict);
			dbus_message_iter_close_container(&array2, &entry2);
			dbus_message_iter_close_container(&entry1, &array2);
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
		r = manager_get_managed_objects(impl, m);
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

	impl->dbus_connection = spa_dbus_get_connection(impl->dbus, SPA_DBUS_TYPE_SESSION);
	if (impl->dbus_connection == NULL) {
		spa_log_warn(impl->log, "no session dbus connection");
		goto fail;
	}
	impl->conn = spa_dbus_connection_get(impl->dbus_connection);
	if (impl->conn == NULL) {
		spa_log_warn(impl->log, "failed to get session dbus connection");
		goto fail;
	}

	/* XXX: We should handle spa_dbus reconnecting, but we don't, so ref
	 * XXX: the handle so that we can keep it if spa_dbus unrefs it.
	 */
	dbus_connection_ref(impl->conn);

	res = dbus_bus_request_name(impl->conn, PW_TELEPHONY_SERVICE, DBUS_NAME_FLAG_DO_NOT_QUEUE, &err);
	if (res < 0) {
		spa_log_warn(impl->log, "D-Bus RequestName() error: %s", err.message);
		goto fail;
	}
	if (res == DBUS_REQUEST_NAME_REPLY_EXISTS) {
		spa_log_warn(impl->log, "Bluetooth Telephony service is already registered by another connection");
		goto fail;
	}

	if (!dbus_connection_register_object_path(impl->conn, PW_TELEPHONY_OBJECT_PATH,
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

static DBusMessage *ag_get_managed_objects(struct agimpl *agimpl, DBusMessage *m)
{
	struct callimpl *callimpl;
	spa_autoptr(DBusMessage) r = NULL;
	DBusMessageIter iter, array1, entry1, array2, entry2;
	const char *interface = PW_TELEPHONY_CALL_IFACE;

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return NULL;

	dbus_message_iter_init_append(r, &iter);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &array1);

	spa_list_for_each (callimpl, &agimpl->call_list, link) {
		dbus_message_iter_open_container(&array1, DBUS_TYPE_DICT_ENTRY, NULL, &entry1);
		dbus_message_iter_append_basic(&entry1, DBUS_TYPE_OBJECT_PATH, &callimpl->path);
		dbus_message_iter_open_container(&entry1, DBUS_TYPE_ARRAY, "{sa{sv}}", &array2);
		dbus_message_iter_open_container(&array2, DBUS_TYPE_DICT_ENTRY, NULL, &entry2);
		dbus_message_iter_append_basic(&entry2, DBUS_TYPE_STRING, &interface);
		dbus_iter_append_call_properties(&entry2, &callimpl->this);
		dbus_message_iter_close_container(&array2, &entry2);
		dbus_message_iter_close_container(&entry1, &array2);
		dbus_message_iter_close_container(&array1, &entry1);
	}
	dbus_message_iter_close_container(&iter, &array1);

	return spa_steal_ptr(r);
}

static DBusMessage *ag_dial(struct agimpl *agimpl, DBusMessage *m)
{
	const char *number = NULL, *c;
	int count = 0;
	spa_autoptr(DBusMessage) r = NULL;

	if (!dbus_message_get_args(m, NULL,
				DBUS_TYPE_STRING, &number,
				DBUS_TYPE_INVALID))
		return NULL;

	/* validate number */
	if (!number)
		goto invalid_argument;
	for (c = number; *c != '\0'; c++) {
		if (!(*c >= '0' && *c <= '9') && !(*c >= 'A' && *c <= 'D') &&
			*c != '#' && *c != '*' && *c != '+' && *c != ',' )
			goto invalid_argument;
		count++;
	}
	if (count < 1 || count > 80)
		goto invalid_argument;

	agimpl->dial_in_progress = true;
	ag_emit_dial(agimpl, number);
	agimpl->dial_in_progress = false;

	if (!agimpl->dial_return || !agimpl->dial_return->path)
		goto failed;

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return NULL;
	if (!dbus_message_append_args(r, DBUS_TYPE_OBJECT_PATH,
			&agimpl->dial_return->path, DBUS_TYPE_INVALID))
		return NULL;

	agimpl->dial_return = NULL;

	return spa_steal_ptr(r);

invalid_argument:
	return dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS,
		"Dial number is not a valid phone number");

failed:
	return dbus_message_new_error(m, DBUS_ERROR_FAILED,
		"Dial did not create a new Call object");
}

static DBusMessage *ag_swap_calls(struct agimpl *agimpl, DBusMessage *m)
{
	ag_emit_swap_calls(agimpl);
	return dbus_message_new_method_return(m);
}

static DBusMessage *ag_release_and_answer(struct agimpl *agimpl, DBusMessage *m)
{
	ag_emit_release_and_answer(agimpl);
	return dbus_message_new_method_return(m);
}

static DBusMessage *ag_release_and_swap(struct agimpl *agimpl, DBusMessage *m)
{
	ag_emit_release_and_swap(agimpl);
	return dbus_message_new_method_return(m);
}

static DBusMessage *ag_hold_and_answer(struct agimpl *agimpl, DBusMessage *m)
{
	ag_emit_hold_and_answer(agimpl);
	return dbus_message_new_method_return(m);
}

static DBusMessage *ag_hangup_all(struct agimpl *agimpl, DBusMessage *m)
{
	ag_emit_hangup_all(agimpl);
	return dbus_message_new_method_return(m);
}

static DBusMessage *ag_create_multiparty(struct agimpl *agimpl, DBusMessage *m)
{
	struct callimpl *callimpl;
	spa_autoptr(DBusMessage) r = NULL;
	DBusMessageIter i, oi;

	ag_emit_create_multiparty(agimpl);

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return NULL;

	dbus_message_iter_init_append(r, &i);
	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "{o}", &oi);

	spa_list_for_each (callimpl, &agimpl->call_list, link) {
		if (callimpl->this.multiparty)
			dbus_message_iter_append_basic(&oi, DBUS_TYPE_OBJECT_PATH,
				&callimpl->path);
	}
	dbus_message_iter_close_container(&i, &oi);
	return spa_steal_ptr(r);
}

static DBusMessage *ag_send_tones(struct agimpl *agimpl, DBusMessage *m)
{
	const char *tones = NULL, *c;

	if (!dbus_message_get_args(m, NULL,
				DBUS_TYPE_STRING, &tones,
				DBUS_TYPE_INVALID))
		return NULL;

	if (!tones)
		goto invalid_argument;
	for (c = tones; *c != '\0'; c++) {
		if (!(*c >= '0' && *c <= '9') && !(*c >= 'A' && *c <= 'D') &&
				*c != '#' && *c != '*')
			goto invalid_argument;
	}

	ag_emit_send_tones(agimpl, tones);
	return dbus_message_new_method_return(m);

invalid_argument:
	return dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS,
		"SendTones argument is not a valid DTMF tones string");
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
		r = ag_get_managed_objects(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "Dial")) {
		r = ag_dial(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "SwapCalls")) {
		r = ag_swap_calls(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "ReleaseAndAnswer")) {
		r = ag_release_and_answer(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "ReleaseAndSwap")) {
		r = ag_release_and_swap(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "HoldAndAnswer")) {
		r = ag_hold_and_answer(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "HangupAll")) {
		r = ag_hangup_all(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "CreateMultiparty")) {
		r = ag_create_multiparty(agimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_AG_IFACE, "SendTones")) {
		r = ag_send_tones(agimpl, m);
	} else {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (r == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(impl->conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	return DBUS_HANDLER_RESULT_HANDLED;
}

struct spa_bt_telephony_ag *
telephony_ag_new(struct spa_bt_telephony *telephony)
{
	struct impl *impl = SPA_CONTAINER_OF(telephony, struct impl, this);
	struct agimpl *agimpl;

	agimpl = calloc(1, sizeof(*agimpl));
	if (agimpl == NULL)
		return NULL;

	agimpl->this.telephony = telephony;
	agimpl->id = find_free_object_id(&impl->ag_list, struct agimpl);
	agimpl->call_list = SPA_LIST_INIT(&agimpl->call_list);
	spa_hook_list_init(&agimpl->listener_list);

	spa_list_append(&impl->ag_list, &agimpl->link);

	return &agimpl->this;
}

void telephony_ag_destroy(struct spa_bt_telephony_ag *ag)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	struct callimpl *callimpl;

	spa_list_consume (callimpl, &agimpl->call_list, link) {
		telephony_call_destroy(&callimpl->this);
	}

	telephony_ag_unregister(ag);
	spa_list_remove(&agimpl->link);
	spa_hook_list_clean(&agimpl->listener_list);

	free(agimpl);
}

int telephony_ag_register(struct spa_bt_telephony_ag *ag)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	struct impl *impl = SPA_CONTAINER_OF(agimpl->this.telephony, struct impl, this);

	spa_autoptr(DBusMessage) msg = NULL;
	DBusMessageIter iter, entry, dict, props_dict;
	char *path;
	const char *interface = PW_TELEPHONY_AG_IFACE;

	const DBusObjectPathVTable vtable = {
		.message_function = ag_handler,
	};

	path = spa_aprintf (PW_TELEPHONY_OBJECT_PATH "/ag%d", agimpl->id);

	/* register object */
	if (!dbus_connection_register_object_path(impl->conn, path, &vtable, agimpl)) {
		spa_log_error(impl->log, "failed to register %s", path);
		return -EIO;
	}
	agimpl->path = strdup(path);

	/* notify on ObjectManager of the parent object */
	msg = dbus_message_new_signal(PW_TELEPHONY_OBJECT_PATH,
				      DBUS_INTERFACE_OBJECT_MANAGER,
				      "InterfacesAdded");

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sa{sv}}", &dict);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &interface);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &props_dict);
	dbus_message_iter_close_container(&entry, &props_dict);
	dbus_message_iter_close_container(&dict, &entry);
	dbus_message_iter_close_container(&iter, &dict);

	if (!dbus_connection_send(impl->conn, msg, NULL)) {
		spa_log_error(impl->log, "failed to send InterfacesAdded for %s", path);
		telephony_ag_unregister(ag);
		return -EIO;
	}

	spa_log_debug(impl->log, "registered AudioGateway: %s", path);

	return 0;
}

void telephony_ag_unregister(struct spa_bt_telephony_ag *ag)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	struct impl *impl = SPA_CONTAINER_OF(agimpl->this.telephony, struct impl, this);

	spa_autoptr(DBusMessage) msg = NULL;
	DBusMessageIter iter, entry;
	const char *interface = PW_TELEPHONY_AG_IFACE;

	if (!agimpl->path)
		return;

	spa_log_debug(impl->log, "removing AudioGateway: %s", agimpl->path);

	msg = dbus_message_new_signal(PW_TELEPHONY_OBJECT_PATH,
				      DBUS_INTERFACE_OBJECT_MANAGER,
				      "InterfacesRemoved");

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &agimpl->path);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
					 DBUS_TYPE_STRING_AS_STRING, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &interface);
	dbus_message_iter_close_container(&iter, &entry);

	if (!dbus_connection_send(impl->conn, msg, NULL)) {
		spa_log_warn(impl->log, "sending InterfacesRemoved failed");
	}
	if (!dbus_connection_unregister_object_path(impl->conn, agimpl->path)) {
		spa_log_warn(impl->log, "failed to unregister %s", agimpl->path);
	}

	free(agimpl->path);
	agimpl->path = NULL;
}

void telephony_ag_add_listener(struct spa_bt_telephony_ag *ag,
	struct spa_hook *listener,
	const struct spa_bt_telephony_ag_events *events,
	void *data)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	spa_hook_list_append(&agimpl->listener_list, listener, events, data);
}

struct spa_bt_telephony_call *
telephony_call_new(struct spa_bt_telephony_ag *ag)
{
	struct agimpl *agimpl = SPA_CONTAINER_OF(ag, struct agimpl, this);
	struct callimpl *callimpl;

	callimpl = calloc(1, sizeof(*callimpl));
	if (callimpl == NULL)
		return NULL;

	callimpl->this.ag = ag;
	callimpl->id = find_free_object_id(&agimpl->call_list, struct callimpl);
	spa_hook_list_init(&callimpl->listener_list);

	spa_list_append(&agimpl->call_list, &callimpl->link);

	/* mark this object as the return value of the Dial method */
	if (agimpl->dial_in_progress)
		agimpl->dial_return = callimpl;

	return &callimpl->this;
}

void telephony_call_destroy(struct spa_bt_telephony_call *call)
{
	struct callimpl *callimpl = SPA_CONTAINER_OF(call, struct callimpl, this);

	telephony_call_unregister(call);
	spa_list_remove(&callimpl->link);
	spa_hook_list_clean(&callimpl->listener_list);

	free(call->line_identification);
	free(call->incoming_line);
	free(call->name);

	free(callimpl);
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

static void
dbus_iter_append_call_properties(DBusMessageIter *i, struct spa_bt_telephony_call *call)
{
	DBusMessageIter dict, entry, variant;

	dbus_message_iter_open_container(i, DBUS_TYPE_ARRAY, "{sv}", &dict);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL,
					 &entry);
	const char *line_identification = "LineIdentification";
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &line_identification);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
					 DBUS_TYPE_STRING_AS_STRING, &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &call->line_identification);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(&dict, &entry);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	const char *incoming_line = "IncomingLine";
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &incoming_line);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
					 DBUS_TYPE_STRING_AS_STRING,
					 &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &call->incoming_line);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(&dict, &entry);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	const char *name = "Name";
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &name);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
					 DBUS_TYPE_STRING_AS_STRING,
					 &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &call->name);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(&dict, &entry);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	const char *multiparty = "Multiparty";
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &multiparty);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
					 DBUS_TYPE_BOOLEAN_AS_STRING,
					 &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &call->multiparty);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(&dict, &entry);

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

static DBusMessage *call_properties_get_all(struct callimpl *callimpl, DBusMessage *m)
{
	const char *iface, *name;
	DBusMessage *r;
	DBusMessageIter i;

	if (!dbus_message_get_args(m, NULL,
				DBUS_TYPE_STRING, &iface,
				DBUS_TYPE_STRING, &name,
				DBUS_TYPE_INVALID))
		return NULL;

	if (!spa_streq(iface, PW_TELEPHONY_CALL_IFACE))
		return dbus_message_new_error(m, DBUS_ERROR_UNKNOWN_INTERFACE,
				"No such interface");

	r = dbus_message_new_method_return(m);
	if (r == NULL)
		return NULL;

	dbus_message_iter_init_append(r, &i);
	dbus_iter_append_call_properties(&i, &callimpl->this);
	return r;
}

static DBusMessage *call_properties_set(struct callimpl *callimpl, DBusMessage *m)
{
	return dbus_message_new_error(m, DBUS_ERROR_PROPERTY_READ_ONLY,
			"Property not writable");
}

static DBusMessage *call_answer(struct callimpl *callimpl, DBusMessage *m)
{
	call_emit_answer(callimpl);
	return dbus_message_new_method_return(m);
}

static DBusMessage *call_hangup(struct callimpl *callimpl, DBusMessage *m)
{
	call_emit_hangup(callimpl);
	return dbus_message_new_method_return(m);
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
		r = call_properties_get_all(callimpl, m);
	} else if (dbus_message_is_method_call(m, DBUS_INTERFACE_PROPERTIES, "Set")) {
		r = call_properties_set(callimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_CALL_IFACE, "Answer")) {
		r = call_answer(callimpl, m);
	} else if (dbus_message_is_method_call(m, PW_TELEPHONY_CALL_IFACE, "Hangup")) {
		r = call_hangup(callimpl, m);
	} else {
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (r == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(impl->conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	return DBUS_HANDLER_RESULT_HANDLED;
}

int telephony_call_register(struct spa_bt_telephony_call *call)
{
	struct callimpl *callimpl = SPA_CONTAINER_OF(call, struct callimpl, this);
	struct agimpl *agimpl = SPA_CONTAINER_OF(callimpl->this.ag, struct agimpl, this);
	struct impl *impl = SPA_CONTAINER_OF(agimpl->this.telephony, struct impl, this);

	spa_autoptr(DBusMessage) msg = NULL;
	DBusMessageIter iter, entry, dict;
	char *path;
	const char *interface = PW_TELEPHONY_CALL_IFACE;

	const DBusObjectPathVTable vtable = {
		.message_function = call_handler,
	};

	path = spa_aprintf ("%s/call%d", agimpl->path, callimpl->id);

	/* register object */
	if (!dbus_connection_register_object_path(impl->conn, path, &vtable, callimpl)) {
		spa_log_error(impl->log, "failed to register %s", path);
		return -EIO;
	}
	callimpl->path = strdup(path);

	/* notify on ObjectManager of the parent object */
	msg = dbus_message_new_signal(agimpl->path,
				      DBUS_INTERFACE_OBJECT_MANAGER,
				      "InterfacesAdded");

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &path);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sa{sv}}", &dict);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &interface);
	dbus_iter_append_call_properties(&entry, call);
	dbus_message_iter_close_container(&dict, &entry);
	dbus_message_iter_close_container(&iter, &dict);

	if (!dbus_connection_send(impl->conn, msg, NULL)) {
		spa_log_error(impl->log, "failed to send InterfacesAdded for %s", path);
		telephony_call_unregister(call);
		return -EIO;
	}

	spa_log_debug(impl->log, "registered Call: %s", path);

	return 0;
}

void telephony_call_unregister(struct spa_bt_telephony_call *call)
{
	struct callimpl *callimpl = SPA_CONTAINER_OF(call, struct callimpl, this);
	struct agimpl *agimpl = SPA_CONTAINER_OF(callimpl->this.ag, struct agimpl, this);
	struct impl *impl = SPA_CONTAINER_OF(agimpl->this.telephony, struct impl, this);

	spa_autoptr(DBusMessage) msg = NULL;
	DBusMessageIter iter, entry;
	const char *interface = PW_TELEPHONY_CALL_IFACE;

	if (!callimpl->path)
		return;

	spa_log_debug(impl->log, "removing Call: %s", callimpl->path);

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

	spa_autoptr(DBusMessage) msg = NULL;
	const char *interface = PW_TELEPHONY_CALL_IFACE;
	DBusMessageIter i, a;

	msg = dbus_message_new_signal(callimpl->path,
				      DBUS_INTERFACE_PROPERTIES,
				      "PropertiesChanged");

	dbus_message_iter_init_append(msg, &i);
	dbus_message_iter_append_basic(&i, DBUS_TYPE_STRING, &interface);

	dbus_iter_append_call_properties(&i, call);

	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY,
			DBUS_TYPE_STRING_AS_STRING, &a);
	dbus_message_iter_close_container(&i, &a);

	if (!dbus_connection_send(impl->conn, msg, NULL)){
		spa_log_warn(impl->log, "sending PropertiesChanged failed");
	}
}

void telephony_call_add_listener(struct spa_bt_telephony_call *call,
	struct spa_hook *listener,
	const struct spa_bt_telephony_call_events *events,
	void *data)
{
	struct callimpl *callimpl = SPA_CONTAINER_OF(call, struct callimpl, this);
	spa_hook_list_append(&callimpl->listener_list, listener, events, data);
}
