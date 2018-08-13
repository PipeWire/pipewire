/* Spa V4l2 Monitor
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <dbus/dbus.h>

#include <spa/support/log.h>
#include <spa/support/type-map.h>
#include <spa/support/loop.h>
#include <spa/support/dbus.h>
#include <spa/support/plugin.h>
#include <spa/monitor/monitor.h>

#include "a2dp-codecs.h"
#include "defs.h"

#define NAME "bluez5-monitor"

struct type {
	uint32_t handle_factory;
	struct spa_type_monitor monitor;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->handle_factory = spa_type_map_get_id(map, SPA_TYPE__HandleFactory);
	spa_type_monitor_map(map, &type->monitor);
}

struct spa_bt_monitor {
	struct spa_handle handle;
	struct spa_monitor monitor;

	struct type type;
	struct spa_type_map *map;
	struct spa_log *log;
	struct spa_dbus *dbus;
	struct spa_dbus_connection *dbus_connection;
	DBusConnection *conn;

	const struct spa_monitor_callbacks *callbacks;
	void *callbacks_data;

	uint32_t index;

	uint32_t count;

	struct spa_list adapter_list;
	struct spa_list device_list;
	struct spa_list transport_list;
};

struct spa_handle_factory spa_a2dp_sink_factory;

static void fill_item(struct spa_bt_monitor *this, struct spa_bt_transport *transport,
		struct spa_pod **result, struct spa_pod_builder *builder)
{
	struct type *t = &this->type;
	char trans[16];

	spa_pod_builder_add(builder,
		"<", 0, t->monitor.MonitorItem,
		":", t->monitor.id,      "s", transport->path,
		":", t->monitor.flags,   "i", 0,
		":", t->monitor.state,   "i", SPA_MONITOR_ITEM_STATE_AVAILABLE,
		":", t->monitor.name,    "s", transport->path,
		":", t->monitor.klass,   "s", "Adapter/Bluetooth",
		":", t->monitor.factory, "p", t->handle_factory, &spa_a2dp_sink_factory,
		":", t->monitor.info,    "[",
		NULL);

	snprintf(trans, sizeof(trans), "%p", transport);

	spa_pod_builder_add(builder,
		    "s", "device.api",  "s", "bluez5",
		    "s", "device.name",  "s", transport->device->name,
		    "s", "device.icon",  "s", transport->device->icon,
		    "s", "device.bluez5.address",  "s", transport->device->address,
		    "s", "bluez5.transport",  "s", trans,
		    NULL);

	*result = spa_pod_builder_add(builder, "]>", NULL);
}

static uint8_t a2dp_default_bitpool(struct spa_bt_monitor *monitor, uint8_t freq, uint8_t mode) {
	/* These bitpool values were chosen based on the A2DP spec recommendation */
	switch (freq) {
	case SBC_SAMPLING_FREQ_16000:
        case SBC_SAMPLING_FREQ_32000:
		return 53;

	case SBC_SAMPLING_FREQ_44100:
		switch (mode) {
		case SBC_CHANNEL_MODE_MONO:
		case SBC_CHANNEL_MODE_DUAL_CHANNEL:
			return 31;

		case SBC_CHANNEL_MODE_STEREO:
		case SBC_CHANNEL_MODE_JOINT_STEREO:
			return 53;
		}

		spa_log_warn(monitor->log, "Invalid channel mode %u", mode);
		return 53;
	case SBC_SAMPLING_FREQ_48000:
		switch (mode) {
		case SBC_CHANNEL_MODE_MONO:
		case SBC_CHANNEL_MODE_DUAL_CHANNEL:
			return 29;

		case SBC_CHANNEL_MODE_STEREO:
		case SBC_CHANNEL_MODE_JOINT_STEREO:
			return 51;
		}

		spa_log_warn(monitor->log, "Invalid channel mode %u", mode);
		return 51;
	}
	spa_log_warn(monitor->log, "Invalid sampling freq %u", freq);
	return 53;
}

