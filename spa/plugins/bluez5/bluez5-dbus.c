/* Spa V4l2 dbus
 *
 * Copyright © 2018 Wim Taymans
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
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/sco.h>

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

#include "a2dp-codecs.h"
#include "defs.h"

#define NAME "bluez5-monitor"

struct spa_bt_monitor {
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_system *main_system;
	struct spa_dbus *dbus;
	struct spa_dbus_connection *dbus_connection;
	DBusConnection *conn;

	struct spa_hook_list hooks;

	uint32_t count;
	uint32_t id;

	struct spa_list adapter_list;
	struct spa_list device_list;
	struct spa_list transport_list;

	unsigned int filters_added:1;
};

struct transport_data {
	struct spa_source rfcomm;
	struct spa_source sco;
};

static inline void add_dict(struct spa_pod_builder *builder, const char *key, const char *val)
{
	spa_pod_builder_string(builder, key);
	spa_pod_builder_string(builder, val);
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

static int select_configuration_sbc(struct spa_bt_monitor *monitor, void *capabilities, size_t size, void *config)
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

static int select_configuration_aac(struct spa_bt_monitor *monitor, void *capabilities, size_t size, void *config)
{
	a2dp_aac_t *cap, conf;
	int freq;

	if (size < sizeof(conf)) {
		spa_log_error(monitor->log, "Capabilities array has invalid size");
		return -ENOSPC;
	}
	cap = capabilities;
	conf = *cap;

	if (conf.object_type & AAC_OBJECT_TYPE_MPEG2_AAC_LC)
		conf.object_type = AAC_OBJECT_TYPE_MPEG2_AAC_LC;
	else if (conf.object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LC)
		conf.object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LC;
	else if (conf.object_type & AAC_OBJECT_TYPE_MPEG4_AAC_LTP)
		conf.object_type = AAC_OBJECT_TYPE_MPEG4_AAC_LTP;
	else if (conf.object_type & AAC_OBJECT_TYPE_MPEG4_AAC_SCA)
		conf.object_type = AAC_OBJECT_TYPE_MPEG4_AAC_SCA;
	else {
		spa_log_error(monitor->log, "No supported object type: 0x%x", conf.object_type);
		return -ENOTSUP;
	}

	freq = AAC_GET_FREQUENCY(conf);
	if (freq & AAC_SAMPLING_FREQ_48000)
		freq = AAC_SAMPLING_FREQ_48000;
	else if (freq & AAC_SAMPLING_FREQ_44100)
		freq = AAC_SAMPLING_FREQ_44100;
	else if (freq & AAC_SAMPLING_FREQ_64000)
		freq = AAC_SAMPLING_FREQ_64000;
	else if (freq & AAC_SAMPLING_FREQ_32000)
		freq = AAC_SAMPLING_FREQ_32000;
	else if (freq & AAC_SAMPLING_FREQ_88200)
		freq = AAC_SAMPLING_FREQ_88200;
	else if (freq & AAC_SAMPLING_FREQ_96000)
		freq = AAC_SAMPLING_FREQ_96000;
	else if (freq & AAC_SAMPLING_FREQ_24000)
		freq = AAC_SAMPLING_FREQ_24000;
	else if (freq & AAC_SAMPLING_FREQ_22050)
		freq = AAC_SAMPLING_FREQ_22050;
	else if (freq & AAC_SAMPLING_FREQ_16000)
		freq = AAC_SAMPLING_FREQ_16000;
	else if (freq & AAC_SAMPLING_FREQ_12000)
		freq = AAC_SAMPLING_FREQ_12000;
	else if (freq & AAC_SAMPLING_FREQ_11025)
		freq = AAC_SAMPLING_FREQ_11025;
	else if (freq & AAC_SAMPLING_FREQ_8000)
		freq = AAC_SAMPLING_FREQ_8000;
	else {
		spa_log_error(monitor->log, "No supported sampling frequency: 0x%0x", freq);
		return -ENOTSUP;
	}
	AAC_SET_FREQUENCY(conf, freq);

	if (conf.channels & AAC_CHANNELS_2)
		conf.channels = AAC_CHANNELS_2;
	else if (conf.channels & AAC_CHANNELS_1)
		conf.channels = AAC_CHANNELS_1;
	else {
		spa_log_error(monitor->log, "No supported channels: 0x%0x", conf.channels);
		return -ENOTSUP;
	}
	memcpy(config, &conf, size);

	spa_log_debug(monitor->log, "SelectConfiguration() %d %d %d", conf.object_type, freq, conf.channels);

	return 0;
}

static DBusHandlerResult endpoint_select_configuration(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	const char *path;
	uint8_t *cap, config[16];
	uint8_t *pconf = (uint8_t *) config;
	DBusMessage *r;
	DBusError err;
	int size, res;

	dbus_error_init(&err);

	path = dbus_message_get_path(m);

	if (!dbus_message_get_args(m, &err, DBUS_TYPE_ARRAY,
				DBUS_TYPE_BYTE, &cap, &size, DBUS_TYPE_INVALID)) {
		spa_log_error(monitor->log, "Endpoint SelectConfiguration(): %s", err.message);
		dbus_error_free(&err);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	if (strstr(path, "/A2DP/SBC/") == path) {
		res = select_configuration_sbc(monitor, cap, size, config);
	} else if (strstr(path, "/A2DP/MPEG24/") == path) {
		res = select_configuration_aac(monitor, cap, size, config);
	} else
		res = -ENOTSUP;

	if (res < 0) {
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
				enum spa_bt_profile profile;

				dbus_message_iter_get_basic(&iter, &uuid);

				profile = spa_bt_profile_from_uuid(uuid);

				if (profile && (adapter->profiles & profile) == 0) {
					spa_log_debug(monitor->log, "adapter %p: add UUID=%s", adapter, uuid);
					adapter->profiles |= profile;
				}
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

	d->id = monitor->id++;
	d->monitor = monitor;
	d->path = strdup(path);
	spa_list_init(&d->transport_list);

	spa_list_prepend(&monitor->device_list, &d->link);

	return d;
}

#if 0
static int device_free(struct spa_bt_device *device)
{
	struct spa_bt_transport *t;
	struct spa_bt_monitor *monitor = device->monitor;

	spa_log_debug(monitor->log, "%p", device);

	spa_list_for_each(t, &device->transport_list, device_link) {
		if (t->device == device) {
			spa_list_remove(&t->device_link);
			t->device = NULL;
		}
	}
	spa_list_remove(&device->link);
	free(device->path);
	free(device);
	return 0;
}
#endif

static int device_add(struct spa_bt_monitor *monitor, struct spa_bt_device *device)
{
	struct spa_device_object_info info;
	char dev[32];
	struct spa_dict_item items[20];
        uint32_t n_items = 0;

	if (device->added)
		return 0;

	info = SPA_DEVICE_OBJECT_INFO_INIT();
	info.type = SPA_TYPE_INTERFACE_Device;
	info.factory_name = SPA_NAME_API_BLUEZ5_DEVICE;
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_FLAGS |
		SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.flags = 0;

	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_API, "bluez5");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_MEDIA_CLASS, "Audio/Device");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_NAME, device->name);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_ALIAS, device->alias);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_ICON_NAME, device->icon);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_PATH, device->path);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_ADDRESS, device->address);
	snprintf(dev, sizeof(dev), "pointer:%p", device);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_DEVICE, dev);

	info.props = &SPA_DICT_INIT(items, n_items);

	device->added = true;
        spa_device_emit_object_info(&monitor->hooks, device->id, &info);

	return 0;
}

static int device_remove(struct spa_bt_monitor *monitor, struct spa_bt_device *device)
{
	if (!device->added)
		return 0;

	device->added = false;
	spa_device_emit_object_info(&monitor->hooks, device->id, NULL);

	return 0;
}


#define DEVICE_PROFILE_TIMEOUT_SEC 3

static void device_timer_event(struct spa_source *source)
{
	struct spa_bt_device *device = source->data;
	struct spa_bt_monitor *monitor = device->monitor;
	uint64_t exp;

	if (spa_system_timerfd_read(monitor->main_system, source->fd, &exp) < 0)
                spa_log_warn(monitor->log, "error reading timerfd: %s", strerror(errno));

	spa_log_debug(monitor->log, "device %p: timeout %08x %08x",
			device, device->profiles, device->connected_profiles);

	device_add(device->monitor, device);
}

static int device_start_timer(struct spa_bt_device *device)
{
	struct spa_bt_monitor *monitor = device->monitor;
	struct itimerspec ts;

	spa_log_debug(monitor->log, "device %p: start timer", device);
	if (device->timer.data == NULL) {
		device->timer.data = device;
		device->timer.func = device_timer_event;
		device->timer.fd = spa_system_timerfd_create(monitor->main_system,
				CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
		device->timer.mask = SPA_IO_IN;
		device->timer.rmask = 0;
		spa_loop_add_source(monitor->main_loop, &device->timer);
	}
	ts.it_value.tv_sec = DEVICE_PROFILE_TIMEOUT_SEC;
	ts.it_value.tv_nsec = 0;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(monitor->main_system, device->timer.fd, 0, &ts, NULL);
	return 0;
}

static int device_stop_timer(struct spa_bt_device *device)
{
	struct spa_bt_monitor *monitor = device->monitor;
	struct itimerspec ts;

	if (device->timer.data == NULL)
		return 0;

	spa_log_debug(monitor->log, "device %p: stop timer", device);
	spa_loop_remove_source(monitor->main_loop, &device->timer);
        ts.it_value.tv_sec = 0;
        ts.it_value.tv_nsec = 0;
        ts.it_interval.tv_sec = 0;
        ts.it_interval.tv_nsec = 0;
        spa_system_timerfd_settime(monitor->main_system, device->timer.fd, 0, &ts, NULL);
	spa_system_close(monitor->main_system, device->timer.fd);
	device->timer.data = NULL;
	return 0;
}

static int device_check_profiles(struct spa_bt_device *device, bool force)
{
	struct spa_bt_monitor *monitor = device->monitor;
	uint32_t connected_profiles = device->connected_profiles;

	if (connected_profiles & SPA_BT_PROFILE_HEADSET_HEAD_UNIT)
		connected_profiles |= SPA_BT_PROFILE_HEADSET_HEAD_UNIT;
	if (connected_profiles & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY)
		connected_profiles |= SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY;

	spa_log_debug(monitor->log, "device %p: profiles %08x %08x %d",
			device, device->profiles, connected_profiles, device->added);

	if (connected_profiles == 0) {
		if (device->added) {
			device_stop_timer(device);
			device_remove(monitor, device);
		}
	}
	else if (force || (device->profiles & connected_profiles) == device->profiles) {
		device_stop_timer(device);
		device_add(monitor, device);
	} else {
		device_start_timer(device);
	}
	return 0;
}

static void device_set_connected(struct spa_bt_device *device, int connected)
{
	if (device->connected && !connected)
		device->connected_profiles = 0;

	device->connected = connected;

	if (connected)
		device_check_profiles(device, false);
	else
		device_stop_timer(device);
}

static int device_connect_profile(struct spa_bt_device *device, enum spa_bt_profile profile)
{
	device->connected_profiles |= profile;
	device_check_profiles(device, false);
	return 0;
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

				device->adapter = adapter_find(monitor, value);
				if (device->adapter == NULL) {
					spa_log_warn(monitor->log, "unknown adapter %s", value);
				}
			}
			else if (strcmp(key, "Icon") == 0) {
				free(device->icon);
				device->icon = strdup(value);
			}
		}
		else if (type == DBUS_TYPE_UINT32) {
			uint32_t value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "device %p: %s=%08x", device, key, value);

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
			else if (strcmp(key, "ServicesResolved") == 0) {
				if (value)
					device_check_profiles(device, true);
			}
		}
		else if (strcmp(key, "UUIDs") == 0) {
			DBusMessageIter iter;

			if (strcmp(dbus_message_iter_get_signature(&it[1]), "as") != 0)
				goto next;

			dbus_message_iter_recurse(&it[1], &iter);

			while (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INVALID) {
				const char *uuid;
				enum spa_bt_profile profile;

				dbus_message_iter_get_basic(&iter, &uuid);

				profile = spa_bt_profile_from_uuid(uuid);
				if (profile && (device->profiles & profile) == 0) {
					spa_log_debug(monitor->log, "device %p: add UUID=%s", device, uuid);
					device->profiles |= profile;
				}
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

static struct spa_bt_transport *transport_create(struct spa_bt_monitor *monitor, char *path, size_t extra)
{
	struct spa_bt_transport *t;

	t = calloc(1, sizeof(struct spa_bt_transport) + extra);
	if (t == NULL)
		return NULL;

	t->monitor = monitor;
	t->path = path;
	t->fd = -1;
	t->user_data = SPA_MEMBER(t, sizeof(struct spa_bt_transport), void);
	spa_hook_list_init(&t->listener_list);

	spa_list_append(&monitor->transport_list, &t->link);

	return t;
}
static void transport_set_state(struct spa_bt_transport *transport, enum spa_bt_transport_state state)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	enum spa_bt_transport_state old = transport->state;

	if (old != state) {
		transport->state = state;
		spa_log_debug(monitor->log, "transport %p: %s state changed %d -> %d",
				transport, transport->path, old, state);
		spa_bt_transport_emit_state_changed(transport, old, state);
	}
}

static void transport_free(struct spa_bt_transport *transport)
{
	struct spa_bt_monitor *monitor = transport->monitor;

	spa_log_debug(monitor->log, "transport %p: free %s", transport, transport->path);

	transport_set_state(transport, SPA_BT_TRANSPORT_STATE_IDLE);

	spa_bt_transport_emit_destroy(transport);

	spa_bt_transport_destroy(transport);

	spa_list_remove(&transport->link);
	if (transport->device) {
		transport->device->connected_profiles &= ~transport->profile;
		spa_list_remove(&transport->device_link);
	}
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
				switch (spa_bt_profile_from_uuid(value)) {
				case SPA_BT_PROFILE_A2DP_SOURCE:
					transport->profile = SPA_BT_PROFILE_A2DP_SINK;
					break;
				case SPA_BT_PROFILE_A2DP_SINK:
					transport->profile = SPA_BT_PROFILE_A2DP_SOURCE;
					break;
				default:
					spa_log_warn(monitor->log, "unknown profile %s", value);
					break;
				}
			}
			else if (strcmp(key, "State") == 0) {
				transport_set_state(transport, spa_bt_transport_state_from_string(value));
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

static int transport_acquire(void *data, bool optional)
{
	struct spa_bt_transport *transport = data;
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
	spa_log_debug(monitor->log, "transport %p: %s %s, fd %d MTU %d:%d", transport, method,
			transport->path, transport->fd, transport->read_mtu, transport->write_mtu);

finish:
	dbus_message_unref(r);
	return ret;
}

static int transport_release(void *data)
{
	struct spa_bt_transport *transport = data;
	struct spa_bt_monitor *monitor = transport->monitor;
	DBusMessage *m, *r;
	DBusError err;

	if (transport->fd < 0)
		return 0;

	spa_log_debug(monitor->log, "transport %p: Release %s",
			transport, transport->path);

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

static const struct spa_bt_transport_implementation transport_impl = {
	SPA_VERSION_BT_TRANSPORT_IMPLEMENTATION,
	.acquire = transport_acquire,
	.release = transport_release,
};

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
		transport = transport_create(monitor, strdup(transport_path), 0);
		if (transport == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		spa_bt_transport_set_implementation(transport, &transport_impl, transport);
	}
	transport_update_props(transport, &it[1], NULL);

	if (transport->device == NULL) {
		spa_log_warn(monitor->log, "no device found for transport");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	spa_list_append(&transport->device->transport_list, &transport->device_link);

	device_connect_profile(transport->device, transport->profile);

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult endpoint_clear_configuration(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	DBusError err;
	DBusMessage *r;
	const char *transport_path;
	struct spa_bt_transport *transport;

	dbus_error_init(&err);

	if (!dbus_message_get_args(m, &err,
				   DBUS_TYPE_OBJECT_PATH, &transport_path,
				   DBUS_TYPE_INVALID)) {
		spa_log_warn(monitor->log, "Bad ClearConfiguration method call: %s",
			err.message);
		dbus_error_free(&err);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	transport = transport_find(monitor, transport_path);

	if (transport != NULL) {
		struct spa_bt_device *device = transport->device;

		spa_log_debug(monitor->log, "transport %p: free %s",
			transport, transport->path);

		transport_free(transport);
		if (device != NULL)
			device_check_profiles(device, false);
	}

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
		case A2DP_CODEC_MPEG24:
			/* We don't support MPEG24 for now */
			return -ENOTSUP;
			/* profile_path = "/A2DP/MPEG24/Source";
			break; */
		default:
			return -ENOTSUP;
		}
		break;
	case SPA_BT_PROFILE_A2DP_SINK:
		switch (codec) {
		case A2DP_CODEC_SBC:
			profile_path = "/A2DP/SBC/Sink";
			break;

		case A2DP_CODEC_MPEG24:
			/* We don't support MPEG24 for now */
			return -ENOTSUP;
			/* profile_path = "/A2DP/MPEG24/Sink";
			break; */
		default:
			return -ENOTSUP;
		}
		break;
	default:
		return -ENOTSUP;
	}

	object_path = spa_aprintf("%s/%d", profile_path, monitor->count++);
	if (object_path == NULL)
		return -errno;

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

	/* We don't support MPEG24 for now */
	/*
	register_a2dp_endpoint(monitor, a->path,
			       SPA_BT_UUID_A2DP_SOURCE,
			       SPA_BT_PROFILE_A2DP_SOURCE,
			       A2DP_CODEC_MPEG24,
			       &bluez_a2dp_aac, sizeof(bluez_a2dp_aac));
	register_a2dp_endpoint(monitor, a->path,
			       SPA_BT_UUID_A2DP_SINK,
			       SPA_BT_PROFILE_A2DP_SINK,
			       A2DP_CODEC_MPEG24,
			       &bluez_a2dp_aac, sizeof(bluez_a2dp_aac));
	*/

	register_a2dp_endpoint(monitor, a->path,
			       SPA_BT_UUID_A2DP_SOURCE,
			       SPA_BT_PROFILE_A2DP_SOURCE,
			       A2DP_CODEC_SBC,
			       &bluez_a2dp_sbc, sizeof(bluez_a2dp_sbc));
	register_a2dp_endpoint(monitor, a->path,
			       SPA_BT_UUID_A2DP_SINK,
			       SPA_BT_PROFILE_A2DP_SINK,
			       A2DP_CODEC_SBC,
			       &bluez_a2dp_sbc, sizeof(bluez_a2dp_sbc));
	return 0;
}

