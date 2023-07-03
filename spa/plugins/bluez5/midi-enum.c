/* Spa midi dbus */
/* SPDX-FileCopyrightText: Copyright © 2022 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
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

#include "midi.h"
#include "config.h"

#include "bluez5-interface-gen.h"
#include "dbus-monitor.h"

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

	GDBusConnection *conn;
	struct dbus_monitor monitor;
	GDBusObjectManagerServer *manager;

	struct spa_hook_list hooks;

	uint32_t id;
};

struct _MidiEnumCharacteristicProxy
{
	Bluez5GattCharacteristic1Proxy parent_instance;

	struct impl *impl;

	gchar *description;
	uint32_t id;
	GCancellable *read_call;
	GCancellable *dsc_call;
	unsigned int node_emitted:1;
	unsigned int read_probed:1;
	unsigned int read_done:1;
	unsigned int dsc_probed:1;
	unsigned int dsc_done:1;
};

G_DECLARE_FINAL_TYPE(MidiEnumCharacteristicProxy, midi_enum_characteristic_proxy, MIDI_ENUM,
		CHARACTERISTIC_PROXY, Bluez5GattCharacteristic1Proxy)
G_DEFINE_TYPE(MidiEnumCharacteristicProxy, midi_enum_characteristic_proxy, BLUEZ5_TYPE_GATT_CHARACTERISTIC1_PROXY)
#define MIDI_ENUM_TYPE_CHARACTERISTIC_PROXY	(midi_enum_characteristic_proxy_get_type())

struct _MidiEnumManagerProxy
{
	Bluez5GattManager1Proxy parent_instance;

	GCancellable *register_call;
	unsigned int registered:1;
};

G_DECLARE_FINAL_TYPE(MidiEnumManagerProxy, midi_enum_manager_proxy, MIDI_ENUM,
		MANAGER_PROXY, Bluez5GattManager1Proxy)
G_DEFINE_TYPE(MidiEnumManagerProxy, midi_enum_manager_proxy, BLUEZ5_TYPE_GATT_MANAGER1_PROXY)
#define MIDI_ENUM_TYPE_MANAGER_PROXY	(midi_enum_manager_proxy_get_type())


static void emit_chr_node(struct impl *impl, MidiEnumCharacteristicProxy *chr, Bluez5Device1 *device)
{
	struct spa_device_object_info info;
	char nick[512], class[16];
	struct spa_dict_item items[23];
	uint32_t n_items = 0;
	const char *path = g_dbus_proxy_get_object_path(G_DBUS_PROXY(chr));
	const char *alias = bluez5_device1_get_alias(device);

	spa_log_debug(impl->log, "emit node for path=%s", path);

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
			alias ? alias : bluez5_device1_get_name(device));
	if (chr->description && chr->description[0] != '\0') {
		spa_scnprintf(nick, sizeof(nick), "%s (%s)", alias, chr->description);
		items[n_items++] = SPA_DICT_ITEM_INIT("node.nick", nick);
	}
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_ICON, bluez5_device1_get_icon(device));
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_PATH, path);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_ADDRESS, bluez5_device1_get_address(device));
	snprintf(class, sizeof(class), "0x%06x", bluez5_device1_get_class(device));
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_CLASS, class);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_ROLE, "client");

	info.props = &SPA_DICT_INIT(items, n_items);
	spa_device_emit_object_info(&impl->hooks, chr->id, &info);
}

static void remove_chr_node(struct impl *impl, MidiEnumCharacteristicProxy *chr)
{
	spa_log_debug(impl->log, "remove node for path=%s", g_dbus_proxy_get_object_path(G_DBUS_PROXY(chr)));

	spa_device_emit_object_info(&impl->hooks, chr->id, NULL);
}

static void check_chr_node(struct impl *impl, MidiEnumCharacteristicProxy *chr);

static void read_probe_reply(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	MidiEnumCharacteristicProxy *chr = MIDI_ENUM_CHARACTERISTIC_PROXY(source_object);
	struct impl *impl = user_data;
	gchar *value = NULL;
	GError *err = NULL;

	bluez5_gatt_characteristic1_call_read_value_finish(
		BLUEZ5_GATT_CHARACTERISTIC1(source_object), &value, res, &err);

	if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		/* Operation canceled: user_data may be invalid by now */
		g_error_free(err);
		goto done;
	}
	if (err) {
		spa_log_error(impl->log, "%s.ReadValue() failed: %s",
				BLUEZ_GATT_CHR_INTERFACE,
				err->message);
		g_error_free(err);
		goto done;
	}

	g_free(value);

	spa_log_debug(impl->log, "MIDI GATT read probe done for path=%s",
			g_dbus_proxy_get_object_path(G_DBUS_PROXY(chr)));

	chr->read_done = true;

	check_chr_node(impl, chr);