static int select_configuration_sbc(struct spa_bt_monitor *monitor, void *capabilities, int size, void *config)
{
	a2dp_sbc_t *cap, conf;
	int bitpool;

	if (size < sizeof(conf)) {
		spa_log_error(monitor->log, "Capabilities array has invalid size");
		return -ENOSPC;
	}
	cap = capabilities;
	conf = *cap;

	if (conf.frequency & SBC_SAMPLING_FREQ_48000)
		conf.frequency = SBC_SAMPLING_FREQ_48000;
	else if (conf.frequency & SBC_SAMPLING_FREQ_44100)
		conf.frequency = SBC_SAMPLING_FREQ_44100;
	else if (conf.frequency & SBC_SAMPLING_FREQ_32000)
		conf.frequency = SBC_SAMPLING_FREQ_32000;
	else if (conf.frequency & SBC_SAMPLING_FREQ_16000)
		conf.frequency = SBC_SAMPLING_FREQ_16000;
	else {
		spa_log_error(monitor->log, "No supported sampling frequencies: 0x%x", conf.frequency);
		return -ENOTSUP;
	}

	if (conf.channel_mode & SBC_CHANNEL_MODE_JOINT_STEREO)
		conf.channel_mode = SBC_CHANNEL_MODE_JOINT_STEREO;
	else if (conf.channel_mode & SBC_CHANNEL_MODE_STEREO)
		conf.channel_mode = SBC_CHANNEL_MODE_STEREO;
	else if (conf.channel_mode & SBC_CHANNEL_MODE_DUAL_CHANNEL)
		conf.channel_mode = SBC_CHANNEL_MODE_DUAL_CHANNEL;
	else if (conf.channel_mode & SBC_CHANNEL_MODE_MONO)
		conf.channel_mode = SBC_CHANNEL_MODE_MONO;
	else {
		spa_log_error(monitor->log, "No supported channel modes: 0x%x", conf.channel_mode);
		return -ENOTSUP;
	}

	if (conf.block_length & SBC_BLOCK_LENGTH_16)
		conf.block_length = SBC_BLOCK_LENGTH_16;
	else if (conf.block_length & SBC_BLOCK_LENGTH_12)
		conf.block_length = SBC_BLOCK_LENGTH_12;
	else if (conf.block_length & SBC_BLOCK_LENGTH_8)
		conf.block_length = SBC_BLOCK_LENGTH_8;
	else if (conf.block_length & SBC_BLOCK_LENGTH_4)
		conf.block_length = SBC_BLOCK_LENGTH_4;
	else {
		spa_log_error(monitor->log, "No supported block lengths: 0x%x", conf.block_length);
		return -ENOTSUP;
	}

	if (conf.subbands & SBC_SUBBANDS_8)
		conf.subbands = SBC_SUBBANDS_8;
	else if (conf.subbands & SBC_SUBBANDS_4)
		conf.subbands = SBC_SUBBANDS_4;
	else {
		spa_log_error(monitor->log, "No supported subbands: 0x%x", conf.subbands);
		return -ENOTSUP;
	}

	if (conf.allocation_method & SBC_ALLOCATION_LOUDNESS)
		conf.allocation_method = SBC_ALLOCATION_LOUDNESS;
	else if (conf.allocation_method & SBC_ALLOCATION_SNR)
		conf.allocation_method = SBC_ALLOCATION_SNR;
	else {
		spa_log_error(monitor->log, "No supported allocation: 0x%x", conf.allocation_method);
		return -ENOTSUP;
	}

	bitpool = a2dp_default_bitpool(monitor, conf.frequency, conf.channel_mode);

	conf.min_bitpool = SPA_MAX(MIN_BITPOOL, conf.min_bitpool);
	conf.max_bitpool = SPA_MIN(bitpool, conf.max_bitpool);
	memcpy(config, &conf, size);

	spa_log_debug(monitor->log, "SelectConfiguration(): %d %d %d %d ",
			conf.frequency, conf.channel_mode, conf.min_bitpool, conf.max_bitpool);

	return 0;
}

