/* Spa Bluez5 midi
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

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <dbus/dbus.h>

#include <spa/utils/defs.h>
#include <spa/utils/string.h>
#include <spa/utils/result.h>

#include "dbus-manager.h"
#include "dbus-monitor.h"
#include "midi.h"

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT (&impl->log_topic)

#define MIDI_SERVER_PATH	"/midiserver"
#define MIDI_SERVICE_PATH	"/midiserver/service"
#define MIDI_CHR_PATH		"/midiserver/service/chr"
#define MIDI_DSC_PATH		"/midiserver/service/chr/dsc"

#define BLE_DEFAULT_MTU		23

struct impl
{
	struct spa_bt_midi_server this;

	struct spa_log_topic log_topic;
	struct spa_log *log;

	const struct spa_bt_midi_server_cb *cb;

	struct spa_dbus_local_object *service;
	struct spa_dbus_local_object *chr;
	struct spa_dbus_local_object *dsc;

	struct spa_dbus_monitor *dbus_monitor;
	struct spa_dbus_object_manager *objects;
	DBusConnection *conn;
	void *user_data;
};

struct adapter
{
	struct spa_dbus_object object;
	struct spa_dbus_async_call register_call;
	unsigned int registered:1;
};

struct chr {
	struct spa_dbus_local_object object;
	unsigned int write_acquired:1;
	unsigned int notify_acquired:1;
};

struct dsc {
	struct spa_dbus_local_object object;
};


/*
 * Characteristic user descriptor: not in BLE MIDI standard, but we
 * put a device name here in case we have multiple MIDI endpoints.
 */

static DBusMessage *parse_options(struct impl *impl, DBusMessage *m, const char *out_key, uint16_t *out_value);

static DBusMessage *dsc_read_value(struct spa_dbus_local_object *object, DBusMessage *m)
{
	struct dsc *dsc = SPA_CONTAINER_OF(object, struct dsc, object);
	struct impl *impl = dsc->object.user_data;
	DBusMessage *r;
	DBusMessageIter i, a;
	const char *description = NULL;
	const uint8_t *ptr;
	uint16_t offset = 0;
	int len;

	r = parse_options(impl, m, "offset", &offset);
	if (r)
		return r;

	if (impl->cb->get_description)
		description = impl->cb->get_description(impl->user_data);
	if (!description)
		description = "";

	len = strlen(description);
	if (offset > len)
		return dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS,
				"Invalid arguments");

	r = dbus_message_new_method_return(m);
	if (r == NULL)
		return NULL;

	ptr = SPA_PTROFF(description, offset, const uint8_t);
	len -= offset;

	dbus_message_iter_init_append(r, &i);
	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "y", &a);
	dbus_message_iter_append_fixed_array(&a, DBUS_TYPE_BYTE, &ptr, len);
	dbus_message_iter_close_container(&i, &a);
	return r;
}

static int dsc_prop_uuid_get(struct spa_dbus_local_object *object, DBusMessageIter *value)
{
	const char *uuid = BT_GATT_CHARACTERISTIC_USER_DESCRIPTION_UUID;
	dbus_message_iter_append_basic(value, DBUS_TYPE_STRING, &uuid);
	return 0;
}

static int dsc_prop_characteristic_get(struct spa_dbus_local_object *object, DBusMessageIter *value)
{
	struct dsc *dsc = SPA_CONTAINER_OF(object, struct dsc, object);
	struct impl *impl = dsc->object.user_data;
	dbus_message_iter_append_basic(value, DBUS_TYPE_OBJECT_PATH, &impl->chr->path);
	return 0;
}

static int dsc_prop_flags_get(struct spa_dbus_local_object *object, DBusMessageIter *value)
{
	DBusMessageIter a;
	static const char * const flags[] = { "encrypt-read" };

	dbus_message_iter_open_container(value, DBUS_TYPE_ARRAY, "s", &a);
	SPA_FOR_EACH_ELEMENT_VAR(flags, p)
		dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, p);
	dbus_message_iter_close_container(value, &a);

	return 0;
}