done:
	g_clear_object(&chr->read_call);
}

static int read_probe(struct impl *impl, MidiEnumCharacteristicProxy *chr)
{
	GVariantBuilder builder;
	GVariant *options;

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
			g_dbus_proxy_get_object_path(G_DBUS_PROXY(chr)));

	chr->read_call = g_cancellable_new();

	g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
	options = g_variant_builder_end(&builder);

	bluez5_gatt_characteristic1_call_read_value(BLUEZ5_GATT_CHARACTERISTIC1(chr),
			options,
			chr->read_call,
			read_probe_reply,
			impl);

	return 0;
}

static Bluez5GattDescriptor1 *find_dsc(struct impl *impl, MidiEnumCharacteristicProxy *chr)
{
	const char *path = g_dbus_proxy_get_object_path(G_DBUS_PROXY(chr));
	Bluez5GattDescriptor1 *found = NULL;;
	GList *objects;

	objects = g_dbus_object_manager_get_objects(dbus_monitor_manager(&impl->monitor));

	for (GList *llo = g_list_first(objects); llo; llo = llo->next) {
		GList *interfaces = g_dbus_object_get_interfaces(G_DBUS_OBJECT(llo->data));

		for (GList *lli = g_list_first(interfaces); lli; lli = lli->next) {
			Bluez5GattDescriptor1 *dsc;

			if (!BLUEZ5_IS_GATT_DESCRIPTOR1(lli->data))
				continue;

			dsc = BLUEZ5_GATT_DESCRIPTOR1(lli->data);

			if (!spa_streq(bluez5_gatt_descriptor1_get_uuid(dsc),
							BT_GATT_CHARACTERISTIC_USER_DESCRIPTION_UUID))
				continue;

			if (spa_streq(bluez5_gatt_descriptor1_get_characteristic(dsc), path)) {
				found = dsc;
				break;
			}
		}
		g_list_free_full(interfaces, g_object_unref);

		if (found)
			break;
	}
	g_list_free_full(objects, g_object_unref);

	return found;
}

static void read_dsc_reply(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	MidiEnumCharacteristicProxy *chr = MIDI_ENUM_CHARACTERISTIC_PROXY(user_data);
	struct impl *impl = chr->impl;
	gchar *value = NULL;
	GError *err = NULL;

	chr->dsc_done = true;

	bluez5_gatt_descriptor1_call_read_value_finish(
		BLUEZ5_GATT_DESCRIPTOR1(source_object), &value, res, &err);

	if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		/* Operation canceled: user_data may be invalid by now */
		g_error_free(err);
		goto done;
	}
	if (err) {
		spa_log_error(impl->log, "%s.ReadValue() failed: %s",
				BLUEZ_GATT_DSC_INTERFACE,
				err->message);
		g_error_free(err);
		goto done;
	}

	spa_log_debug(impl->log, "MIDI GATT read probe done for path=%s",
			g_dbus_proxy_get_object_path(G_DBUS_PROXY(chr)));

	g_free(chr->description);
	chr->description = value;

	spa_log_debug(impl->log, "MIDI GATT user descriptor value: '%s'",
			chr->description);

	check_chr_node(impl, chr);

done:
	g_clear_object(&chr->dsc_call);
}

static int read_dsc(struct impl *impl, MidiEnumCharacteristicProxy *chr)
{
	Bluez5GattDescriptor1 *dsc;
	GVariant *options;
	GVariantBuilder builder;

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
			g_dbus_proxy_get_object_path(G_DBUS_PROXY(dsc)));

	chr->dsc_call = g_cancellable_new();

	g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
	options = g_variant_builder_end(&builder);

	bluez5_gatt_descriptor1_call_read_value(BLUEZ5_GATT_DESCRIPTOR1(dsc),
			options,
			chr->dsc_call,
			read_dsc_reply,
			chr);

	return 0;
}

static int read_probe_reset(struct impl *impl, MidiEnumCharacteristicProxy *chr)
{
	g_cancellable_cancel(chr->read_call);
	g_clear_object(&chr->read_call);

	g_cancellable_cancel(chr->dsc_call);
	g_clear_object(&chr->dsc_call);

	chr->read_probed = false;
	chr->read_done = false;
	chr->dsc_probed = false;
	chr->dsc_done = false;
	return 0;
}