static DBusHandlerResult endpoint_select_configuration(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	a2dp_sbc_t *cap, config;
	uint8_t *pconf = (uint8_t *) &config;
	DBusMessage *r;
	DBusError err;
	int size;

	dbus_error_init(&err);

	if (!dbus_message_get_args(m, &err, DBUS_TYPE_ARRAY,
				DBUS_TYPE_BYTE, &cap, &size, DBUS_TYPE_INVALID)) {
		spa_log_error(monitor->log, "Endpoint SelectConfiguration(): %s", err.message);
		dbus_error_free(&err);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (select_configuration_sbc(monitor, cap, size, &config) < 0) {
		if ((r = dbus_message_new_error(m, "org.bluez.Error.InvalidArguments",
				"Unable to select configuration")) == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		goto exit_send;
	}

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_message_append_args(r, DBUS_TYPE_ARRAY,
			DBUS_TYPE_BYTE, &pconf, size, DBUS_TYPE_INVALID))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

      exit_send:
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static struct spa_bt_adapter *adapter_find(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_adapter *d;
	spa_list_for_each(d, &monitor->adapter_list, link)
		if (strcmp(d->path, path) == 0)
			return d;
	return NULL;
}

static int adapter_update_props(struct spa_bt_adapter *adapter,
				DBusMessageIter *props_iter,
				DBusMessageIter *invalidated_iter)
{
	struct spa_bt_monitor *monitor = adapter->monitor;

	while (dbus_message_iter_get_arg_type(props_iter) != DBUS_TYPE_INVALID) {
		DBusMessageIter it[2];
		const char *key;
		int type;

		dbus_message_iter_recurse(props_iter, &it[0]);
		dbus_message_iter_get_basic(&it[0], &key);
		dbus_message_iter_next(&it[0]);
		dbus_message_iter_recurse(&it[0], &it[1]);

		type = dbus_message_iter_get_arg_type(&it[1]);

		if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
			const char *value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "adapter %p: %s=%s", adapter, key, value);

			if (strcmp(key, "Alias") == 0) {
				free(adapter->alias);
				adapter->alias = strdup(value);
			}
			else if (strcmp(key, "Name") == 0) {
				free(adapter->name);
				adapter->name = strdup(value);
			}
			else if (strcmp(key, "Address") == 0) {
				free(adapter->address);
				adapter->address = strdup(value);
			}
		}
		else if (type == DBUS_TYPE_UINT32) {
			uint32_t value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "adapter %p: %s=%d", adapter, key, value);

			if (strcmp(key, "Class") == 0)
				adapter->bluetooth_class = value;

		}
		else if (type == DBUS_TYPE_BOOLEAN) {
			int value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "adapter %p: %s=%d", adapter, key, value);

			if (strcmp(key, "Powered") == 0) {
				adapter->powered = value;
			}
		}
		else if (strcmp(key, "UUIDs") == 0) {
			DBusMessageIter iter;

			if (strcmp(dbus_message_iter_get_signature(&it[1]), "as") != 0)
				goto next;

			dbus_message_iter_recurse(&it[1], &iter);

			while (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INVALID) {
				const char *uuid;

				dbus_message_iter_get_basic(&iter, &uuid);

				spa_log_debug(monitor->log, "adapter %p: add UUID=%s", adapter, uuid);

				adapter->profiles |= spa_bt_profile_from_uuid(uuid);

				dbus_message_iter_next(&iter);
			}
		}
		else
			spa_log_debug(monitor->log, "adapter %p: unhandled key %s", adapter, key);

	      next:
		dbus_message_iter_next(props_iter);
	}
	return 0;
}

static struct spa_bt_adapter *adapter_create(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_adapter *d;

	d = calloc(1, sizeof(struct spa_bt_adapter));
	if (d == NULL)
		return NULL;

	d->monitor = monitor;
	d->path = strdup(path);

	spa_list_prepend(&monitor->adapter_list, &d->link);

	return d;
}

static struct spa_bt_device *device_find(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_device *d;
	spa_list_for_each(d, &monitor->device_list, link)
		if (strcmp(d->path, path) == 0)
			return d;
	return NULL;
}

static struct spa_bt_device *device_create(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_device *d;

	d = calloc(1, sizeof(struct spa_bt_device));
	if (d == NULL)
		return NULL;

	d->monitor = monitor;
	d->path = strdup(path);

	spa_list_prepend(&monitor->device_list, &d->link);

	return d;
}

static void device_set_connected(struct spa_bt_device *device, int connected)
{
	device->connected = connected;
}

