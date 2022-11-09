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

#include <errno.h>
#include <stddef.h>

#include <dbus/dbus.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/dbus.h>
#include <spa/support/plugin.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/utils/hook.h>
#include <spa/utils/type.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/node/node.h>
#include <spa/node/keys.h>

#include "dbus-monitor.h"
#include "dbus-manager.h"

#include "midi.h"
#include "config.h"

#define MIDI_OBJECT_PATH	"/midi"
#define MIDI_PROFILE_PATH	MIDI_OBJECT_PATH "/profile"

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.midi");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

struct impl
{
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;
	struct spa_dbus *dbus;
	struct spa_dbus_connection *dbus_connection;
	DBusConnection *conn;

	struct spa_dbus_monitor *dbus_monitor;

	struct spa_dbus_object_manager *object_manager;
	struct spa_dbus_local_object *profile;

	struct spa_hook_list hooks;

	uint32_t id;

	unsigned int object_manager_registered:1;
	unsigned int profile_registered:1;
};

struct adapter
{
	struct spa_dbus_object object;
	DBusPendingCall *register_call;
	unsigned int registered:1;
};

struct device
{
	struct spa_dbus_object object;
	char *adapter_path;
	char *name;
	char *alias;
	char *address;
	char *icon;
	uint32_t class;
	uint16_t appearance;
	unsigned int connected:1;
	unsigned int services_resolved:1;
};

struct service
{
	struct spa_dbus_object object;
	char *device_path;
	unsigned int valid_uuid:1;
};

struct chr
{
	struct spa_dbus_object object;
	char *service_path;
	char *description;
	uint32_t id;
	DBusPendingCall *read_call;
	DBusPendingCall *dsc_call;
	unsigned int node_emitted:1;
	unsigned int valid_uuid:1;
	unsigned int read_probed:1;
	unsigned int read_done:1;
	unsigned int dsc_probed:1;
	unsigned int dsc_done:1;
};

struct dsc
{
	struct spa_dbus_object object;
	char *chr_path;
	unsigned int valid_uuid:1;
};

static void emit_chr_node(struct impl *impl, struct chr *chr, struct device *device)
{
	struct spa_device_object_info info;
	char nick[512], class[16];
	struct spa_dict_item items[23];
	uint32_t n_items = 0;

	spa_log_debug(impl->log, "emit node for path=%s", chr->object.path);

	info = SPA_DEVICE_OBJECT_INFO_INIT();
	info.type = SPA_TYPE_INTERFACE_Node;
	info.factory_name = SPA_NAME_API_BLUEZ5_MIDI_NODE;
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_FLAGS |
		SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.flags = 0;

	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_API, "bluez5");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_BUS, "bluetooth");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_MEDIA_CLASS, "Midi/Bridge");
	items[n_items++] = SPA_DICT_ITEM_INIT("node.description",
			device->alias ? device->alias : device->name);
	if (chr->description && chr->description[0] != '\0') {
		spa_scnprintf(nick, sizeof(nick), "%s (%s)", device->alias, chr->description);
		items[n_items++] = SPA_DICT_ITEM_INIT("node.nick", nick);
	}
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_ICON, device->icon);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_PATH, chr->object.path);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_ADDRESS, device->address);
	snprintf(class, sizeof(class), "0x%06x", device->class);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_CLASS, class);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_ROLE, "client");

	info.props = &SPA_DICT_INIT(items, n_items);
	spa_device_emit_object_info(&impl->hooks, chr->id, &info);
}

static void remove_chr_node(struct impl *impl, struct chr *chr)
{
	spa_log_debug(impl->log, "remove node for path=%s", chr->object.path);

	spa_device_emit_object_info(&impl->hooks, chr->id, NULL);
}

static void check_chr_node(struct impl *impl, struct chr *chr);