static void lookup_chr_node(struct impl *impl, MidiEnumCharacteristicProxy *chr,
		Bluez5GattService1 **service, Bluez5Device1 **device)
{
	GDBusObject *object;
	const char *service_path;
	const char *device_path;

	*service = NULL;
	*device = NULL;

	service_path = bluez5_gatt_characteristic1_get_service(BLUEZ5_GATT_CHARACTERISTIC1(chr));
	if (!service_path)
		return;

	object = g_dbus_object_manager_get_object(dbus_monitor_manager(&impl->monitor), service_path);
	if (object) {
		GDBusInterface *iface = g_dbus_object_get_interface(object, BLUEZ_GATT_SERVICE_INTERFACE);
		*service = BLUEZ5_GATT_SERVICE1(iface);
	}

	if (!*service)
		return;

	device_path = bluez5_gatt_service1_get_device(*service);
	if (!device_path)
		return;

	object = g_dbus_object_manager_get_object(dbus_monitor_manager(&impl->monitor), device_path);
	if (object) {
		GDBusInterface *iface = g_dbus_object_get_interface(object, BLUEZ_DEVICE_INTERFACE);
		*device = BLUEZ5_DEVICE1(iface);
	}
}

static void check_chr_node(struct impl *impl, MidiEnumCharacteristicProxy *chr)
{
	Bluez5GattService1 *service;
	Bluez5Device1 *device;
	bool available;

	lookup_chr_node(impl, chr, &service, &device);

	if (!device || !bluez5_device1_get_connected(device)) {
		/* Retry read probe on each connection */
		read_probe_reset(impl, chr);
	}

	spa_log_debug(impl->log,
			"At %s, connected:%d resolved:%d",
			g_dbus_proxy_get_object_path(G_DBUS_PROXY(chr)),
			bluez5_device1_get_connected(device),
			bluez5_device1_get_services_resolved(device));

	available = service && device &&
			bluez5_device1_get_connected(device) &&
			bluez5_device1_get_services_resolved(device) &&
			spa_streq(bluez5_gatt_service1_get_uuid(service), BT_MIDI_SERVICE_UUID) &&
			spa_streq(bluez5_gatt_characteristic1_get_uuid(BLUEZ5_GATT_CHARACTERISTIC1(chr)),
					BT_MIDI_CHR_UUID);

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

static GList *get_all_valid_chr(struct impl *impl)
{
	GList *lst = NULL;
	GList *objects;

	if (!dbus_monitor_manager(&impl->monitor)) {
		/* Still initializing (or it failed) */
		return NULL;
	}

	objects = g_dbus_object_manager_get_objects(dbus_monitor_manager(&impl->monitor));
	for (GList *p = g_list_first(objects); p; p = p->next) {
		GList *interfaces = g_dbus_object_get_interfaces(G_DBUS_OBJECT(p->data));

		for (GList *p2 = g_list_first(interfaces); p2; p2 = p2->next) {
			MidiEnumCharacteristicProxy *chr;

			if (!MIDI_ENUM_IS_CHARACTERISTIC_PROXY(p2->data))
				continue;

			chr = MIDI_ENUM_CHARACTERISTIC_PROXY(p2->data);
			if (chr->impl == NULL)
				continue;

			lst = g_list_append(lst, g_object_ref(chr));
		}
		g_list_free_full(interfaces, g_object_unref);
	}
	g_list_free_full(objects, g_object_unref);

	return lst;
}

static void check_all_nodes(struct impl *impl)
{
	/*
	 * Check if the nodes we have emitted are in sync with connected devices.
	 */

	GList *chrs = get_all_valid_chr(impl);

	for (GList *p = chrs; p; p = p->next)
		check_chr_node(impl, MIDI_ENUM_CHARACTERISTIC_PROXY(p->data));

	g_list_free_full(chrs, g_object_unref);
}

static void manager_register_application_reply(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	MidiEnumManagerProxy *manager = MIDI_ENUM_MANAGER_PROXY(source_object);
	struct impl *impl = user_data;
	GError *err = NULL;

	bluez5_gatt_manager1_call_register_application_finish(
		BLUEZ5_GATT_MANAGER1(source_object), res, &err);

	if (g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
		/* Operation canceled: user_data may be invalid by now */
		g_error_free(err);
		goto done;
	}
	if (err) {
		spa_log_error(impl->log, "%s.RegisterApplication() failed: %s",
				BLUEZ_GATT_MANAGER_INTERFACE,
				err->message);
		g_error_free(err);
		goto done;
	}

	manager->registered = true;

done:
	g_clear_object(&manager->register_call);
}

static int manager_register_application(struct impl *impl, MidiEnumManagerProxy *manager)
{
	GVariantBuilder builder;
	GVariant *options;

	if (manager->registered)
		return 0;
	if (manager->register_call)
		return -EBUSY;

	spa_log_debug(impl->log, "%s.RegisterApplication(%s) on %s",
			BLUEZ_GATT_MANAGER_INTERFACE,
			g_dbus_object_manager_get_object_path(G_DBUS_OBJECT_MANAGER(impl->manager)),
			g_dbus_proxy_get_object_path(G_DBUS_PROXY(manager)));

	manager->register_call = g_cancellable_new();

	g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));
	options = g_variant_builder_end(&builder);

	bluez5_gatt_manager1_call_register_application(BLUEZ5_GATT_MANAGER1(manager),
			g_dbus_object_manager_get_object_path(G_DBUS_OBJECT_MANAGER(impl->manager)),
			options,
			manager->register_call,
			manager_register_application_reply,
			impl);

	return 0;
}