static int device_update_props(struct spa_bt_device *device,
			       DBusMessageIter *props_iter,
			       DBusMessageIter *invalidated_iter)
{
	struct spa_bt_monitor *monitor = device->monitor;

	while (dbus_message_iter_get_arg_type(props_iter) != DBUS_TYPE_INVALID) {
		DBusMessageIter it[2];
		const char *key;
		int type;

		dbus_message_iter_recurse(props_iter, &it[0]);
		dbus_message_iter_get_basic(&it[0], &key);
		dbus_message_iter_next(&it[0]);
		dbus_message_iter_recurse(&it[0], &it[1]);

		type = dbus_message_iter_get_arg_type(&it[1]);

		if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
			const char *value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "device %p: %s=%s", device, key, value);

			if (strcmp(key, "Alias") == 0) {
				free(device->alias);
				device->alias = strdup(value);
			}
			else if (strcmp(key, "Name") == 0) {
				free(device->name);
				device->name = strdup(value);
			}
			else if (strcmp(key, "Address") == 0) {
				free(device->address);
				device->address = strdup(value);
			}
			else if (strcmp(key, "Adapter") == 0) {
				free(device->adapter_path);
				device->adapter_path = strdup(value);
			}
			else if (strcmp(key, "Icon") == 0) {
				free(device->icon);
				device->icon = strdup(value);
			}
		}
		else if (type == DBUS_TYPE_UINT32) {
			uint32_t value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "device %p: %s=%d", device, key, value);

			if (strcmp(key, "Class") == 0)
				device->bluetooth_class = value;
		}
		else if (type == DBUS_TYPE_UINT16) {
			uint16_t value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "device %p: %s=%d", device, key, value);

			if (strcmp(key, "Appearance") == 0)
				device->appearance = value;
		}
		else if (type == DBUS_TYPE_INT16) {
			int16_t value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "device %p: %s=%d", device, key, value);

			if (strcmp(key, "RSSI") == 0)
				device->RSSI = value;
		}
		else if (type == DBUS_TYPE_BOOLEAN) {
			int value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "device %p: %s=%d", device, key, value);

			if (strcmp(key, "Paired") == 0) {
				device->paired = value;
			}
			else if (strcmp(key, "Trusted") == 0) {
				device->trusted = value;
			}
			else if (strcmp(key, "Connected") == 0) {
				device_set_connected(device, value);
			}
			else if (strcmp(key, "Blocked") == 0) {
				device->blocked = value;
			}
		}
		else if (strcmp(key, "UUIDs") == 0) {
			DBusMessageIter iter;

			if (strcmp(dbus_message_iter_get_signature(&it[1]), "as") != 0)
				goto next;

			dbus_message_iter_recurse(&it[1], &iter);

			while (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INVALID) {
				const char *uuid;

				dbus_message_iter_get_basic(&iter, &uuid);

				spa_log_debug(monitor->log, "device %p: add UUID=%s", device, uuid);

				device->profiles |= spa_bt_profile_from_uuid(uuid);

				dbus_message_iter_next(&iter);
			}
		}
		else
			spa_log_debug(monitor->log, "device %p: unhandled key %s type %d", device, key, type);

	      next:
		dbus_message_iter_next(props_iter);
	}
	return 0;
}

static struct spa_bt_transport *transport_find(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_transport *t;
	spa_list_for_each(t, &monitor->transport_list, link)
		if (strcmp(t->path, path) == 0)
			return t;
	return NULL;
}

static struct spa_bt_transport *transport_create(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_transport *t;

	t = calloc(1, sizeof(struct spa_bt_transport));
	if (t == NULL)
		return NULL;

	t->monitor = monitor;
	t->path = strdup(path);
	t->fd = -1;

	spa_list_prepend(&monitor->transport_list, &t->link);

	return t;
}

static void transport_free(struct spa_bt_transport *transport)
{
	spa_list_remove(&transport->link);
	free(transport->path);
	free(transport);
}

