/* Spa Bluez5 midi */
/* SPDX-FileCopyrightText: Copyright © 2022 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <sys/socket.h>
#include <unistd.h>

#include <spa/utils/defs.h>
#include <spa/utils/string.h>
#include <spa/utils/result.h>

#include "midi.h"

#include "bluez5-interface-gen.h"
#include "dbus-monitor.h"

#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT (&impl->log_topic)

#define MIDI_SERVER_PATH	"/midiserver%u"
#define MIDI_SERVICE_PATH	"/midiserver%u/service"
#define MIDI_CHR_PATH		"/midiserver%u/service/chr"
#define MIDI_DSC_PATH		"/midiserver%u/service/chr/dsc"

#define BLE_DEFAULT_MTU		23

struct impl
{
	struct spa_bt_midi_server this;

	struct spa_log_topic log_topic;
	struct spa_log *log;

	const struct spa_bt_midi_server_cb *cb;

	GDBusConnection *conn;
	struct dbus_monitor monitor;
	GDBusObjectManagerServer *manager;

	Bluez5GattCharacteristic1 *chr;

	void *user_data;

	uint32_t server_id;

	unsigned int write_acquired:1;
	unsigned int notify_acquired:1;
};

struct _MidiServerManagerProxy
{
	Bluez5GattManager1Proxy parent_instance;

	GCancellable *register_call;
	unsigned int registered:1;
};

G_DECLARE_FINAL_TYPE(MidiServerManagerProxy, midi_server_manager_proxy, MIDI_SERVER,
		MANAGER_PROXY, Bluez5GattManager1Proxy)
G_DEFINE_TYPE(MidiServerManagerProxy, midi_server_manager_proxy, BLUEZ5_TYPE_GATT_MANAGER1_PROXY)
#define MIDI_SERVER_TYPE_MANAGER_PROXY	(midi_server_manager_proxy_get_type())


/*
 * Characteristic user descriptor: not in BLE MIDI standard, but we
 * put a device name here in case we have multiple MIDI endpoints.
 */

static gboolean dsc_handle_read_value(Bluez5GattDescriptor1 *iface, GDBusMethodInvocation *invocation,
		GVariant *arg_options, gpointer user_data)
{
	struct impl *impl = user_data;
	const char *description = NULL;
	uint16_t offset = 0;
	int len;

	g_variant_lookup(arg_options, "offset", "q", &offset);

	if (impl->cb->get_description)
		description = impl->cb->get_description(impl->user_data);
	if (!description)
		description = "";

	len = strlen(description);
	if (offset > len) {
		g_dbus_method_invocation_return_dbus_error(invocation,
				"org.freedesktop.DBus.Error.InvalidArgs",
				"Invalid arguments");
		return TRUE;
	}

	bluez5_gatt_descriptor1_complete_read_value(iface,
			invocation, description + offset);
	return TRUE;
}

static int export_dsc(struct impl *impl)
{
	static const char * const flags[] = { "encrypt-read", NULL };
	GDBusObjectSkeleton *skeleton = NULL;
	Bluez5GattDescriptor1 *iface = NULL;
	int res = -ENOMEM;
	char path[128];

	iface = bluez5_gatt_descriptor1_skeleton_new();
	if (!iface)
		goto done;

	spa_scnprintf(path, sizeof(path), MIDI_DSC_PATH, impl->server_id);
	skeleton = g_dbus_object_skeleton_new(path);
	if (!skeleton)
		goto done;
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(iface));

	bluez5_gatt_descriptor1_set_uuid(iface, BT_GATT_CHARACTERISTIC_USER_DESCRIPTION_UUID);
	spa_scnprintf(path, sizeof(path), MIDI_CHR_PATH, impl->server_id);
	bluez5_gatt_descriptor1_set_characteristic(iface, path);
	bluez5_gatt_descriptor1_set_flags(iface, flags);

	g_signal_connect(iface, "handle-read-value", G_CALLBACK(dsc_handle_read_value), impl);

	g_dbus_object_manager_server_export(impl->manager, skeleton);

	spa_log_debug(impl->log, "MIDI GATT Descriptor exported, path=%s",
			g_dbus_object_get_object_path(G_DBUS_OBJECT(skeleton)));

	res = 0;