static void read_probe_reply(DBusPendingCall **call_ptr, DBusMessage *r)
{
	struct chr *chr = SPA_CONTAINER_OF(call_ptr, struct chr, read_call);
	struct impl *impl = chr->object.user_data;

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(impl->log, "%s.ReadValue() failed: %s",
				BLUEZ_GATT_CHR_INTERFACE,
				dbus_message_get_error_name(r));
		return;
	}

	spa_log_debug(impl->log, "MIDI GATT read probe done for path=%s", chr->object.path);

	chr->read_done = true;

	check_chr_node(impl, chr);
}

static int read_probe(struct impl *impl, struct chr *chr)
{
	DBusMessageIter i, d;
	DBusMessage *m;

	/*
	 * BLE MIDI-1.0 §5: The Central shall read the MIDI I/O characteristic
	 * of the Peripheral after establishing a connection with the accessory.
	 */

	if (chr->read_probed)
		return 0;
	if (chr->read_call)
		return -EBUSY;

	chr->read_probed = true;

	spa_log_debug(impl->log, "MIDI GATT read probe for path=%s",
			chr->object.path);

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
			chr->object.path,
			BLUEZ_GATT_CHR_INTERFACE,
			"ReadValue");
	if (m == NULL)
		return -ENOMEM;

	dbus_message_iter_init_append(m, &i);
	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "{sv}", &d);
	dbus_message_iter_close_container(&i, &d);

	return spa_dbus_async_call(impl->conn, m, &chr->read_call,
			read_probe_reply);
}

struct dsc *find_dsc(struct impl *impl, struct chr *chr)
{
	struct dsc *dsc;
	const struct spa_list *dscs = spa_dbus_monitor_object_list(
		impl->dbus_monitor, BLUEZ_GATT_DSC_INTERFACE);

	spa_assert(dscs);
	spa_list_for_each(dsc, dscs, object.link) {
		if (dsc->valid_uuid && spa_streq(dsc->chr_path, chr->object.path))
			return dsc;
	}
	return NULL;
}

static void read_dsc_reply(DBusPendingCall **call_ptr, DBusMessage *r)
{
	struct chr *chr = SPA_CONTAINER_OF(call_ptr, struct chr, dsc_call);
	struct impl *impl = chr->object.user_data;
	DBusError err;
	DBusMessageIter args, arr;
	void *value = NULL;
	int n = 0;

	chr->dsc_done = true;

	dbus_error_init(&err);
	if (dbus_set_error_from_message(&err, r)) {
		spa_log_error(impl->log, "%s.ReadValue() failed: %s (%s)",
				BLUEZ_GATT_DSC_INTERFACE,
				err.name ? err.name : "unknown",
				err.message ? err.message : "");
		dbus_error_free(&err);
		return;
	}
	dbus_error_free(&err);
	if (!dbus_message_has_signature(r, "ay")) {
		spa_log_warn(impl->log, "invalid ReadValue() signature");
		return;
	}

	dbus_message_iter_init(r, &args);
	dbus_message_iter_recurse(&args, &arr);
	dbus_message_iter_get_fixed_array(&arr, &value, &n);

	free(chr->description);
	if (n > 0)
		chr->description = strndup(value, n);
	else
		chr->description = NULL;

	spa_log_debug(impl->log, "MIDI GATT user descriptor value: '%s'",
			chr->description);

	check_chr_node(impl, chr);
}

static int read_dsc(struct impl *impl, struct chr *chr)
{
	DBusMessageIter i, d;
	DBusMessage *m;
	struct dsc *dsc;

	if (chr->dsc_probed)
		return 0;
	if (chr->dsc_call)
		return -EBUSY;

	chr->dsc_probed = true;

	dsc = find_dsc(impl, chr);
	if (dsc == NULL) {
		chr->dsc_done = true;
		return -ENOENT;
	}

	spa_log_debug(impl->log, "MIDI GATT user descriptor read, path=%s",
			dsc->object.path);

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
			dsc->object.path,
			BLUEZ_GATT_DSC_INTERFACE,
			"ReadValue");
	if (m == NULL) {
		chr->dsc_done = true;
		return -ENOMEM;
	}

	dbus_message_iter_init_append(m, &i);
	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "{sv}", &d);
	dbus_message_iter_close_container(&i, &d);

	return spa_dbus_async_call(impl->conn, m, &chr->dsc_call,
			read_dsc_reply);
}