static int transport_update_props(struct spa_bt_transport *transport,
				  DBusMessageIter *props_iter,
				  DBusMessageIter *invalidated_iter)
{
	struct spa_bt_monitor *monitor = transport->monitor;

	while (dbus_message_iter_get_arg_type(props_iter) != DBUS_TYPE_INVALID) {
		DBusMessageIter it[2];
		const char *key;
		int type;

		dbus_message_iter_recurse(props_iter, &it[0]);
		dbus_message_iter_get_basic(&it[0], &key);
		dbus_message_iter_next(&it[0]);
		dbus_message_iter_recurse(&it[0], &it[1]);

		type = dbus_message_iter_get_arg_type(&it[1]);

		if (type == DBUS_TYPE_STRING || type == DBUS_TYPE_OBJECT_PATH) {
			const char *value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "transport %p: %s=%s", transport, key, value);

			if (strcmp(key, "UUID") == 0) {
				transport->profile = spa_bt_profile_from_uuid(value);
			}
			else if (strcmp(key, "State") == 0) {
				transport->state = spa_bt_transport_state_from_string(value);
			}
			else if (strcmp(key, "Device") == 0) {
				transport->device = device_find(monitor, value);
				if (transport->device == NULL)
					spa_log_warn(monitor->log, "could not find device %s", value);
			}
		}
		else if (strcmp(key, "Codec") == 0) {
			int8_t value;

			if (type != DBUS_TYPE_BYTE)
				goto next;
			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "transport %p: %s=%02x", transport, key, value);

			transport->codec = value;
		}
		else if (strcmp(key, "Configuration") == 0) {
			DBusMessageIter iter;
			char *value;
			int len;

			if (strcmp(dbus_message_iter_get_signature(&it[1]), "ay") != 0)
				goto next;

			dbus_message_iter_recurse(&it[1], &iter);
			dbus_message_iter_get_fixed_array(&iter, &value, &len);

			spa_log_debug(monitor->log, "transport %p: %s=%d", transport, key, len);

			free(transport->configuration);
			transport->configuration_len = 0;

			transport->configuration = malloc(len);
			if (transport->configuration) {
				memcpy(transport->configuration, value, len);
				transport->configuration_len = len;
			}
		}
		else if (strcmp(key, "Volume") == 0) {
		}
	      next:
		dbus_message_iter_next(props_iter);
	}
	return 0;
}

static struct spa_bt_node *node_create(struct spa_bt_monitor *monitor, struct spa_bt_transport *transport)
{
	struct spa_event *event;
	struct spa_pod_builder b = { NULL, };
	uint8_t buffer[4096];
	struct spa_pod *item;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	event = spa_pod_builder_object(&b, 0, monitor->type.monitor.Added);
	fill_item(monitor, transport, &item, &b);

	monitor->callbacks->event(monitor->callbacks_data, event);

	return NULL;
}

static int transport_acquire(struct spa_bt_transport *transport, bool optional)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	DBusMessage *m, *r;
	DBusError err;
	int ret = 0;
	const char *method = optional ? "TryAcquire" : "Acquire";

	if (transport->fd >= 0)
		return 0;

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
					 transport->path,
					 BLUEZ_MEDIA_TRANSPORT_INTERFACE,
					 method);
	if (m == NULL)
		return -ENOMEM;

	dbus_error_init(&err);

	r = dbus_connection_send_with_reply_and_block(monitor->conn, m, -1, &err);
	dbus_message_unref(m);
	m = NULL;

	if (r == NULL) {
		if (optional && strcmp(err.name, "org.bluez.Error.NotAvailable") == 0) {
			spa_log_info(monitor->log, "Failed optional acquire of unavailable transport %s",
					transport->path);
		}
		else {
			spa_log_error(monitor->log, "Transport %s() failed for transport %s (%s)",
					method, transport->path, err.message);
		}

		dbus_error_free(&err);
		return -EIO;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(monitor->log, "%s returned error: %s", method, dbus_message_get_error_name(r));
		ret = -EIO;
		goto finish;
	}

	if (!dbus_message_get_args(r, &err,
				   DBUS_TYPE_UNIX_FD, &transport->fd,
				   DBUS_TYPE_UINT16, &transport->read_mtu,
				   DBUS_TYPE_UINT16, &transport->write_mtu,
				   DBUS_TYPE_INVALID)) {
		spa_log_error(monitor->log, "Failed to parse %s() reply: %s", method, err.message);
		dbus_error_free(&err);
		ret = -EIO;
		goto finish;
	}
	spa_log_debug(monitor->log, "transport %p: %s, fd %d MTU %d:%d", transport, method,
			transport->fd, transport->read_mtu, transport->write_mtu);

finish:
	dbus_message_unref(r);
	return ret;
}

static int transport_release(struct spa_bt_transport *transport)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	DBusMessage *m, *r;
	DBusError err;

	if (transport->fd < 0)
		return 0;

	spa_log_debug(monitor->log, "transport %p: release", transport);

	close(transport->fd);
	transport->fd = -1;

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
					 transport->path,
					 BLUEZ_MEDIA_TRANSPORT_INTERFACE,
					 "Release");
	if (m == NULL)
		return -ENOMEM;

	dbus_error_init(&err);

	r = dbus_connection_send_with_reply_and_block(monitor->conn, m, -1, &err);
	dbus_message_unref(m);
	m = NULL;

	if (r != NULL)
		dbus_message_unref(r);

	if (dbus_error_is_set(&err)) {
		spa_log_error(monitor->log, "Failed to release transport %s: %s",
				transport->path, err.message);
		dbus_error_free(&err);
	}
	else
		spa_log_info(monitor->log, "Transport %s released", transport->path);

	return 0;
}