done:
	g_clear_object(&iface);
	g_clear_object(&skeleton);
	return res;
}


/*
 * MIDI characteristic
 */

static gboolean chr_handle_read_value(Bluez5GattCharacteristic1 *iface,
		GDBusMethodInvocation *invocation, GVariant *arg_options,
		gpointer user_data)
{
	/* BLE MIDI-1.0: returns empty value */
	bluez5_gatt_characteristic1_complete_read_value(iface, invocation, "");
	return TRUE;
}

static void chr_change_acquired(struct impl *impl, bool write, bool enabled)
{
	if (write) {
		impl->write_acquired = enabled;
		bluez5_gatt_characteristic1_set_write_acquired(impl->chr, enabled);
	} else {
		impl->notify_acquired = enabled;
		bluez5_gatt_characteristic1_set_notify_acquired(impl->chr, enabled);
	}
}

static int create_socketpair(int fds[2])
{
	if (socketpair(AF_LOCAL, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds) < 0)
		return -errno;
	return 0;
}

static gboolean chr_handle_acquire(Bluez5GattCharacteristic1 *object,
		GDBusMethodInvocation *invocation,
		GUnixFDList *dummy, GVariant *arg_options,
		bool write, gpointer user_data)
{
	struct impl *impl = user_data;
	const char *err_msg = "Failed";
	uint16_t mtu = BLE_DEFAULT_MTU;
	gint fds[2] = {-1, -1};
	int res;
	GUnixFDList *fd_list = NULL;
	GVariant *fd_handle = NULL;
	GError *err = NULL;

	if ((write && (impl->cb->acquire_write == NULL)) ||
			(!write && (impl->cb->acquire_notify == NULL))) {
		err_msg = "Not supported";
		goto fail;
	}
	if ((write && impl->write_acquired) ||
			(!write && impl->notify_acquired)) {
		err_msg = "Already acquired";
		goto fail;
	}

	g_variant_lookup(arg_options, "mtu", "q", &mtu);

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

	fd_handle = g_variant_new_handle(0);
	fd_list = g_unix_fd_list_new_from_array(&fds[1], 1);
	fds[1] = -1;

	chr_change_acquired(impl, write, true);

	if (write) {
		bluez5_gatt_characteristic1_complete_acquire_write(
			object, invocation, fd_list, fd_handle, mtu);
	} else {
		bluez5_gatt_characteristic1_complete_acquire_notify(
			object, invocation, fd_list, fd_handle, mtu);
	}

	g_clear_object(&fd_list);
	return TRUE;

fail:
	if (fds[0] >= 0)
		close(fds[0]);
	if (fds[1] >= 0)
		close(fds[1]);

	if (err)
		g_error_free(err);
	g_clear_pointer(&fd_handle, g_variant_unref);
	g_clear_object(&fd_list);
	g_dbus_method_invocation_return_dbus_error(invocation,
			"org.freedesktop.DBus.Error.Failed", err_msg);
	return TRUE;

}

static gboolean chr_handle_acquire_write(Bluez5GattCharacteristic1 *object,
		GDBusMethodInvocation *invocation,
		GUnixFDList *fd_list, GVariant *arg_options,
		gpointer user_data)
{
	return chr_handle_acquire(object, invocation, fd_list, arg_options, true, user_data);
}

static gboolean chr_handle_acquire_notify(Bluez5GattCharacteristic1 *object,
		GDBusMethodInvocation *invocation,
		GUnixFDList *fd_list, GVariant *arg_options,
		gpointer user_data)
{
	return chr_handle_acquire(object, invocation, fd_list, arg_options, false, user_data);
}