static int read_probe_reset(struct impl *impl, struct chr *chr)
{
	spa_dbus_async_call_cancel(&chr->read_call);
	spa_dbus_async_call_cancel(&chr->dsc_call);
	chr->read_probed = false;
	chr->read_done = false;
	chr->dsc_probed = false;
	chr->dsc_done = false;
	return 0;
}

static void lookup_chr_node(struct impl *impl, struct chr *chr, struct service **service, struct device **device)
{
	*service = (struct service *)spa_dbus_monitor_find(impl->dbus_monitor,
			chr->service_path, BLUEZ_GATT_SERVICE_INTERFACE);
	if (*service)
		*device = (struct device *)spa_dbus_monitor_find(impl->dbus_monitor,
				(*service)->device_path, BLUEZ_DEVICE_INTERFACE);
	else
		*device = NULL;
}

static void check_chr_node(struct impl *impl, struct chr *chr)
{
	struct service *service;
	struct device *device;
	bool available;

	lookup_chr_node(impl, chr, &service, &device);

	if (!device || !device->connected) {
		/* Retry read probe on each connection */
		read_probe_reset(impl, chr);
	}

	available = service && device && device->connected &&
		device->services_resolved && service->valid_uuid &&
		chr->valid_uuid;

	if (available && !chr->read_done) {
		read_probe(impl, chr);
		available = false;
	}

	if (available && !chr->dsc_done) {
		read_dsc(impl, chr);
		available = chr->dsc_done;
	}

	if (chr->node_emitted && !available) {
		remove_chr_node(impl, chr);
		chr->node_emitted = false;
	} else if (!chr->node_emitted && available) {
		emit_chr_node(impl, chr, device);
		chr->node_emitted = true;
	}
}

static void check_all_nodes(struct impl *impl)
{
	/*
	 * Check if the nodes we have emitted are in sync with connected devices.
	 */

	struct spa_list *chrs = spa_dbus_monitor_object_list(
		impl->dbus_monitor, BLUEZ_GATT_CHR_INTERFACE);
	struct chr *chr;

	spa_assert(chrs);

	spa_list_for_each(chr, chrs, object.link)
		check_chr_node(impl, chr);
}

static void adapter_register_application_reply(DBusPendingCall **call_ptr, DBusMessage *r)
{
	struct adapter *adapter = SPA_CONTAINER_OF(call_ptr, struct adapter, register_call);
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
	dbus_message_iter_append_basic(&i, DBUS_TYPE_OBJECT_PATH, &impl->object_manager->path);
	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "{sv}", &d);
	dbus_message_iter_close_container(&i, &d);

	return spa_dbus_async_call(impl->conn, m, &adapter->register_call,
			adapter_register_application_reply);
}

/*
 * DBus monitoring
 */

static const char *get_dbus_string(DBusMessageIter *value)
{
	const char *v = NULL;
	int type = dbus_message_iter_get_arg_type(value);
	if (value && (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH))
		dbus_message_iter_get_basic(value, &v);
	return v;
}

static void dup_dbus_string(DBusMessageIter *value, char **v)
{
	const char *str = get_dbus_string(value);
	free(*v);
	*v = str ? strdup(str) : NULL;
}

static bool get_dbus_bool(DBusMessageIter *value)
{
	dbus_bool_t v = FALSE;
	if (value && dbus_message_iter_get_arg_type(value) == DBUS_TYPE_BOOLEAN)
		dbus_message_iter_get_basic(value, &v);
	return v ? true : false;
}