static DBusHandlerResult endpoint_set_configuration(DBusConnection *conn,
		const char *path, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	const char *transport_path;
	DBusMessageIter it[2];
	DBusMessage *r;
	struct spa_bt_transport *transport;
	bool is_new = false;

	if (!dbus_message_has_signature(m, "oa{sv}")) {
		spa_log_warn(monitor->log, "invalid SetConfiguration() signature");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_message_iter_init(m, &it[0]);
	dbus_message_iter_get_basic(&it[0], &transport_path);
	dbus_message_iter_next(&it[0]);
	dbus_message_iter_recurse(&it[0], &it[1]);

	transport = transport_find(monitor, transport_path);
	is_new = transport == NULL;

	if (is_new) {
		transport = transport_create(monitor, transport_path);
		if (transport == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		transport->acquire = transport_acquire;
		transport->release = transport_release;
	}
	transport_update_props(transport, &it[1], NULL);

	if (is_new)
		node_create(monitor, transport);

	if (transport->device == NULL) {
		spa_log_warn(monitor->log, "no device found for transport");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult endpoint_clear_configuration(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	DBusMessage *r;

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult endpoint_release(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	DBusMessage *r;

	r = dbus_message_new_error(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE ".Error.NotImplemented",
                                            "Method not implemented");
	if (r == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult endpoint_handler(DBusConnection *c, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	const char *path, *interface, *member;
	DBusMessage *r;
	DBusHandlerResult res;

	path = dbus_message_get_path(m);
	interface = dbus_message_get_interface(m);
	member = dbus_message_get_member(m);

	spa_log_debug(monitor->log, "dbus: path=%s, interface=%s, member=%s", path, interface, member);

	if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
		const char *xml = ENDPOINT_INTROSPECT_XML;

		if ((r = dbus_message_new_method_return(m)) == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_connection_send(monitor->conn, r, NULL))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		dbus_message_unref(r);
		res = DBUS_HANDLER_RESULT_HANDLED;
	}
	else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "SetConfiguration"))
		res = endpoint_set_configuration(c, path, m, userdata);
	else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "SelectConfiguration"))
		res = endpoint_select_configuration(c, m, userdata);
	else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "ClearConfiguration"))
		res = endpoint_clear_configuration(c, m, userdata);
	else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "Release"))
		res = endpoint_release(c, m, userdata);
	else
		res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	return res;
}

static void register_endpoint_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_monitor *monitor = user_data;
	DBusMessage *r;

	r = dbus_pending_call_steal_reply(pending);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(monitor->log, "BlueZ D-Bus ObjectManager not available");
		goto finish;
	}
	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(monitor->log, "RegisterEndpoint() failed: %s",
				dbus_message_get_error_name(r));
		goto finish;
	}

      finish:
	dbus_message_unref(r);
        dbus_pending_call_unref(pending);
}