/*
 * DBus monitoring (Glib)
 */

static void midi_enum_characteristic_proxy_init(MidiEnumCharacteristicProxy *chr)
{
}

static void midi_enum_characteristic_proxy_finalize(GObject *object)
{
	MidiEnumCharacteristicProxy *chr = MIDI_ENUM_CHARACTERISTIC_PROXY(object);

	g_cancellable_cancel(chr->read_call);
	g_clear_object(&chr->read_call);

	g_cancellable_cancel(chr->dsc_call);
	g_clear_object(&chr->dsc_call);

	if (chr->impl && chr->node_emitted)
		remove_chr_node(chr->impl, chr);

	chr->impl = NULL;

	g_free(chr->description);
	chr->description = NULL;
}

static void midi_enum_characteristic_proxy_class_init(MidiEnumCharacteristicProxyClass *klass)
{
	GObjectClass *object_class = (GObjectClass *) klass;

	object_class->finalize = midi_enum_characteristic_proxy_finalize;
}

static void midi_enum_manager_proxy_init(MidiEnumManagerProxy *manager)
{
}

static void midi_enum_manager_proxy_finalize(GObject *object)
{
	MidiEnumManagerProxy *manager = MIDI_ENUM_MANAGER_PROXY(object);

	g_cancellable_cancel(manager->register_call);
	g_clear_object(&manager->register_call);
}

static void midi_enum_manager_proxy_class_init(MidiEnumManagerProxyClass *klass)
{
	GObjectClass *object_class = (GObjectClass *) klass;

	object_class->finalize = midi_enum_manager_proxy_finalize;
}

static void manager_update(struct dbus_monitor *monitor, GDBusInterface *iface)
{
	struct impl *impl = SPA_CONTAINER_OF(monitor, struct impl, monitor);

	manager_register_application(impl, MIDI_ENUM_MANAGER_PROXY(iface));
}

static void manager_clear(struct dbus_monitor *monitor, GDBusInterface *iface)
{
	midi_enum_manager_proxy_finalize(G_OBJECT(iface));
}

static void device_update(struct dbus_monitor *monitor, GDBusInterface *iface)
{
	struct impl *impl = SPA_CONTAINER_OF(monitor, struct impl, monitor);

	check_all_nodes(impl);
}

static void service_update(struct dbus_monitor *monitor, GDBusInterface *iface)
{
	struct impl *impl = SPA_CONTAINER_OF(monitor, struct impl, monitor);
	Bluez5GattService1 *service = BLUEZ5_GATT_SERVICE1(iface);

	if (!spa_streq(bluez5_gatt_service1_get_uuid(service), BT_MIDI_SERVICE_UUID))
		return;

	check_all_nodes(impl);
}

static void chr_update(struct dbus_monitor *monitor, GDBusInterface *iface)
{
	struct impl *impl = SPA_CONTAINER_OF(monitor, struct impl, monitor);
	MidiEnumCharacteristicProxy *chr = MIDI_ENUM_CHARACTERISTIC_PROXY(iface);

	if (!spa_streq(bluez5_gatt_characteristic1_get_uuid(BLUEZ5_GATT_CHARACTERISTIC1(chr)),
					BT_MIDI_CHR_UUID))
		return;

	if (chr->impl == NULL) {
		chr->impl = impl;
		chr->id = ++impl->id;
	}

	check_chr_node(impl, chr);
}

static void chr_clear(struct dbus_monitor *monitor, GDBusInterface *iface)
{
	midi_enum_characteristic_proxy_finalize(G_OBJECT(iface));
}