static uint32_t get_dbus_uint32(DBusMessageIter *value)
{
	uint32_t v = 0;
	if (value && dbus_message_iter_get_arg_type(value) == DBUS_TYPE_UINT32)
		dbus_message_iter_get_basic(value, &v);
	return v;
}

static uint16_t get_dbus_uint16(DBusMessageIter *value)
{
	uint16_t v = 0;
	if (value && dbus_message_iter_get_arg_type(value) == DBUS_TYPE_UINT16)
		dbus_message_iter_get_basic(value, &v);
	return v;
}

static void adapter_update(struct spa_dbus_object *object)
{
	struct adapter *adapter = SPA_CONTAINER_OF(object, struct adapter, object);

	if (adapter->registered || adapter->register_call)
		return;

	adapter_register_application(adapter);
}

static void adapter_remove(struct spa_dbus_object *object)
{
	struct adapter *adapter = SPA_CONTAINER_OF(object, struct adapter, object);

	spa_dbus_async_call_cancel(&adapter->register_call);
}

static void device_update(struct spa_dbus_object *object)
{
	struct impl *impl = object->user_data;

	check_all_nodes(impl);
}

static void device_remove(struct spa_dbus_object *object)
{
	struct device *device = SPA_CONTAINER_OF(object, struct device, object);

	free(device->adapter_path);
	free(device->name);
	free(device->alias);
	free(device->address);
	free(device->icon);
}

static void device_property(struct spa_dbus_object *object, const char *key, DBusMessageIter *value)
{
	struct device *device = SPA_CONTAINER_OF(object, struct device, object);

	if (spa_streq(key, "Adapter"))
		dup_dbus_string(value, &device->adapter_path);
	else if (spa_streq(key, "Connected"))
		device->connected = get_dbus_bool(value);
	else if (spa_streq(key, "ServicesResolved"))
		device->services_resolved = get_dbus_bool(value);
	else if (spa_streq(key, "Name"))
		dup_dbus_string(value, &device->name);
	else if (spa_streq(key, "Alias"))
		dup_dbus_string(value, &device->alias);
	else if (spa_streq(key, "Address"))
		dup_dbus_string(value, &device->address);
	else if (spa_streq(key, "Icon"))
		dup_dbus_string(value, &device->icon);
	else if (spa_streq(key, "Class"))
		device->class = get_dbus_uint32(value);
	else if (spa_streq(key, "Appearance"))
		device->appearance = get_dbus_uint16(value);
}

static void service_update(struct spa_dbus_object *object)
{
	struct service *service = SPA_CONTAINER_OF(object, struct service, object);
	struct impl *impl = object->user_data;

	if (!service->valid_uuid) {
		spa_dbus_monitor_ignore_object(impl->dbus_monitor, object);
		return;
	}

	check_all_nodes(impl);
}

static void service_remove(struct spa_dbus_object *object)
{
	struct service *service = SPA_CONTAINER_OF(object, struct service, object);

	free(service->device_path);
}

static void service_property(struct spa_dbus_object *object, const char *key, DBusMessageIter *value)
{
	struct service *service = SPA_CONTAINER_OF(object, struct service, object);

	if (spa_streq(key, "UUID"))
		service->valid_uuid = spa_streq(get_dbus_string(value), BT_MIDI_SERVICE_UUID);
	else if (spa_streq(key, "Device"))
		dup_dbus_string(value, &service->device_path);
}

static void chr_update(struct spa_dbus_object *object)
{
	struct impl *impl = object->user_data;
	struct chr *chr = SPA_CONTAINER_OF(object, struct chr, object);

	if (!chr->valid_uuid) {
		spa_dbus_monitor_ignore_object(impl->dbus_monitor, object);
		return;
	}

	if (chr->id == 0)
		chr->id = ++impl->id;

	check_chr_node(impl, chr);
}

static void chr_remove(struct spa_dbus_object *object)
{
	struct impl *impl = object->user_data;
	struct chr *chr = SPA_CONTAINER_OF(object, struct chr, object);

	read_probe_reset(impl, chr);

	if (chr->node_emitted)
		remove_chr_node(impl, chr);

	free(chr->service_path);
	free(chr->description);
}