static int register_a2dp_endpoint(struct spa_bt_monitor *monitor,
				  const char *path,
				  const char *uuid,
				  enum spa_bt_profile profile,
				  uint16_t codec,
				  const void *configuration,
				  size_t configuration_size)
{
	const char *profile_path;
	char *object_path, *str;
	const DBusObjectPathVTable vtable_endpoint = {
		.message_function = endpoint_handler,
	};
	DBusMessage *m;
	DBusMessageIter it[5];
	DBusPendingCall *call;

	switch (profile) {
	case SPA_BT_PROFILE_A2DP_SOURCE:
		switch (codec) {
		case A2DP_CODEC_SBC:
			profile_path = "/A2DP/SBC/Source";
			break;
		default:
			return -ENOTSUP;
		}
		break;
	default:
		return -ENOTSUP;
	}

	asprintf(&object_path, "%s/%d", profile_path, monitor->count++);

	spa_log_debug(monitor->log, "Registering endpoint: %s", object_path);

	if (!dbus_connection_register_object_path(monitor->conn,
						  object_path,
						  &vtable_endpoint, monitor))
		return -EIO;

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
					 path,
					 BLUEZ_MEDIA_INTERFACE,
					 "RegisterEndpoint");
	if (m == NULL)
		return -EIO;

	dbus_message_iter_init_append(m, &it[0]);
	dbus_message_iter_append_basic(&it[0], DBUS_TYPE_OBJECT_PATH, &object_path);

	dbus_message_iter_open_container(&it[0], DBUS_TYPE_ARRAY, "{sv}", &it[1]);

	dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
	str = "UUID";
	dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
	dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "s", &it[3]);
	dbus_message_iter_append_basic(&it[3], DBUS_TYPE_STRING, &uuid);
	dbus_message_iter_close_container(&it[2], &it[3]);
	dbus_message_iter_close_container(&it[1], &it[2]);

	dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
	str = "Codec";
	dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
	dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "y", &it[3]);
	dbus_message_iter_append_basic(&it[3], DBUS_TYPE_BYTE, &codec);
	dbus_message_iter_close_container(&it[2], &it[3]);
	dbus_message_iter_close_container(&it[1], &it[2]);

	dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
	str = "Capabilities";
	dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
	dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "ay", &it[3]);
	dbus_message_iter_open_container(&it[3], DBUS_TYPE_ARRAY, "y", &it[4]);
	dbus_message_iter_append_fixed_array (&it[4], DBUS_TYPE_BYTE,
			&configuration, configuration_size);
	dbus_message_iter_close_container(&it[3], &it[4]);
	dbus_message_iter_close_container(&it[2], &it[3]);
	dbus_message_iter_close_container(&it[1], &it[2]);
	dbus_message_iter_close_container(&it[0], &it[1]);

	dbus_connection_send_with_reply(monitor->conn, m, &call, -1);
	dbus_pending_call_set_notify(call, register_endpoint_reply, monitor, NULL);
        dbus_message_unref(m);

	return 0;
}

static int adapter_register_endpoints(struct spa_bt_adapter *a)
{
	struct spa_bt_monitor *monitor = a->monitor;

	register_a2dp_endpoint(monitor, a->path,
			       SPA_BT_UUID_A2DP_SOURCE,
			       SPA_BT_PROFILE_A2DP_SOURCE,
			       A2DP_CODEC_SBC,
			       &bluez_a2dp_sbc, sizeof(bluez_a2dp_sbc));
	return 0;
}

static void interface_added(struct spa_bt_monitor *monitor,
			    DBusConnection *conn,
			    const char *object_path,
			    const char *interface_name,
			    DBusMessageIter *props_iter)
{
	if (strcmp(interface_name, BLUEZ_ADAPTER_INTERFACE) == 0) {
		struct spa_bt_adapter *a;

		a = adapter_find(monitor, object_path);
		if (a == NULL) {
			a = adapter_create(monitor, object_path);
			if (a == NULL) {
				spa_log_warn(monitor->log, "can't create adapter");
				return;
			}
		}
		adapter_update_props(a, props_iter, NULL);
		adapter_register_endpoints(a);
	}
	else if (strcmp(interface_name, BLUEZ_DEVICE_INTERFACE) == 0) {
		struct spa_bt_device *d;

		d = device_find(monitor, object_path);
		if (d == NULL) {
			d = device_create(monitor, object_path);
			if (d == NULL) {
				spa_log_warn(monitor->log, "can't create device");
				return;
			}
		}
		device_update_props(d, props_iter, NULL);
	}
	else
		spa_log_debug(monitor->log, "Unknown interface %s found, skipping", interface_name);
}