static DBusHandlerResult profile_release(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	DBusMessage *r;

	r = dbus_message_new_error(m, BLUEZ_PROFILE_INTERFACE ".Error.NotImplemented",
                                            "Method not implemented");
	if (r == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static void rfcomm_event(struct spa_source *source)
{
	struct spa_bt_transport *t = source->data;
	struct spa_bt_monitor *monitor = t->monitor;

	if (source->rmask & (SPA_IO_HUP | SPA_IO_ERR)) {
		spa_log_info(monitor->log, "lost RFCOMM connection.");
		if (source->loop)
			spa_loop_remove_source(source->loop, source);
		goto fail;
	}

	if (source->rmask & SPA_IO_IN) {
		char buf[512];
		ssize_t len;
		int gain, dummy;
		bool  do_reply = false;

		len = read(source->fd, buf, 511);
		if (len < 0) {
			spa_log_error(monitor->log, "RFCOMM read error: %s", strerror(errno));
			goto fail;
		}
		buf[len] = 0;
		spa_log_debug(monitor->log, "RFCOMM << %s", buf);

		/* There are only four HSP AT commands:
		 * AT+VGS=value: value between 0 and 15, sent by the HS to AG to set the speaker gain.
		 * +VGS=value is sent by AG to HS as a response to an AT+VGS command or when the gain
		 * is changed on the AG side.
		 * AT+VGM=value: value between 0 and 15, sent by the HS to AG to set the microphone gain.
		 * +VGM=value is sent by AG to HS as a response to an AT+VGM command or when the gain
		 * is changed on the AG side.
		 * AT+CKPD=200: Sent by HS when headset button is pressed.
		 * RING: Sent by AG to HS to notify of an incoming call. It can safely be ignored because
		 * it does not expect a reply. */
		if (sscanf(buf, "AT+VGS=%d", &gain) == 1 ||
		    sscanf(buf, "\r\n+VGM=%d\r\n", &gain) == 1) {
//			t->speaker_gain = gain;
			do_reply = true;
		} else if (sscanf(buf, "AT+VGM=%d", &gain) == 1 ||
		    sscanf(buf, "\r\n+VGS=%d\r\n", &gain) == 1) {
//			t->microphone_gain = gain;
			do_reply = true;
		} else if (sscanf(buf, "AT+CKPD=%d", &dummy) == 1) {
			do_reply = true;
		} else {
			do_reply = false;
		}

		if (do_reply) {
			spa_log_debug(monitor->log, "RFCOMM >> OK");

			len = write(source->fd, "\r\nOK\r\n", 6);

			/* we ignore any errors, it's not critical and real errors should
			 * be caught with the HANGUP and ERROR events handled above */
			if (len < 0)
				spa_log_error(monitor->log, "RFCOMM write error: %s", strerror(errno));
		}
	}
	return;

fail:
	transport_free(t);
	return;
}

static int sco_do_accept(struct spa_bt_transport *t)
{
	struct transport_data *td = t->user_data;
	struct spa_bt_monitor *monitor = t->monitor;
	struct sockaddr_sco addr;
	socklen_t optlen;
	int sock;

	memset(&addr, 0, sizeof(addr));
	optlen = sizeof(addr);

	spa_log_info(monitor->log, "transport %p: doing accept", t);
	sock = accept(td->sco.fd, (struct sockaddr *) &addr, &optlen);
	if (sock < 0) {
		if (errno != EAGAIN)
			spa_log_error(monitor->log, "accept(): %s", strerror(errno));
		goto fail;
	}
	return sock;
fail:
	return -1;
}

static int sco_do_connect(struct spa_bt_transport *t)
{
	struct spa_bt_monitor *monitor = t->monitor;
	struct spa_bt_device *d = t->device;
	struct sockaddr_sco addr;
	socklen_t len;
	int err, i;
	int sock;
	bdaddr_t src;
	bdaddr_t dst;
	const char *src_addr, *dst_addr;

	if (d->adapter == NULL)
		return -EIO;

	src_addr = d->adapter->address;
	dst_addr = d->address;

	/* don't use ba2str to avoid -lbluetooth */
	for (i = 5; i >= 0; i--, src_addr += 3)
		src.b[i] = strtol(src_addr, NULL, 16);
	for (i = 5; i >= 0; i--, dst_addr += 3)
		dst.b[i] = strtol(dst_addr, NULL, 16);

	sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET, BTPROTO_SCO);
	if (sock < 0) {
		spa_log_error(monitor->log, "socket(SEQPACKET, SCO) %s", strerror(errno));
		return -errno;
	}

	len = sizeof(addr);
	memset(&addr, 0, len);
	addr.sco_family = AF_BLUETOOTH;
	bacpy(&addr.sco_bdaddr, &src);

	if (bind(sock, (struct sockaddr *) &addr, len) < 0) {
		spa_log_error(monitor->log, "bind(): %s", strerror(errno));
		goto fail_close;
	}

	memset(&addr, 0, len);
	addr.sco_family = AF_BLUETOOTH;
	bacpy(&addr.sco_bdaddr, &dst);

	spa_log_info(monitor->log, "transport %p: doing connect", t);
	err = connect(sock, (struct sockaddr *) &addr, len);
	if (err < 0 && !(errno == EAGAIN || errno == EINPROGRESS)) {
		spa_log_error(monitor->log, "connect(): %s", strerror(errno));
		goto fail_close;
	}

	return sock;

fail_close:
	close(sock);
	return -1;
}


static int sco_acquire_cb(void *data, bool optional)
{
	struct spa_bt_transport *t = data;
	struct spa_bt_monitor *monitor = t->monitor;
	int sock;
	socklen_t len;

	if (optional)
		sock = sco_do_accept(t);
	else
		sock = sco_do_connect(t);

	if (sock < 0)
		goto fail;

	t->read_mtu = 48;
	t->write_mtu = 48;

	if (true) {
		struct sco_options sco_opt;

		len = sizeof(sco_opt);
		memset(&sco_opt, 0, len);

		if (getsockopt(sock, SOL_SCO, SCO_OPTIONS, &sco_opt, &len) < 0)
			spa_log_warn(monitor->log, "getsockopt(SCO_OPTIONS) failed, loading defaults");
		else {
			spa_log_debug(monitor->log, "autodetected mtu = %u", sco_opt.mtu);
			t->read_mtu = sco_opt.mtu;
			t->write_mtu = sco_opt.mtu;
		}
	}
	return sock;
fail:
	return -1;
}

static int sco_release_cb(void *data)
{
	struct spa_bt_transport *t = data;
	struct spa_bt_monitor *monitor = t->monitor;
	spa_log_info(monitor->log, "Transport %s released", t->path);
	/* device will close the SCO socket for us */
	return 0;
}

static void sco_event(struct spa_source *source)
{
	struct spa_bt_transport *t = source->data;
	struct spa_bt_monitor *monitor = t->monitor;

	if (source->rmask & (SPA_IO_HUP | SPA_IO_ERR)) {
		spa_log_error(monitor->log, "error listening SCO connection: %s", strerror(errno));
		goto fail;
	}

#if 0
	if (t->state != PA_BLUETOOTH_TRANSPORT_STATE_PLAYING) {
		spa_log_info(monitor->log, "SCO incoming connection: changing state to PLAYING");
		pa_bluetooth_transport_set_state (t, PA_BLUETOOTH_TRANSPORT_STATE_PLAYING);
	}
#endif

fail:
	return;
}

static int sco_listen(struct spa_bt_transport *t)
{
	struct spa_bt_monitor *monitor = t->monitor;
	struct transport_data *td = t->user_data;
	struct sockaddr_sco addr;
	int sock, i;
	bdaddr_t src;
	const char *src_addr;

	if (t->device->adapter == NULL)
		return -EIO;

	sock = socket(PF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BTPROTO_SCO);
	if (sock < 0) {
		spa_log_error(monitor->log, "socket(SEQPACKET, SCO) %m");
		return -errno;
	}

	src_addr = t->device->adapter->address;

	/* don't use ba2str to avoid -lbluetooth */
	for (i = 5; i >= 0; i--, src_addr += 3)
		src.b[i] = strtol(src_addr, NULL, 16);

	/* Bind to local address */
	memset(&addr, 0, sizeof(addr));
	addr.sco_family = AF_BLUETOOTH;
	bacpy(&addr.sco_bdaddr, &src);

	if (bind(sock, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
		spa_log_error(monitor->log, "bind(): %m");
		goto fail_close;
	}

	spa_log_info(monitor->log, "transport %p: doing listen", t);
	if (listen(sock, 1) < 0) {
		spa_log_error(monitor->log, "listen(): %m");
		goto fail_close;
	}

	td->sco.func = sco_event;
	td->sco.data = t;
	td->sco.fd = sock;
	td->sco.mask = SPA_IO_IN;
	td->sco.rmask = 0;
	spa_loop_add_source(monitor->main_loop, &td->sco);

	return sock;

fail_close:
	close(sock);
	return -1;
}

static int sco_destroy_cb(void *data)
{
	struct spa_bt_transport *trans = data;
	struct transport_data *td = trans->user_data;

	if (td->sco.data) {
		if (td->sco.loop)
			spa_loop_remove_source(td->sco.loop, &td->sco);
		shutdown(td->sco.fd, SHUT_RDWR);
		close (td->sco.fd);
		td->sco.fd = -1;
	}
	if (td->rfcomm.data) {
		if (td->rfcomm.loop)
			spa_loop_remove_source(td->rfcomm.loop, &td->rfcomm);
		shutdown(td->rfcomm.fd, SHUT_RDWR);
		close (td->rfcomm.fd);
		td->rfcomm.fd = -1;
	}
	return 0;
}

static const struct spa_bt_transport_implementation sco_transport_impl = {
	SPA_VERSION_BT_TRANSPORT_IMPLEMENTATION,
	.acquire = sco_acquire_cb,
	.release = sco_release_cb,
	.destroy = sco_destroy_cb,
};

static DBusHandlerResult profile_new_connection(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	DBusMessage *r;
	DBusMessageIter it[5];
	const char *handler, *path;
	char *pathfd;
	enum spa_bt_profile profile;
	struct spa_bt_device *d;
	struct spa_bt_transport *t;
	struct transport_data *td;
	int fd;

	if (!dbus_message_has_signature(m, "oha{sv}")) {
		spa_log_warn(monitor->log, "invalid NewConnection() signature");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	handler = dbus_message_get_path(m);
	if (strcmp(handler, PROFILE_HSP_AG) == 0)
		profile = SPA_BT_PROFILE_HSP_HS;
	else if (strcmp(handler, PROFILE_HSP_HS) == 0)
		profile = SPA_BT_PROFILE_HSP_AG;
	else if (strcmp(handler, PROFILE_HFP_HS) == 0)
		profile = SPA_BT_PROFILE_HFP_AG;
	else if (strcmp(handler, PROFILE_HFP_AG) == 0)
		profile = SPA_BT_PROFILE_HFP_HF;
	else {
		spa_log_warn(monitor->log, "invalid handler %s", handler);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_message_iter_init(m, &it[0]);
	dbus_message_iter_get_basic(&it[0], &path);

	d = device_find(monitor, path);
	if (d == NULL) {
		spa_log_warn(monitor->log, "unknown device for path %s", path);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_message_iter_next(&it[0]);
	dbus_message_iter_get_basic(&it[0], &fd);

	spa_log_debug(monitor->log, "NewConnection path=%s, fd=%d, profile %s", path, fd, handler);

	if ((pathfd = spa_aprintf("%s/fd%d", path, fd)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	t = transport_create(monitor, pathfd, sizeof(struct transport_data));
	if (t == NULL) {
		spa_log_warn(monitor->log, "can't create transport: %m");
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	}
	spa_bt_transport_set_implementation(t, &sco_transport_impl, t);

	t->device = d;
	spa_list_append(&t->device->transport_list, &t->device_link);
	t->profile = profile;

	td = t->user_data;
	td->rfcomm.func = rfcomm_event;
	td->rfcomm.data = t;
	td->rfcomm.fd = fd;
	td->rfcomm.mask = SPA_IO_IN;
	td->rfcomm.rmask = 0;
	spa_loop_add_source(monitor->main_loop, &td->rfcomm);

	device_connect_profile(t->device, profile);

	sco_listen(t);

	spa_log_debug(monitor->log, "Transport %s available for profile %s", t->path, handler);

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult profile_request_disconnection(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	DBusMessage *r;
	const char *handler, *path;
	struct spa_bt_device *d;
	struct spa_bt_transport *t, *tmp;
	enum spa_bt_profile profile;
	DBusMessageIter it[5];

	if (!dbus_message_has_signature(m, "o")) {
		spa_log_warn(monitor->log, "invalid RequestDisconnection() signature");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	handler = dbus_message_get_path(m);
	if (strcmp(handler, PROFILE_HSP_AG) == 0)
		profile = SPA_BT_PROFILE_HSP_HS;
	else if (strcmp(handler, PROFILE_HSP_HS) == 0)
		profile = SPA_BT_PROFILE_HSP_AG;
	else if (strcmp(handler, PROFILE_HFP_HS) == 0)
		profile = SPA_BT_PROFILE_HFP_AG;
	else if (strcmp(handler, PROFILE_HFP_AG) == 0)
		profile = SPA_BT_PROFILE_HFP_HF;
	else {
		spa_log_warn(monitor->log, "invalid handler %s", handler);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_message_iter_init(m, &it[0]);
	dbus_message_iter_get_basic(&it[0], &path);

	d = device_find(monitor, path);
	if (d == NULL) {
		spa_log_warn(monitor->log, "unknown device for path %s", path);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	spa_list_for_each_safe(t, tmp, &d->transport_list, device_link) {
		if (t->profile == profile)
			transport_free(t);
	}
	device_check_profiles(d, false);

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	dbus_message_unref(r);
	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult profile_handler(DBusConnection *c, DBusMessage *m, void *userdata)
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
		const char *xml = PROFILE_INTROSPECT_XML;

		if ((r = dbus_message_new_method_return(m)) == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_connection_send(monitor->conn, r, NULL))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		dbus_message_unref(r);
		res = DBUS_HANDLER_RESULT_HANDLED;
	}
	else if (dbus_message_is_method_call(m, BLUEZ_PROFILE_INTERFACE, "Release"))
		res = profile_release(c, m, userdata);
	else if (dbus_message_is_method_call(m, BLUEZ_PROFILE_INTERFACE, "RequestDisconnection"))
		res = profile_request_disconnection(c, m, userdata);
	else if (dbus_message_is_method_call(m, BLUEZ_PROFILE_INTERFACE, "NewConnection"))
		res = profile_new_connection(c, m, userdata);
	else
		res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	return res;
}

static void register_profile_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_monitor *monitor = user_data;
	DBusMessage *r;

	r = dbus_pending_call_steal_reply(pending);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, BLUEZ_ERROR_NOT_SUPPORTED)) {
		spa_log_warn(monitor->log, "Register profile not supported");
		goto finish;
	}
	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(monitor->log, "Error registering profile");
		goto finish;
	}
	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(monitor->log, "RegisterProfile() failed: %s",
				dbus_message_get_error_name(r));
		goto finish;
	}

      finish:
	dbus_message_unref(r);
        dbus_pending_call_unref(pending);
}

static int register_profile(struct spa_bt_monitor *monitor, const char *profile, const char *uuid)
{
	static const DBusObjectPathVTable vtable_profile = {
		.message_function = profile_handler,
	};
	DBusMessage *m;
	DBusMessageIter it[4];
	dbus_bool_t autoconnect;
	dbus_uint16_t version, chan;
	char *str;
	DBusPendingCall *call;

	spa_log_debug(monitor->log, "Registering Profile %s %s", profile, uuid);

	if (!dbus_connection_register_object_path(monitor->conn,
						  profile,
						  &vtable_profile, monitor))
		return -EIO;

	m = dbus_message_new_method_call(BLUEZ_SERVICE, "/org/bluez",
			BLUEZ_PROFILE_MANAGER_INTERFACE, "RegisterProfile");
	if (m == NULL)
		return -ENOMEM;

	dbus_message_iter_init_append(m, &it[0]);
	dbus_message_iter_append_basic(&it[0], DBUS_TYPE_OBJECT_PATH, &profile);
	dbus_message_iter_append_basic(&it[0], DBUS_TYPE_STRING, &uuid);
	dbus_message_iter_open_container(&it[0], DBUS_TYPE_ARRAY, "{sv}", &it[1]);

	if (strcmp(uuid, SPA_BT_UUID_HSP_HS) == 0 ||
	    strcmp(uuid, SPA_BT_UUID_HSP_HS_ALT) == 0) {

		/* In the headset role, the connection will only be initiated from the remote side */
		str = "AutoConnect";
		autoconnect = 0;
		dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
		dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
		dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "b", &it[3]);
		dbus_message_iter_append_basic(&it[3], DBUS_TYPE_BOOLEAN, &autoconnect);
		dbus_message_iter_close_container(&it[2], &it[3]);
		dbus_message_iter_close_container(&it[1], &it[2]);

		str = "Channel";
		chan = HSP_HS_DEFAULT_CHANNEL;
		dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
		dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
		dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "q", &it[3]);
		dbus_message_iter_append_basic(&it[3], DBUS_TYPE_UINT16, &chan);
		dbus_message_iter_close_container(&it[2], &it[3]);
		dbus_message_iter_close_container(&it[1], &it[2]);

		/* HSP version 1.2 */
		str = "Version";
		version = 0x0102;
		dbus_message_iter_open_container(&it[1], DBUS_TYPE_DICT_ENTRY, NULL, &it[2]);
		dbus_message_iter_append_basic(&it[2], DBUS_TYPE_STRING, &str);
		dbus_message_iter_open_container(&it[2], DBUS_TYPE_VARIANT, "q", &it[3]);
		dbus_message_iter_append_basic(&it[3], DBUS_TYPE_UINT16, &version);
		dbus_message_iter_close_container(&it[2], &it[3]);
		dbus_message_iter_close_container(&it[1], &it[2]);
	}
	dbus_message_iter_close_container(&it[0], &it[1]);

	dbus_connection_send_with_reply(monitor->conn, m, &call, -1);
	dbus_pending_call_set_notify(call, register_profile_reply, monitor, NULL);
        dbus_message_unref(m);
	return 0;
}

static void interface_added(struct spa_bt_monitor *monitor,
			    DBusConnection *conn,
			    const char *object_path,
			    const char *interface_name,
			    DBusMessageIter *props_iter)
{
	spa_log_debug(monitor->log, "Found object %s, interface %s", object_path, interface_name);

	if (strcmp(interface_name, BLUEZ_ADAPTER_INTERFACE) == 0) {
		struct spa_bt_adapter *a;

		a = adapter_find(monitor, object_path);
		if (a == NULL) {
			a = adapter_create(monitor, object_path);
			if (a == NULL) {
				spa_log_warn(monitor->log, "can't create adapter: %m");
				return;
			}
		}
		adapter_update_props(a, props_iter, NULL);
		adapter_register_endpoints(a);
	}
	else if (strcmp(interface_name, BLUEZ_PROFILE_MANAGER_INTERFACE) == 0) {
		register_profile(monitor, PROFILE_HSP_AG, SPA_BT_UUID_HSP_AG);
		register_profile(monitor, PROFILE_HSP_HS, SPA_BT_UUID_HSP_HS);
		register_profile(monitor, PROFILE_HFP_AG, SPA_BT_UUID_HFP_AG);
		register_profile(monitor, PROFILE_HFP_HS, SPA_BT_UUID_HFP_HF);
	}
	else if (strcmp(interface_name, BLUEZ_DEVICE_INTERFACE) == 0) {
		struct spa_bt_device *d;

		d = device_find(monitor, object_path);
		if (d == NULL) {
			d = device_create(monitor, object_path);
			if (d == NULL) {
				spa_log_warn(monitor->log, "can't create device: %m");
				return;
			}
		}
		device_update_props(d, props_iter, NULL);
	}
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

static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *m, void *user_data)
{
	struct spa_bt_monitor *monitor = user_data;
	DBusError err;

	dbus_error_init(&err);

	if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
		spa_log_debug(monitor->log, "Name owner changed %s", dbus_message_get_path(m));
	} else if (dbus_message_is_signal(m, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded")) {
		spa_log_debug(monitor->log, "interfaces added %s", dbus_message_get_path(m));
	} else if (dbus_message_is_signal(m, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved")) {
		spa_log_debug(monitor->log, "interfaces removed %s", dbus_message_get_path(m));
	} else if (dbus_message_is_signal(m, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
		DBusMessageIter it[2];
		const char *iface, *path;

		if (!dbus_message_iter_init(m, &it[0]) ||
		    strcmp(dbus_message_get_signature(m), "sa{sv}as") != 0) {
			spa_log_error(monitor->log, "Invalid signature found in PropertiesChanged");
			goto fail;
		}
		path = dbus_message_get_path(m);

		dbus_message_iter_get_basic(&it[0], &iface);
		dbus_message_iter_next(&it[0]);
		dbus_message_iter_recurse(&it[0], &it[1]);

		if (strcmp(iface, BLUEZ_ADAPTER_INTERFACE) == 0) {
			struct spa_bt_adapter *a;

			a = adapter_find(monitor, path);
			if (a == NULL) {
				spa_log_warn(monitor->log,
						"Properties changed in unknown adapter %s", path);
				goto fail;
			}
			spa_log_debug(monitor->log, "Properties changed in adapter %s", path);

			adapter_update_props(a, &it[1], NULL);
		}
		else if (strcmp(iface, BLUEZ_DEVICE_INTERFACE) == 0) {
			struct spa_bt_device *d;

			d = device_find(monitor, path);
			if (d == NULL) {
				spa_log_warn(monitor->log,
						"Properties changed in unknown device %s", path);
				goto fail;
			}
			spa_log_debug(monitor->log, "Properties changed in device %s", path);

			device_update_props(d, &it[1], NULL);
		}
		else if (strcmp(iface, BLUEZ_MEDIA_TRANSPORT_INTERFACE) == 0) {
			struct spa_bt_transport *transport;

			transport = transport_find(monitor, path);
			if (transport == NULL) {
				spa_log_warn(monitor->log,
						"Properties changed in unknown transport %s", path);
				goto fail;
			}

			spa_log_debug(monitor->log, "Properties changed in transport %s", path);

			transport_update_props(transport, &it[1], NULL);
		}
        }

fail:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void add_filters(struct spa_bt_monitor *this)
{
	DBusError err;

	if (this->filters_added)
		return;

	dbus_error_init(&err);

	if (!dbus_connection_add_filter(this->conn, filter_cb, this, NULL)) {
		spa_log_error(this->log, "failed to add filter function");
		goto fail;
	}

	dbus_bus_add_match(this->conn,
			"type='signal',sender='org.freedesktop.DBus',"
			"interface='org.freedesktop.DBus',member='NameOwnerChanged',"
			"arg0='" BLUEZ_SERVICE "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" BLUEZ_SERVICE "',"
			"interface='org.freedesktop.DBus.ObjectManager',member='InterfacesAdded'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" BLUEZ_SERVICE "',"
			"interface='org.freedesktop.DBus.ObjectManager',member='InterfacesRemoved'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" BLUEZ_SERVICE "',"
			"interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
			"arg0='" BLUEZ_ADAPTER_INTERFACE "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" BLUEZ_SERVICE "',"
			"interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
			"arg0='" BLUEZ_DEVICE_INTERFACE "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" BLUEZ_SERVICE "',"
			"interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
			"arg0='" BLUEZ_MEDIA_TRANSPORT_INTERFACE "'", &err);

	this->filters_added = true;

	return;

fail:
	dbus_error_free(&err);
}

static int
impl_device_add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	struct spa_bt_monitor *this = object;
        struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

        spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	add_filters(this);
	get_managed_objects(this);

        spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static const struct spa_device_methods impl_device = {
	SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_device_add_listener,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct spa_bt_monitor *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct spa_bt_monitor *) handle;

	if (strcmp(type, SPA_TYPE_INTERFACE_Device) == 0)
		*interface = &this->device;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct spa_bt_monitor);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct spa_bt_monitor *this;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct spa_bt_monitor *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	this->dbus = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DBus);
	this->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);
	this->main_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_System);

	if (this->dbus == NULL) {
		spa_log_error(this->log, "a dbus is needed");
		return -EINVAL;
	}

	this->dbus_connection = spa_dbus_get_connection(this->dbus, DBUS_BUS_SYSTEM);
	if (this->dbus_connection == NULL) {
		spa_log_error(this->log, "no dbus connection");
		return -EIO;
	}
	this->conn = spa_dbus_connection_get(this->dbus_connection);

	spa_hook_list_init(&this->hooks);

	this->device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, this);

	spa_list_init(&this->adapter_list);
	spa_list_init(&this->device_list);
	spa_list_init(&this->transport_list);

	return 0;
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

const struct spa_handle_factory spa_bluez5_dbus_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BLUEZ5_ENUM_DBUS,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