static const struct spa_dbus_local_interface midi_dsc_interfaces[] = {
	{
		.name = BLUEZ_GATT_DSC_INTERFACE,
		.methods = (struct spa_dbus_method[]) {
			{
				.name = "ReadValue",
				.call = dsc_read_value,
			},
			{NULL}
		},
		.properties = (struct spa_dbus_property[]) {
			{
				.name = "UUID",
				.signature = "s",
				.get = dsc_prop_uuid_get,
			},
			{
				.name = "Characteristic",
				.signature = "o",
				.get = dsc_prop_characteristic_get,
			},
			{
				.name = "Flags",
				.signature = "as",
				.get = dsc_prop_flags_get,
			},
			{NULL}
		},
	},
	{NULL}
};


/*
 * MIDI characteristic
 */

#define CHR_IFACE			0
#define CHR_PROP_UUID			0
#define CHR_PROP_SERVICE		1
#define CHR_PROP_WRITE_ACQUIRED		2
#define CHR_PROP_NOTIFY_ACQUIRED	3
#define CHR_PROP_FLAGS			4

static DBusMessage *chr_read_value(struct spa_dbus_local_object *object, DBusMessage *m)
{
	DBusMessage *r;
	DBusMessageIter i, a;

	r = dbus_message_new_method_return(m);
	if (r == NULL)
		return NULL;

	/* BLE MIDI-1.0: reading value returns an empty reply */
	dbus_message_iter_init_append(r, &i);
	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "y", &a);
	dbus_message_iter_close_container(&i, &a);
	return r;
}

static void chr_change_acquired(struct impl *impl, struct chr *chr, bool write, bool enabled)
{
	const struct spa_dbus_local_interface *iface = &chr->object.interfaces[CHR_IFACE];
	struct spa_dbus_property changed[2] = {0};

	if (write) {
		if (chr->write_acquired != enabled)
			changed[0] = iface->properties[CHR_PROP_WRITE_ACQUIRED];
		chr->write_acquired = enabled;
	} else {
		if (chr->notify_acquired != enabled)
			changed[0] = iface->properties[CHR_PROP_NOTIFY_ACQUIRED];
		chr->notify_acquired = enabled;
	}

	spa_dbus_object_manager_properties_changed(impl->objects, impl->chr, iface, changed);
}

static DBusMessage *parse_options(struct impl *impl, DBusMessage *m, const char *out_key, uint16_t *out_value)
{
	DBusMessageIter args, options;

	if (!dbus_message_iter_init(m, &args) || !spa_streq(dbus_message_get_signature(m), "a{sv}"))
		return dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS,
				"Invalid arguments");

	dbus_message_iter_recurse(&args, &options);
	if (dbus_message_iter_get_arg_type(&options) != DBUS_TYPE_DICT_ENTRY)
		return dbus_message_new_error(m, DBUS_ERROR_INVALID_ARGS,
				"Invalid arguments");

	while (dbus_message_iter_get_arg_type(&options) == DBUS_TYPE_DICT_ENTRY) {
		const char *key;
		int type;
		DBusMessageIter value, entry;

		dbus_message_iter_recurse(&options, &entry);
		dbus_message_iter_get_basic(&entry, &key);
		dbus_message_iter_next(&entry);
		dbus_message_iter_recurse(&entry, &value);
		type = dbus_message_iter_get_arg_type(&value);

		if (spa_streq(key, out_key) && type == DBUS_TYPE_UINT16)
			dbus_message_iter_get_basic(&value, out_value);

		dbus_message_iter_next(&options);
	}

	return NULL;
}

static int create_socketpair(int fds[2])
{
	if (socketpair(AF_LOCAL, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) < 0)
		return -errno;
	return 0;
}