static void monitor_start(struct impl *impl)
{
	struct dbus_monitor_proxy_type proxy_types[] = {
		{ BLUEZ_DEVICE_INTERFACE, BLUEZ5_TYPE_DEVICE1_PROXY, device_update, NULL },
		{ BLUEZ_GATT_MANAGER_INTERFACE, MIDI_ENUM_TYPE_MANAGER_PROXY, manager_update, manager_clear },
		{ BLUEZ_GATT_SERVICE_INTERFACE, BLUEZ5_TYPE_GATT_SERVICE1_PROXY, service_update, NULL },
		{ BLUEZ_GATT_CHR_INTERFACE, MIDI_ENUM_TYPE_CHARACTERISTIC_PROXY, chr_update, chr_clear },
		{ BLUEZ_GATT_DSC_INTERFACE, BLUEZ5_TYPE_GATT_DESCRIPTOR1_PROXY, NULL, NULL },
		{ NULL, BLUEZ5_TYPE_OBJECT_PROXY, NULL, NULL },
		{ NULL, G_TYPE_INVALID, NULL, NULL }
	};

	SPA_STATIC_ASSERT(SPA_N_ELEMENTS(proxy_types) <= DBUS_MONITOR_MAX_TYPES);

	dbus_monitor_init(&impl->monitor, BLUEZ5_TYPE_OBJECT_MANAGER_CLIENT,
			impl->log, impl->conn, BLUEZ_SERVICE, "/", proxy_types, NULL);
}

/*
 * DBus GATT profile, to enable BlueZ autoconnect
 */

static gboolean profile_handle_release(Bluez5GattProfile1 *iface, GDBusMethodInvocation *invocation)
{
	bluez5_gatt_profile1_complete_release(iface, invocation);
	return TRUE;
}

static int export_profile(struct impl *impl)
{
	static const char *uuids[] = { BT_MIDI_SERVICE_UUID, NULL };
	GDBusObjectSkeleton *skeleton = NULL;
	Bluez5GattProfile1 *iface = NULL;
	int res = -ENOMEM;

	iface = bluez5_gatt_profile1_skeleton_new();
	if (!iface)
		goto done;

	skeleton = g_dbus_object_skeleton_new(MIDI_PROFILE_PATH);
	if (!skeleton)
		goto done;
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(iface));

	bluez5_gatt_profile1_set_uuids(iface, uuids);
	g_signal_connect(iface, "handle-release", G_CALLBACK(profile_handle_release), NULL);

	g_dbus_object_manager_server_export(impl->manager, skeleton);

	spa_log_debug(impl->log, "MIDI GATT Profile exported, path=%s",
			g_dbus_object_get_object_path(G_DBUS_OBJECT(skeleton)));

	res = 0;

done:
	g_clear_object(&iface);
	g_clear_object(&skeleton);
	return res;
}

/*
 * Monitor impl
 */

static int impl_device_add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;
	GList *chrs;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	chrs = get_all_valid_chr(this);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	for (GList *p = g_list_first(chrs); p; p = p->next) {
		MidiEnumCharacteristicProxy *chr = MIDI_ENUM_CHARACTERISTIC_PROXY(p->data);
		Bluez5Device1 *device;
		Bluez5GattService1 *service;

		if (!chr->node_emitted)
			continue;

		lookup_chr_node(this, chr, &service, &device);
		if (device)
			emit_chr_node(this, chr, device);
	}
	g_list_free_full(chrs, g_object_unref);

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

	dbus_monitor_clear(&this->monitor);
	g_clear_object(&this->manager);
	g_clear_object(&this->conn);

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
	GError *error = NULL;
	int res = 0;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);

	if (this->log == NULL)
		return -EINVAL;

	spa_log_topic_init(this->log, &log_topic);

	if (!(info && spa_atob(spa_dict_lookup(info, SPA_KEY_API_GLIB_MAINLOOP)))) {
		spa_log_error(this->log, "Glib mainloop is not usable: %s not set",
				SPA_KEY_API_GLIB_MAINLOOP);
		return -EINVAL;
	}

	spa_hook_list_init(&this->hooks);

	this->device.iface = SPA_INTERFACE_INIT(
		SPA_TYPE_INTERFACE_Device,
				SPA_VERSION_DEVICE,
				&impl_device, this);

	this->conn = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (!this->conn) {
		spa_log_error(this->log, "Creating GDBus connection failed: %s",
				error->message);
		g_error_free(error);
		goto fail;
	}

	this->manager = g_dbus_object_manager_server_new(MIDI_OBJECT_PATH);
	if (!this->manager){
		spa_log_error(this->log, "Creating GDBus object manager failed");
		goto fail;
	}

	if ((res = export_profile(this)) < 0)
		goto fail;

	g_dbus_object_manager_server_set_connection(this->manager, this->conn);

	monitor_start(this);

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