static void chr_property(struct spa_dbus_object *object, const char *key, DBusMessageIter *value)
{
	struct chr *chr = SPA_CONTAINER_OF(object, struct chr, object);

	if (spa_streq(key, "UUID"))
		chr->valid_uuid = spa_streq(get_dbus_string(value), BT_MIDI_CHR_UUID);
	else if (spa_streq(key, "Service"))
		dup_dbus_string(value, &chr->service_path);
}

static void dsc_update(struct spa_dbus_object *object)
{
	struct impl *impl = object->user_data;
	struct dsc *dsc = SPA_CONTAINER_OF(object, struct dsc, object);

	if (!dsc->valid_uuid) {
		spa_dbus_monitor_ignore_object(impl->dbus_monitor, object);
		return;
	}
}

static void dsc_property(struct spa_dbus_object *object, const char *key, DBusMessageIter *value)
{
	struct dsc *dsc = SPA_CONTAINER_OF(object, struct dsc, object);

	if (spa_streq(key, "UUID"))
		dsc->valid_uuid = spa_streq(get_dbus_string(value),
				BT_GATT_CHARACTERISTIC_USER_DESCRIPTION_UUID);
	else if (spa_streq(key, "Characteristic"))
		dup_dbus_string(value, &dsc->chr_path);
}

static void dsc_remove(struct spa_dbus_object *object)
{
	struct dsc *dsc = SPA_CONTAINER_OF(object, struct dsc, object);

	free(dsc->chr_path);
}

static const struct spa_dbus_interface monitor_interfaces[] = {
	{
		.name = BLUEZ_ADAPTER_INTERFACE,
		.update = adapter_update,
		.remove = adapter_remove,
		.object_size = sizeof(struct adapter),
	},
	{
		.name = BLUEZ_DEVICE_INTERFACE,
		.update = device_update,
		.remove = device_remove,
		.property = device_property,
		.object_size = sizeof(struct device),
	},
	{
		.name = BLUEZ_GATT_SERVICE_INTERFACE,
		.update = service_update,
		.remove = service_remove,
		.property = service_property,
		.object_size = sizeof(struct service),
	},
	{
		.name = BLUEZ_GATT_CHR_INTERFACE,
		.update = chr_update,
		.remove = chr_remove,
		.property = chr_property,
		.object_size = sizeof(struct chr),
	},
	{
		.name = BLUEZ_GATT_DSC_INTERFACE,
		.update = dsc_update,
		.remove = dsc_remove,
		.property = dsc_property,
		.object_size = sizeof(struct dsc),
	},
	{NULL}
};

/*
 * DBus GATT profile, to enable BlueZ autoconnect
 */

static DBusMessage *profile_release(struct spa_dbus_local_object *object, DBusMessage *m)
{
	/* noop */
	return dbus_message_new_method_return(m);
}

static int profile_uuids_get(struct spa_dbus_local_object *object, DBusMessageIter *value)
{
	DBusMessageIter a;
	const char *uuid = BT_MIDI_SERVICE_UUID;

	dbus_message_iter_open_container(value, DBUS_TYPE_ARRAY, "s", &a);
	dbus_message_iter_append_basic(&a, DBUS_TYPE_STRING, &uuid);
	dbus_message_iter_close_container(value, &a);

	return 0;
}

static const struct spa_dbus_local_interface profile_interfaces[] = {
	{
		.name = BLUEZ_GATT_PROFILE_INTERFACE,
		.methods = (struct spa_dbus_method[]) {
			{
				.name = "Release",
				.call = profile_release,
			},
			{NULL}
		},
		.properties = (struct spa_dbus_property[]) {
			{
				.name = "UUIDs",
				.signature = "as",
				.get = profile_uuids_get,
			},
			{NULL},
		},
	},
	{NULL}
};

/*
 * Monitor impl
 */