static DBusMessage *chr_acquire(struct spa_dbus_local_object *object, DBusMessage *m, bool write)
{
	struct chr *chr = SPA_CONTAINER_OF(object, struct chr, object);
	struct impl *impl = chr->object.user_data;
	const char *err_msg = "Failed";
	uint16_t mtu = BLE_DEFAULT_MTU;
	int fds[2] = {-1, -1};
	int res;
	DBusMessage *r;
	DBusMessageIter i;

	if ((write && (impl->cb->acquire_write == NULL)) ||
			(!write && (impl->cb->acquire_notify == NULL))) {
		err_msg = "Not supported";
		goto fail;
	}
	if ((write && chr->write_acquired) ||
			(!write && chr->notify_acquired)) {
		err_msg = "Already acquired";
		goto fail;
	}

	r = parse_options(impl, m, "mtu", &mtu);
	if (r)
		return r;

	if (create_socketpair(fds) < 0) {
		err_msg = "Socketpair creation failed";
		goto fail;
	}

	if (write)
		res = impl->cb->acquire_write(impl->user_data, fds[0], mtu);
	else
		res = impl->cb->acquire_notify(impl->user_data, fds[0], mtu);
	if (res < 0) {
		err_msg = "Acquiring failed";
		goto fail;
	}
	fds[0] = -1;

	r = dbus_message_new_method_return(m);
	if (r == NULL)
		goto fail;

	dbus_message_iter_init_append(r, &i);
	dbus_message_iter_append_basic(&i, DBUS_TYPE_UNIX_FD, &fds[1]);
	dbus_message_iter_append_basic(&i, DBUS_TYPE_UINT16, &mtu);

	close(fds[1]);
	fds[1] = -1;

	chr_change_acquired(impl, chr, write, true);

	return r;

fail:
	if (fds[0] >= 0)
		close(fds[0]);
	if (fds[1] >= 0)
		close(fds[1]);
	return dbus_message_new_error(m, DBUS_ERROR_FAILED, err_msg);

}

static DBusMessage *chr_acquire_write(struct spa_dbus_local_object *object, DBusMessage *m)
{
	return chr_acquire(object, m, true);
}

static DBusMessage *chr_acquire_notify(struct spa_dbus_local_object *object, DBusMessage *m)
{
	return chr_acquire(object, m, false);
}

static int chr_prop_uuid_get(struct spa_dbus_local_object *object, DBusMessageIter *value)
{
	const char *uuid = BT_MIDI_CHR_UUID;
	dbus_message_iter_append_basic(value, DBUS_TYPE_STRING, &uuid);
	return 0;
}

static int chr_prop_service_get(struct spa_dbus_local_object *object, DBusMessageIter *value)
{
	const char *service = MIDI_SERVICE_PATH;
	dbus_message_iter_append_basic(value, DBUS_TYPE_OBJECT_PATH, &service);
	return 0;
}

static int chr_prop_notify_acquired_get(struct spa_dbus_local_object *object, DBusMessageIter *value)
{
	struct chr *chr = SPA_CONTAINER_OF(object, struct chr, object);
	dbus_bool_t v = chr->notify_acquired;
	dbus_message_iter_append_basic(value, DBUS_TYPE_BOOLEAN, &v);
	return 0;
}

static int chr_prop_write_acquired_get(struct spa_dbus_local_object *object, DBusMessageIter *value)
{
	struct chr *chr = SPA_CONTAINER_OF(object, struct chr, object);
	dbus_bool_t v = chr->write_acquired;
	dbus_message_iter_append_basic(value, DBUS_TYPE_BOOLEAN, &v);
	return 0;
}

static int chr_prop_flags_get(struct spa_dbus_local_object *object, DBusMessageIter *value)
{
	DBusMessageIter a;
	static const char * const flags[] = {"encrypt-read", "write-without-response",
		"encrypt-write", "encrypt-notify" };

	dbus_message_iter_open_container(value, DBUS_TYPE_ARRAY, "s", &a);
	SPA_FOR_EACH_ELEMENT_VAR(flags, p)
		dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, p);
	dbus_message_iter_close_container(value, &a);

	return 0;
}