static int export_chr(struct impl *impl)
{
	static const char * const flags[] = { "encrypt-read", "write-without-response",
		"encrypt-write", "encrypt-notify", NULL };
	GDBusObjectSkeleton *skeleton = NULL;
	Bluez5GattCharacteristic1 *iface = NULL;
	int res = -ENOMEM;
	char path[128];

	iface = bluez5_gatt_characteristic1_skeleton_new();
	if (!iface)
		goto done;

	spa_scnprintf(path, sizeof(path), MIDI_CHR_PATH, impl->server_id);
	skeleton = g_dbus_object_skeleton_new(path);
	if (!skeleton)
		goto done;
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(iface));

	bluez5_gatt_characteristic1_set_uuid(iface, BT_MIDI_CHR_UUID);
	spa_scnprintf(path, sizeof(path), MIDI_SERVICE_PATH, impl->server_id);
	bluez5_gatt_characteristic1_set_service(iface, path);
	bluez5_gatt_characteristic1_set_write_acquired(iface, FALSE);
	bluez5_gatt_characteristic1_set_notify_acquired(iface, FALSE);
	bluez5_gatt_characteristic1_set_flags(iface, flags);

	g_signal_connect(iface, "handle-read-value", G_CALLBACK(chr_handle_read_value), impl);
	g_signal_connect(iface, "handle-acquire-write", G_CALLBACK(chr_handle_acquire_write), impl);
	g_signal_connect(iface, "handle-acquire-notify", G_CALLBACK(chr_handle_acquire_notify), impl);

	g_dbus_object_manager_server_export(impl->manager, skeleton);

	impl->chr = g_object_ref(iface);

	spa_log_debug(impl->log, "MIDI GATT Characteristic exported, path=%s",
			g_dbus_object_get_object_path(G_DBUS_OBJECT(skeleton)));

	res = 0;

done:
	g_clear_object(&iface);
	g_clear_object(&skeleton);
	return res;
}


/*
 * MIDI service
 */

static int export_service(struct impl *impl)
{
	GDBusObjectSkeleton *skeleton = NULL;
	Bluez5GattService1 *iface = NULL;
	int res = -ENOMEM;
	char path[128];

	iface = bluez5_gatt_service1_skeleton_new();
	if (!iface)
		goto done;

	spa_scnprintf(path, sizeof(path), MIDI_SERVICE_PATH, impl->server_id);
	skeleton = g_dbus_object_skeleton_new(path);
	if (!skeleton)
		goto done;
	g_dbus_object_skeleton_add_interface(skeleton, G_DBUS_INTERFACE_SKELETON(iface));

	bluez5_gatt_service1_set_uuid(iface, BT_MIDI_SERVICE_UUID);
	bluez5_gatt_service1_set_primary(iface, TRUE);

	g_dbus_object_manager_server_export(impl->manager, skeleton);

	spa_log_debug(impl->log, "MIDI GATT Service exported, path=%s",
			g_dbus_object_get_object_path(G_DBUS_OBJECT(skeleton)));

	res = 0;

done:
	g_clear_object(&iface);
	g_clear_object(&skeleton);
	return res;
}


/*
 * Registration on all GattManagers
 */