static int impl_device_add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;
	struct spa_list *chrs;
	struct chr *chr;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	chrs = spa_dbus_monitor_object_list(
		this->dbus_monitor, BLUEZ_GATT_CHR_INTERFACE);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	spa_list_for_each(chr, chrs, object.link) {
		struct service *service;
		struct device *device;

		if (!chr->node_emitted)
			continue;

		lookup_chr_node(this, chr, &service, &device);
		if (device)
			emit_chr_node(this, chr, device);
	}

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static const struct spa_device_methods impl_device = {
	SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_device_add_listener,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct impl *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Device))
		*interface = &this->device;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this;

	this = (struct impl *) handle;

	if (this->object_manager)
		spa_dbus_object_manager_destroy(this->object_manager);
	if (this->dbus_monitor)
		spa_dbus_monitor_destroy(this->dbus_monitor);
	if (this->conn)
		dbus_connection_unref(this->conn);
	if (this->dbus_connection)
		spa_dbus_connection_destroy(this->dbus_connection);

	spa_zero(*this);

	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
		const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int
impl_init(const struct spa_handle_factory *factory,
		struct spa_handle *handle,
		const struct spa_dict *info,
		const struct spa_support *support,
		uint32_t n_support)
{
	struct impl *this;
	int res = 0;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	this->dbus = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DBus);

	if (this->log == NULL)
		return -EINVAL;

	spa_log_topic_init(this->log, &log_topic);

	if (this->dbus == NULL) {
		spa_log_error(this->log, "a dbus is needed");
		return -EINVAL;
	}

	this->conn = NULL;
	this->dbus_connection = NULL;

	this->dbus_connection = spa_dbus_get_connection(this->dbus, SPA_DBUS_TYPE_SYSTEM);
	if (this->dbus_connection == NULL) {
		spa_log_error(this->log, "no dbus connection");
		res = -EIO;
		goto fail;
	}

	this->conn = spa_dbus_connection_get(this->dbus_connection);
	if (this->conn == NULL) {
		spa_log_error(this->log, "failed to get dbus connection");
		res = -EIO;
		goto fail;
	}

	/*
	 * XXX: We should handle spa_dbus reconnecting, but we don't, so ref
	 * XXX: the handle so that we can keep it if spa_dbus unrefs it.
	 */
	dbus_connection_ref(this->conn);

	spa_hook_list_init(&this->hooks);

	this->device.iface = SPA_INTERFACE_INIT(
		SPA_TYPE_INTERFACE_Device,
				SPA_VERSION_DEVICE,
				&impl_device, this);

	this->dbus_monitor = spa_dbus_monitor_new(this->conn,
			BLUEZ_SERVICE, "/", monitor_interfaces,
			this->log, this);
	if (!this->dbus_monitor)
		goto fail;

	this->object_manager = spa_dbus_object_manager_new(this->conn,
			MIDI_OBJECT_PATH, this->log);
	if (!this->object_manager)
		goto fail;

	this->profile = spa_dbus_object_manager_register(this->object_manager,
			MIDI_PROFILE_PATH,
			profile_interfaces,
			sizeof(struct spa_dbus_local_object),
			this);
	if (!this->profile)
		goto fail;

	return 0;

fail:
	res = (res < 0) ? res : ((errno > 0) ? -errno : -EIO);
	impl_clear(handle);
	return res;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE_INTERFACE_Device,},
};

static int
impl_enum_interface_info(const struct spa_handle_factory *factory,
		const struct spa_interface_info **info,
		uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];

	return 1;
}

static const struct spa_dict_item info_items[] = {
	{ SPA_KEY_FACTORY_AUTHOR, "Pauli Virtanen <pav@iki.fi>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "Bluez5 MIDI connection" },
};

static const struct spa_dict info = SPA_DICT_INIT_ARRAY(info_items);

const struct spa_handle_factory spa_bluez5_midi_enum_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BLUEZ5_MIDI_ENUM,
	&info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