static const struct spa_dbus_local_interface midi_chr_interfaces[] = {
	[CHR_IFACE] = {
		.name = BLUEZ_GATT_CHR_INTERFACE,
		.methods = (struct spa_dbus_method[]) {
			{
				.name = "ReadValue",
				.call = chr_read_value,
			},
			{
				.name = "AcquireWrite",
				.call = chr_acquire_write,
			},
			{
				.name = "AcquireNotify",
				.call = chr_acquire_notify,
			},
			{NULL}
		},
		.properties = (struct spa_dbus_property[]) {
			[CHR_PROP_UUID] = {
				.name = "UUID",
				.signature = "s",
				.get = chr_prop_uuid_get,
			},
			[CHR_PROP_SERVICE] = {
				.name = "Service",
				.signature = "o",
				.get = chr_prop_service_get,
			},
			[CHR_PROP_WRITE_ACQUIRED] = {
				.name = "WriteAcquired",
				.signature = "b",
				.get = chr_prop_write_acquired_get,
			},
			[CHR_PROP_NOTIFY_ACQUIRED] = {
				.name = "NotifyAcquired",
				.signature = "b",
				.get = chr_prop_notify_acquired_get,
			},
			[CHR_PROP_FLAGS] = {
				.name = "Flags",
				.signature = "as",
				.get = chr_prop_flags_get,
			},
			{NULL}
		},
	},
	{NULL}
};

/*
 * MIDI service
 */

static int service_prop_uuid_get(struct spa_dbus_local_object *object, DBusMessageIter *value)
{
	const char *uuid = BT_MIDI_SERVICE_UUID;
	dbus_message_iter_append_basic(value, DBUS_TYPE_STRING, &uuid);
	return 0;
}

static int service_prop_primary_get(struct spa_dbus_local_object *object, DBusMessageIter *value)
{
	dbus_bool_t primary = TRUE;
	dbus_message_iter_append_basic(value, DBUS_TYPE_BOOLEAN, &primary);
	return 0;
}

static const struct spa_dbus_local_interface midi_service_interfaces[] = {
	{
		.name = BLUEZ_GATT_SERVICE_INTERFACE,
		.properties = (struct spa_dbus_property[]) {
			{
				.name = "UUID",
				.signature = "s",
				.get = service_prop_uuid_get,
			},
			{
				.name = "Primary",
				.signature = "b",
				.get = service_prop_primary_get,
			},
			{NULL},
		},
	},
	{NULL}
};

/*
 * Adapters
 */

static void adapter_register_application_reply(struct spa_dbus_async_call *call, DBusMessage *r)
{
	struct adapter *adapter = SPA_CONTAINER_OF(call, struct adapter, register_call);
	struct impl *impl = adapter->object.user_data;

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(impl->log, "%s.RegisterApplication() failed: %s",
				BLUEZ_GATT_MANAGER_INTERFACE,
				dbus_message_get_error_name(r));
		return;
	}

	adapter->registered = true;
}

static int adapter_register_application(struct adapter *adapter)
{
	struct impl *impl = adapter->object.user_data;
	DBusMessageIter i, d;
	DBusMessage *m;

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
			adapter->object.path,
			BLUEZ_GATT_MANAGER_INTERFACE,
			"RegisterApplication");
	if (m == NULL)
		return -ENOMEM;

	dbus_message_iter_init_append(m, &i);
	dbus_message_iter_append_basic(&i, DBUS_TYPE_OBJECT_PATH, &impl->objects->path);
	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "{sv}", &d);
	dbus_message_iter_close_container(&i, &d);

	return spa_dbus_async_call_send(&adapter->register_call, impl->conn, m,
			adapter_register_application_reply);
}

static void adapter_update(struct spa_dbus_object *object)
{
	struct adapter *adapter = SPA_CONTAINER_OF(object, struct adapter, object);

	if (adapter->registered || adapter->register_call.pending)
		return;

	adapter_register_application(adapter);
}

static void adapter_remove(struct spa_dbus_object *object)
{
	struct adapter *adapter = SPA_CONTAINER_OF(object, struct adapter, object);

	spa_dbus_async_call_cancel(&adapter->register_call);
}