static void manager_register_application_reply(GObject *source_object, GAsyncResult *res, gpointer user_data)
{
	MidiServerManagerProxy *manager = MIDI_SERVER_MANAGER_PROXY(source_object);
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

static int manager_register_application(struct impl *impl, MidiServerManagerProxy *manager)
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

static void midi_server_manager_proxy_init(MidiServerManagerProxy *manager)
{
}

static void midi_server_manager_proxy_finalize(GObject *object)
{
	MidiServerManagerProxy *manager = MIDI_SERVER_MANAGER_PROXY(object);

	g_cancellable_cancel(manager->register_call);
	g_clear_object(&manager->register_call);
}

static void midi_server_manager_proxy_class_init(MidiServerManagerProxyClass *klass)
{
	GObjectClass *object_class = (GObjectClass *) klass;

	object_class->finalize = midi_server_manager_proxy_finalize;
}

static void manager_update(struct dbus_monitor *monitor, GDBusInterface *iface)
{
	struct impl *impl = SPA_CONTAINER_OF(monitor, struct impl, monitor);

	manager_register_application(impl, MIDI_SERVER_MANAGER_PROXY(iface));
}

static void manager_clear(struct dbus_monitor *monitor, GDBusInterface *iface)
{
	midi_server_manager_proxy_finalize(G_OBJECT(iface));
}

static void on_name_owner_change(struct dbus_monitor *monitor)
{
	struct impl *impl = SPA_CONTAINER_OF(monitor, struct impl, monitor);

	/*
	 * BlueZ disappeared/appeared. It does not appear to close the sockets
	 * it quits, so we should force the chr release now.
	 */
	if (impl->cb->release)
		impl->cb->release(impl->user_data);
	chr_change_acquired(impl, true, false);
	chr_change_acquired(impl, false, false);
}

static void monitor_start(struct impl *impl)
{
	struct dbus_monitor_proxy_type proxy_types[] = {
		{ BLUEZ_GATT_MANAGER_INTERFACE, MIDI_SERVER_TYPE_MANAGER_PROXY, manager_update, manager_clear },
		{ NULL, BLUEZ5_TYPE_OBJECT_PROXY, NULL, NULL },
		{ NULL, G_TYPE_INVALID, NULL, NULL },
	};

	SPA_STATIC_ASSERT(SPA_N_ELEMENTS(proxy_types) <= DBUS_MONITOR_MAX_TYPES);

	dbus_monitor_init(&impl->monitor, BLUEZ5_TYPE_OBJECT_MANAGER_CLIENT,
			impl->log, impl->conn, BLUEZ_SERVICE, "/", proxy_types,
			on_name_owner_change);
}


/*
 * Object registration
 */

static int export_objects(struct impl *impl)
{
	int res = 0;
	char path[128];

	spa_scnprintf(path, sizeof(path), MIDI_SERVER_PATH, impl->server_id);
	impl->manager = g_dbus_object_manager_server_new(path);
	if (!impl->manager){
		spa_log_error(impl->log, "Creating GDBus object manager failed");
		goto fail;
	}

	if ((res = export_service(impl)) < 0)
		goto fail;

	if ((res = export_chr(impl)) < 0)
		goto fail;

	if ((res = export_dsc(impl)) < 0)
		goto fail;

	g_dbus_object_manager_server_set_connection(impl->manager, impl->conn);

	return 0;

fail:
	res = (res < 0) ? res : ((errno > 0) ? -errno : -EIO);

	spa_log_error(impl->log, "Failed to register BLE MIDI services in DBus: %s",
			spa_strerror(res));

	g_clear_object(&impl->manager);

	return res;
}

struct spa_bt_midi_server *spa_bt_midi_server_new(const struct spa_bt_midi_server_cb *cb,
		GDBusConnection *conn, struct spa_log *log, void *user_data)
{
	static unsigned int server_id = 0;
	struct impl *impl;
	char path[128];
	int res = 0;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		goto fail;

	impl->server_id = server_id++;
	impl->user_data = user_data;
	impl->cb = cb;
	impl->log = log;
	impl->log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.midi.server");
	impl->conn = conn;
	spa_log_topic_init(impl->log, &impl->log_topic);

	if ((res = export_objects(impl)) < 0)
		goto fail;

	monitor_start(impl);

	g_object_ref(impl->conn);

	spa_scnprintf(path, sizeof(path), MIDI_CHR_PATH, impl->server_id);
	impl->this.chr_path = strdup(path);

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

	free((void *)impl->this.chr_path);
	g_clear_object(&impl->chr);
	dbus_monitor_clear(&impl->monitor);
	g_clear_object(&impl->manager);
	g_clear_object(&impl->conn);

	free(impl);
}

void spa_bt_midi_server_released(struct spa_bt_midi_server *server, bool write)
{
	struct impl *impl = SPA_CONTAINER_OF(server, struct impl, this);

	chr_change_acquired(impl, write, false);
}