static void get_managed_objects_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_monitor *monitor = user_data;
	DBusMessage *r;
	DBusMessageIter it[6];

	r = dbus_pending_call_steal_reply(pending);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(monitor->log, "BlueZ D-Bus ObjectManager not available");
		goto finish;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(monitor->log, "GetManagedObjects() failed: %s",
				dbus_message_get_error_name(r));
		goto finish;
	}

	if (!dbus_message_iter_init(r, &it[0]) ||
	    strcmp(dbus_message_get_signature(r), "a{oa{sa{sv}}}") != 0) {
		spa_log_error(monitor->log, "Invalid reply signature for GetManagedObjects()");
		goto finish;
	}

	dbus_message_iter_recurse(&it[0], &it[1]);

	while (dbus_message_iter_get_arg_type(&it[1]) != DBUS_TYPE_INVALID) {
		const char *object_path;

		dbus_message_iter_recurse(&it[1], &it[2]);
		dbus_message_iter_get_basic(&it[2], &object_path);
		dbus_message_iter_next(&it[2]);
		dbus_message_iter_recurse(&it[2], &it[3]);

		while (dbus_message_iter_get_arg_type(&it[3]) != DBUS_TYPE_INVALID) {
			const char *interface_name;

			dbus_message_iter_recurse(&it[3], &it[4]);
			dbus_message_iter_get_basic(&it[4], &interface_name);
			dbus_message_iter_next(&it[4]);
			dbus_message_iter_recurse(&it[4], &it[5]);

			interface_added(monitor, monitor->conn,
					object_path, interface_name,
					&it[5]);

			dbus_message_iter_next(&it[3]);
		}
		dbus_message_iter_next(&it[1]);
	}

      finish:
	dbus_message_unref(r);
        dbus_pending_call_unref(pending);
	return;
}

static void get_managed_objects(struct spa_bt_monitor *monitor)
{
	DBusMessage *m;
	DBusPendingCall *call;

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
					 "/",
					 "org.freedesktop.DBus.ObjectManager",
					 "GetManagedObjects");

	dbus_connection_send_with_reply(monitor->conn, m, &call, -1);
	dbus_pending_call_set_notify(call, get_managed_objects_reply, monitor, NULL);
        dbus_message_unref(m);
}

static int
impl_monitor_set_callbacks(struct spa_monitor *monitor,
			   const struct spa_monitor_callbacks *callbacks,
			   void *data)
{
	struct spa_bt_monitor *this;

	spa_return_val_if_fail(monitor != NULL, -EINVAL);

	this = SPA_CONTAINER_OF(monitor, struct spa_bt_monitor, monitor);

	this->callbacks = callbacks;
	this->callbacks_data = data;

	get_managed_objects(this);

	return 0;
}

static int
impl_monitor_enum_items(struct spa_monitor *monitor, uint32_t *index,
			struct spa_pod **item, struct spa_pod_builder *builder)
{
	spa_return_val_if_fail(monitor != NULL, -EINVAL);
	spa_return_val_if_fail(item != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);
	return 0;
}

static const struct spa_monitor impl_monitor = {
	SPA_VERSION_MONITOR,
	NULL,
	impl_monitor_set_callbacks,
	impl_monitor_enum_items,
};

static int impl_get_interface(struct spa_handle *handle, uint32_t interface_id, void **interface)
{
	struct spa_bt_monitor *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct spa_bt_monitor *) handle;

	if (interface_id == this->type.monitor.Monitor)
		*interface = &this->monitor;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	return 0;
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct spa_bt_monitor *this;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct spa_bt_monitor *) handle;

	for (i = 0; i < n_support; i++) {
		if (strcmp(support[i].type, SPA_TYPE__TypeMap) == 0)
			this->map = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE__Log) == 0)
			this->log = support[i].data;
		else if (strcmp(support[i].type, SPA_TYPE__DBus) == 0)
			this->dbus = support[i].data;
	}
	if (this->map == NULL) {
		spa_log_error(this->log, "a type-map is needed");
		return -EINVAL;
	}
	if (this->dbus == NULL) {
		spa_log_error(this->log, "a dbus is needed");
		return -EINVAL;
	}
	init_type(&this->type, this->map);

	this->dbus_connection = spa_dbus_get_connection(this->dbus, DBUS_BUS_SYSTEM);
	if (this->dbus_connection == NULL) {
		spa_log_error(this->log, "no dbus connection");
		return -EIO;
	}
	this->conn = spa_dbus_connection_get(this->dbus_connection);

	this->monitor = impl_monitor;

	spa_list_init(&this->adapter_list);
	spa_list_init(&this->device_list);
	spa_list_init(&this->transport_list);

	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{SPA_TYPE__Monitor,},
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

static const struct spa_handle_factory spa_bluez5_monitor_factory = {
	SPA_VERSION_MONITOR,
	NAME,
	NULL,
	sizeof(struct spa_bt_monitor),
	impl_init,
	impl_enum_interface_info,
};

int spa_handle_factory_register(const struct spa_handle_factory *factory);

static void reg(void) __attribute__ ((constructor));
static void reg(void)
{
	spa_handle_factory_register(&spa_bluez5_monitor_factory);
}