static void bluez_remove(struct spa_dbus_object *object)
{
	struct impl *impl = object->user_data;
	struct chr *chr = (struct chr *)impl->chr;

	/*
	 * BlueZ disappeared. It does not appear to close the sockets it has
	 * acquired in this case, so we should force the chr release.
	 */
	if (impl->cb->release)
		impl->cb->release(impl->user_data);
	chr_change_acquired(impl, chr, true, false);
	chr_change_acquired(impl, chr, false, false);
}

static const struct spa_dbus_interface monitor_interfaces[] = {
	{
		.name = BLUEZ_ADAPTER_INTERFACE,
		.update = adapter_update,
		.remove = adapter_remove,
		.object_size = sizeof(struct adapter),
	},
	{
		.name = SPA_DBUS_MONITOR_NAME_OWNER_INTERFACE,
		.remove = bluez_remove,
		.object_size = sizeof(struct spa_dbus_object),
	},
	{NULL}
};

static int register_objects(struct impl *impl)
{
	int res = 0;

	impl->objects = spa_dbus_object_manager_new(impl->conn, MIDI_SERVER_PATH, impl->log);
	if (impl->objects == NULL)
		goto fail;

	impl->service = spa_dbus_object_manager_register(impl->objects,
			MIDI_SERVICE_PATH,
			midi_service_interfaces,
			sizeof(struct spa_dbus_local_object),
			impl);
	if (impl->service == NULL)
		goto fail;

	impl->chr = spa_dbus_object_manager_register(impl->objects,
			MIDI_CHR_PATH,
			midi_chr_interfaces,
			sizeof(struct chr),
			impl);
	if (impl->chr == NULL)
		goto fail;

	impl->dsc = spa_dbus_object_manager_register(impl->objects,
			MIDI_DSC_PATH,
			midi_dsc_interfaces,
			sizeof(struct dsc),
			impl);
	if (impl->dsc == NULL)
		goto fail;

	impl->dbus_monitor = spa_dbus_monitor_new(impl->conn,
			BLUEZ_SERVICE, "/", monitor_interfaces,
			impl->log, impl);
	if (!impl->dbus_monitor)
		goto fail;

	return 0;

fail:
	res = (res < 0) ? res : ((errno > 0) ? -errno : -EIO);

	spa_log_error(impl->log, "Failed to register BLE MIDI services in DBus: %s",
			spa_strerror(res));

	if (impl->objects)
		spa_dbus_object_manager_destroy(impl->objects);
	if (impl->dbus_monitor)
		spa_dbus_monitor_destroy(impl->dbus_monitor);

	return res;
}

struct spa_bt_midi_server *spa_bt_midi_server_new(DBusConnection *conn, const struct spa_bt_midi_server_cb *cb,
		struct spa_log *log, void *user_data)
{
	struct impl *impl;
	int res = 0;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto fail;

	impl->user_data = user_data;
	impl->conn = conn;
	impl->cb = cb;
	impl->log = log;
	impl->log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.midi.server");
	spa_log_topic_init(impl->log, &impl->log_topic);

	if ((res = register_objects(impl)) < 0)
		goto fail;

	impl->this.chr_path = impl->chr->path;

	dbus_connection_ref(impl->conn);

	return &impl->this;

fail:
	res = (res < 0) ? res : ((errno > 0) ? -errno : -EIO);
	free(impl);
	errno = res;
	return NULL;
}

void spa_bt_midi_server_destroy(struct spa_bt_midi_server *server)
{
	struct impl *impl = SPA_CONTAINER_OF(server, struct impl, this);

	spa_dbus_object_manager_destroy(impl->objects);
	dbus_connection_unref(impl->conn);
	free(impl);
}

void spa_bt_midi_server_released(struct spa_bt_midi_server *server, bool write)
{
	struct impl *impl = SPA_CONTAINER_OF(server, struct impl, this);
	struct chr *chr = (struct chr *)impl->chr;

	chr_change_acquired(impl, chr, write, false);
}
