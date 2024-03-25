/* Spa V4l2 dbus */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <limits.h>

#include <bluetooth/bluetooth.h>

#include <dbus/dbus.h>

#include <spa/debug/mem.h>
#include <spa/debug/log.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/dbus.h>
#include <spa/support/plugin.h>
#include <spa/support/plugin-loader.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/utils/hook.h>
#include <spa/utils/type.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>

#include "config.h"
#include "codec-loader.h"
#include "dbus-helpers.h"
#include "player.h"
#include "iso-io.h"
#include "bap-codec-caps.h"
#include "defs.h"

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

enum backend_selection {
	BACKEND_NONE = -2,
	BACKEND_ANY = -1,
	BACKEND_HSPHFPD = 0,
	BACKEND_OFONO = 1,
	BACKEND_NATIVE = 2,
	BACKEND_NUM,
};

/*
 * Rate limit for BlueZ SetConfiguration calls.
 *
 * Too rapid calls to BlueZ API may cause A2DP profile to disappear, as the
 * internal BlueZ/connection state gets confused. Use some reasonable minimum
 * interval.
 *
 * AVDTP v1.3 Sec. 6.13 mentions 3 seconds as a reasonable timeout in one case
 * (ACP connection reset timeout, if no INT response). The case here is
 * different, but we assume a similar value is fine here.
 */
#define BLUEZ_ACTION_RATE_MSEC	3000

#define CODEC_SWITCH_RETRIES	1

/* How many times to retry acquire on errors, and how long delay to require before we can
 * try again.
 */
#define TRANSPORT_ERROR_MAX_RETRY	3
#define TRANSPORT_ERROR_TIMEOUT		(2*BLUEZ_ACTION_RATE_MSEC*SPA_NSEC_PER_MSEC)


struct spa_bt_monitor {
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;
	struct spa_loop *main_loop;
	struct spa_loop *data_loop;
	struct spa_system *main_system;
	struct spa_system *data_system;
	struct spa_plugin_loader *plugin_loader;
	struct spa_dbus *dbus;
	struct spa_dbus_connection *dbus_connection;
	DBusConnection *conn;

	struct spa_hook_list hooks;

	uint32_t id;

	const struct media_codec * const * media_codecs;

	/*
	 * Lists of BlueZ objects, kept up-to-date by following DBus events
	 * initiated by BlueZ. Object lifetime is also determined by that.
	 */
	struct spa_list adapter_list;
	struct spa_list device_list;
	struct spa_list remote_endpoint_list;
	struct spa_list transport_list;

	unsigned int filters_added:1;
	unsigned int objects_listed:1;
	DBusPendingCall *get_managed_objects_call;

	struct spa_bt_backend *backend;
	struct spa_bt_backend *backends[BACKEND_NUM];
	enum backend_selection backend_selection;

	struct spa_dict enabled_codecs;

	enum spa_bt_profile enabled_profiles;

	unsigned int connection_info_supported:1;
	unsigned int dummy_avrcp_player:1;

	struct spa_bt_quirks *quirks;

#define MAX_SETTINGS 128
	struct spa_dict_item global_setting_items[MAX_SETTINGS];
	struct spa_dict global_settings;

	/* A reference audio info for A2DP codec configuration. */
	struct media_codec_audio_info default_audio_info;
};

/* Stream endpoints owned by BlueZ for each device */
struct spa_bt_remote_endpoint {
	struct spa_list link;
	struct spa_list device_link;
	struct spa_bt_monitor *monitor;
	char *path;

	char *uuid;
	unsigned int codec;
	struct spa_bt_device *device;
	uint8_t *capabilities;
	int capabilities_len;
	bool delay_reporting;
	bool acceptor;
};

/*
 * Codec switching tries various codec/remote endpoint combinations
 * in order, until an acceptable one is found. This triggers BlueZ
 * to initiate DBus calls that result to the creation of a transport
 * with the desired capabilities.
 * The codec switch struct tracks candidates still to be tried.
 */
struct spa_bt_media_codec_switch {
	struct spa_bt_device *device;
	struct spa_list device_link;

	/*
	 * Codec switch may be waiting for either DBus reply from BlueZ
	 * or a timeout (but not both).
	 */
	struct spa_source timer;
	DBusPendingCall *pending;

	uint32_t profile;

	/*
	 * Called asynchronously, so endpoint paths instead of pointers (which may be
	 * invalidated in the meantime).
	 */
	const struct media_codec **codecs;
	char **paths;

	const struct media_codec **codec_iter;	/**< outer iterator over codecs */
	char **path_iter;			/**< inner iterator over endpoint paths */

	uint16_t retries;
	size_t num_paths;
};

#define DEFAULT_RECONNECT_PROFILES SPA_BT_PROFILE_NULL
#define DEFAULT_HW_VOLUME_PROFILES (SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY | SPA_BT_PROFILE_HEADSET_HEAD_UNIT | \
					SPA_BT_PROFILE_A2DP_SOURCE | SPA_BT_PROFILE_A2DP_SINK)

#define BT_DEVICE_DISCONNECTED	0
#define BT_DEVICE_CONNECTED	1
#define BT_DEVICE_INIT		-1

/*
 * SCO socket connect may fail with ECONNABORTED if it is done too soon after
 * previous close. To avoid this in cases where nodes are toggled between
 * stopped/started rapidly, postpone release until the transport has remained
 * unused for a time.
 *
 * Avoiding unnecessary release+reacquire also makes sense for ISO.
 */
#define TRANSPORT_RELEASE_TIMEOUT_MSEC 1000

#define TRANSPORT_VOLUME_TIMEOUT_MSEC 200

#define SPA_BT_TRANSPORT_IS_A2DP(transport) ((transport)->profile & (SPA_BT_PROFILE_A2DP_SOURCE | SPA_BT_PROFILE_A2DP_SINK))

static int spa_bt_transport_stop_volume_timer(struct spa_bt_transport *transport);
static int spa_bt_transport_start_volume_timer(struct spa_bt_transport *transport);
static int spa_bt_transport_stop_release_timer(struct spa_bt_transport *transport);
static int spa_bt_transport_start_release_timer(struct spa_bt_transport *transport);
static void spa_bt_transport_commit_release_timer(struct spa_bt_transport *transport);

static int device_start_timer(struct spa_bt_device *device);
static int device_stop_timer(struct spa_bt_device *device);

static void media_codec_switch_free(struct spa_bt_media_codec_switch *sw);

// Working with BlueZ Battery Provider.
// Developed using https://github.com/dgreid/adhd/commit/655b58f as an example of DBus calls.

// Name of battery, formatted as /org/freedesktop/pipewire/battery/org/bluez/hciX/dev_XX_XX_XX_XX_XX_XX
static char *battery_get_name(const char *device_path)
{
	return spa_aprintf(PIPEWIRE_BATTERY_PROVIDER "%s", device_path);
}

// Unregister virtual battery of device
static void battery_remove(struct spa_bt_device *device)
{
	DBusMessageIter i, entry;
	spa_autoptr(DBusMessage) m = NULL;
	const char *interface;

	cancel_and_unref(&device->battery_pending_call);

	if (!device->adapter || !device->adapter->has_battery_provider || !device->has_battery)
		return;

	spa_log_debug(device->monitor->log, "Removing virtual battery: %s", device->battery_path);

	m = dbus_message_new_signal(PIPEWIRE_BATTERY_PROVIDER,
				      DBUS_INTERFACE_OBJECT_MANAGER,
				      DBUS_SIGNAL_INTERFACES_REMOVED);


	dbus_message_iter_init_append(m, &i);
	dbus_message_iter_append_basic(&i, DBUS_TYPE_OBJECT_PATH,
				       &device->battery_path);
	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY,
					 DBUS_TYPE_STRING_AS_STRING, &entry);
	interface = BLUEZ_INTERFACE_BATTERY_PROVIDER;
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING,
				       &interface);
	dbus_message_iter_close_container(&i, &entry);

	if (!dbus_connection_send(device->monitor->conn, m, NULL)) {
		spa_log_error(device->monitor->log, "sending " DBUS_SIGNAL_INTERFACES_REMOVED " failed");
	}

	device->has_battery = false;
}

// Create properties for Battery Provider request
static void battery_write_properties(DBusMessageIter *iter, struct spa_bt_device *device)
{
	DBusMessageIter dict, entry, variant;

	dbus_message_iter_open_container(iter, DBUS_TYPE_ARRAY, "{sv}", &dict);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL,
					 &entry);
	const char *prop_percentage = "Percentage";
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &prop_percentage);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
					 DBUS_TYPE_BYTE_AS_STRING, &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_BYTE, &device->battery);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(&dict, &entry);

	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	const char *prop_device = "Device";
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &prop_device);
	dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT,
					 DBUS_TYPE_OBJECT_PATH_AS_STRING,
					 &variant);
	dbus_message_iter_append_basic(&variant, DBUS_TYPE_OBJECT_PATH, &device->path);
	dbus_message_iter_close_container(&entry, &variant);
	dbus_message_iter_close_container(&dict, &entry);

	dbus_message_iter_close_container(iter, &dict);
}

// Send current percentage to BlueZ
static void battery_update(struct spa_bt_device *device)
{
	spa_log_debug(device->monitor->log, "updating battery: %s", device->battery_path);

	spa_autoptr(DBusMessage) msg = NULL;
	DBusMessageIter iter;

	msg = dbus_message_new_signal(device->battery_path,
				      DBUS_INTERFACE_PROPERTIES,
				      DBUS_SIGNAL_PROPERTIES_CHANGED);

	dbus_message_iter_init_append(msg, &iter);
	const char *interface = BLUEZ_INTERFACE_BATTERY_PROVIDER;
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING,
				       &interface);

	battery_write_properties(&iter, device);

	if (!dbus_connection_send(device->monitor->conn, msg, NULL))
		spa_log_error(device->monitor->log, "Error updating battery");
}

// Create new virtual battery with value stored in current device object
static void battery_create(struct spa_bt_device *device)
{
	spa_autoptr(DBusMessage) msg = NULL;
	DBusMessageIter iter, entry, dict;
	msg = dbus_message_new_signal(PIPEWIRE_BATTERY_PROVIDER,
				      DBUS_INTERFACE_OBJECT_MANAGER,
				      DBUS_SIGNAL_INTERFACES_ADDED);

	dbus_message_iter_init_append(msg, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH,
				       &device->battery_path);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sa{sv}}", &dict);
	dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	const char *interface = BLUEZ_INTERFACE_BATTERY_PROVIDER;
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING,
				       &interface);

	battery_write_properties(&entry, device);

	dbus_message_iter_close_container(&dict, &entry);
	dbus_message_iter_close_container(&iter, &dict);

	if (!dbus_connection_send(device->monitor->conn, msg, NULL)) {
		spa_log_error(device->monitor->log, "Failed to create virtual battery for %s", device->address);
		return;
	}

	spa_log_debug(device->monitor->log, "Created virtual battery for %s", device->address);
	device->has_battery = true;
}

static void on_battery_provider_registered(DBusPendingCall *pending_call,
				       void *data)
{
	struct spa_bt_device *device = data;

	spa_assert(device->battery_pending_call == pending_call);
	spa_autoptr(DBusMessage) reply = steal_reply_and_unref(&device->battery_pending_call);

	if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(device->monitor->log, "Failed to register battery provider. Error: %s", dbus_message_get_error_name(reply));
		spa_log_error(device->monitor->log, "BlueZ Battery Provider is not available, won't retry to register it. Make sure you are running BlueZ 5.56+ with experimental features to use Battery Provider.");
		device->adapter->battery_provider_unavailable = true;
		return;
	}

	spa_log_debug(device->monitor->log, "Registered Battery Provider");

	device->adapter->has_battery_provider = true;

	if (!device->has_battery)
		battery_create(device);
}

// Register Battery Provider for adapter and then create virtual battery for device
static void register_battery_provider(struct spa_bt_device *device)
{
	spa_autoptr(DBusMessage) method_call = NULL;
	DBusMessageIter message_iter;

	if (device->battery_pending_call) {
		spa_log_debug(device->monitor->log, "Already registering battery provider");
		return;
	}

	method_call = dbus_message_new_method_call(
		BLUEZ_SERVICE, device->adapter_path,
		BLUEZ_INTERFACE_BATTERY_PROVIDER_MANAGER,
		"RegisterBatteryProvider");

	if (!method_call) {
		spa_log_error(device->monitor->log, "Failed to register battery provider");
		return;
	}

	dbus_message_iter_init_append(method_call, &message_iter);
	const char *object_path = PIPEWIRE_BATTERY_PROVIDER;
	dbus_message_iter_append_basic(&message_iter, DBUS_TYPE_OBJECT_PATH,
				       &object_path);

	device->battery_pending_call = send_with_reply(device->monitor->conn, method_call,
						       on_battery_provider_registered, device);
	if (!device->battery_pending_call) {
		spa_log_error(device->monitor->log, "Failed to register battery provider");
		return;
	}
}

static int media_codec_to_endpoint(const struct media_codec *codec,
				   enum spa_bt_media_direction direction,
				   char** object_path)
{
	const char * endpoint;

	if (direction == SPA_BT_MEDIA_SOURCE)
		endpoint = codec->bap ? BAP_SOURCE_ENDPOINT : A2DP_SOURCE_ENDPOINT;
	else if (direction == SPA_BT_MEDIA_SINK) 
		endpoint = codec->bap ? BAP_SINK_ENDPOINT : A2DP_SINK_ENDPOINT;
	else if (direction == SPA_BT_MEDIA_SOURCE_BROADCAST) 
		endpoint = BAP_BROADCAST_SOURCE_ENDPOINT;
	else if (direction == SPA_BT_MEDIA_SINK_BROADCAST) 
		endpoint = BAP_BROADCAST_SINK_ENDPOINT;

	*object_path = spa_aprintf("%s/%s", endpoint,
		codec->endpoint_name ? codec->endpoint_name : codec->name);
	if (*object_path == NULL)
		return -errno;
	return 0;
}

static const struct media_codec *media_endpoint_to_codec(struct spa_bt_monitor *monitor, const char *endpoint, bool *sink, const struct media_codec *preferred)
{
	const char *ep_name;
	const struct media_codec * const * const media_codecs = monitor->media_codecs;
	const struct media_codec *found = NULL;
	int i;

	if (spa_strstartswith(endpoint, A2DP_SINK_ENDPOINT "/")) {
		ep_name = endpoint + strlen(A2DP_SINK_ENDPOINT "/");
		*sink = true;
	} else if (spa_strstartswith(endpoint, A2DP_SOURCE_ENDPOINT "/")) {
		ep_name = endpoint + strlen(A2DP_SOURCE_ENDPOINT "/");
		*sink = false;
	} else if (spa_strstartswith(endpoint, BAP_SOURCE_ENDPOINT "/")) {
		ep_name = endpoint + strlen(BAP_SOURCE_ENDPOINT "/");
		*sink = false;
	} else if (spa_strstartswith(endpoint, BAP_SINK_ENDPOINT "/")) {
		ep_name = endpoint + strlen(BAP_SINK_ENDPOINT "/");
		*sink = true;
	} else if (spa_strstartswith(endpoint, BAP_BROADCAST_SOURCE_ENDPOINT "/")) {
		ep_name = endpoint + strlen(BAP_BROADCAST_SOURCE_ENDPOINT "/");
		*sink = false;
	} else if (spa_strstartswith(endpoint, BAP_BROADCAST_SINK_ENDPOINT "/")) {
		ep_name = endpoint + strlen(BAP_BROADCAST_SINK_ENDPOINT "/");
		*sink = true;
	} else {
		*sink = true;
		return NULL;
	}

	for (i = 0; media_codecs[i]; i++) {
		const struct media_codec *codec = media_codecs[i];
		const char *codec_ep_name =
			codec->endpoint_name ? codec->endpoint_name : codec->name;

		if (!spa_streq(ep_name, codec_ep_name))
			continue;
		if ((*sink && !codec->decode) || (!*sink && !codec->encode))
			continue;

		/* Same endpoint may be shared with multiple codec objects,
		 * which may e.g. correspond to different encoder settings.
		 * Look up which one we selected.
		 */
		if ((preferred && codec == preferred) || found == NULL)
			found = codec;
	}
	return found;
}

static int media_endpoint_to_profile(const char *endpoint)
{

	if (spa_strstartswith(endpoint, A2DP_SINK_ENDPOINT "/"))
		return SPA_BT_PROFILE_A2DP_SOURCE;
	else if (spa_strstartswith(endpoint, A2DP_SOURCE_ENDPOINT "/"))
		return SPA_BT_PROFILE_A2DP_SINK;
	else if (spa_strstartswith(endpoint, BAP_SINK_ENDPOINT "/"))
		return SPA_BT_PROFILE_BAP_SOURCE;
	else if (spa_strstartswith(endpoint, BAP_SOURCE_ENDPOINT "/"))
		return SPA_BT_PROFILE_BAP_SINK;
	else if (spa_strstartswith(endpoint, BAP_BROADCAST_SINK_ENDPOINT "/"))
		return SPA_BT_PROFILE_BAP_BROADCAST_SOURCE;
	else if (spa_strstartswith(endpoint, BAP_BROADCAST_SOURCE_ENDPOINT "/"))
		return SPA_BT_PROFILE_BAP_BROADCAST_SINK;
	else
		return SPA_BT_PROFILE_NULL;
}

static bool is_media_codec_enabled(struct spa_bt_monitor *monitor, const struct media_codec *codec)
{
	return spa_dict_lookup(&monitor->enabled_codecs, codec->name) != NULL;
}

static bool codec_has_direction(const struct media_codec *codec, enum spa_bt_media_direction direction)
{
	switch (direction) {
	case SPA_BT_MEDIA_SOURCE:
	case SPA_BT_MEDIA_SOURCE_BROADCAST:
		return codec->encode;
	case SPA_BT_MEDIA_SINK:
	case SPA_BT_MEDIA_SINK_BROADCAST:
		return codec->decode;
	default:
		spa_assert_not_reached();
	}
}

static enum spa_bt_profile get_codec_profile(const struct media_codec *codec,
		enum spa_bt_media_direction direction)
{
	switch (direction) {
	case SPA_BT_MEDIA_SOURCE:
		return codec->bap ? SPA_BT_PROFILE_BAP_SOURCE : SPA_BT_PROFILE_A2DP_SOURCE;
	case SPA_BT_MEDIA_SINK:
		return codec->bap ? SPA_BT_PROFILE_BAP_SINK : SPA_BT_PROFILE_A2DP_SINK;
	case SPA_BT_MEDIA_SOURCE_BROADCAST:
		return SPA_BT_PROFILE_BAP_BROADCAST_SOURCE;
	case SPA_BT_MEDIA_SINK_BROADCAST:
		return SPA_BT_PROFILE_BAP_BROADCAST_SINK;
	default:
		spa_assert_not_reached();
	}
}

static enum spa_bt_profile swap_profile(enum spa_bt_profile profile)
{
	switch (profile) {
	case SPA_BT_PROFILE_A2DP_SOURCE:
		return SPA_BT_PROFILE_A2DP_SINK;
	case SPA_BT_PROFILE_A2DP_SINK:
		return SPA_BT_PROFILE_A2DP_SOURCE;
	case SPA_BT_PROFILE_BAP_SOURCE:
		return SPA_BT_PROFILE_BAP_SINK;
	case SPA_BT_PROFILE_BAP_SINK:
		return SPA_BT_PROFILE_BAP_SOURCE;
	case SPA_BT_PROFILE_BAP_BROADCAST_SOURCE:
		return SPA_BT_PROFILE_BAP_BROADCAST_SINK;
	case SPA_BT_PROFILE_BAP_BROADCAST_SINK:
		return SPA_BT_PROFILE_BAP_BROADCAST_SOURCE;
	default:
		return SPA_BT_PROFILE_NULL;
	}
}

static bool endpoint_should_be_registered(struct spa_bt_monitor *monitor,
					  const struct media_codec *codec,
					  enum spa_bt_media_direction direction)
{
	/* Codecs with fill_caps == NULL share endpoint with another codec,
	 * and don't have their own endpoint
	 */
	return is_media_codec_enabled(monitor, codec) &&
		codec_has_direction(codec, direction) &&
		codec->fill_caps &&
		(get_codec_profile(codec, direction) & monitor->enabled_profiles);
}

static DBusHandlerResult endpoint_select_configuration(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	const char *path;
	uint8_t *cap, config[A2DP_MAX_CAPS_SIZE];
	uint8_t *pconf = (uint8_t *) config;
	spa_autoptr(DBusMessage) r = NULL;
	spa_auto(DBusError) err = DBUS_ERROR_INIT;
	int size, res;
	const struct media_codec *codec;
	bool sink;

	path = dbus_message_get_path(m);

	if (!dbus_message_get_args(m, &err, DBUS_TYPE_ARRAY,
				DBUS_TYPE_BYTE, &cap, &size, DBUS_TYPE_INVALID)) {
		spa_log_error(monitor->log, "Endpoint SelectConfiguration(): %s", err.message);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	spa_log_info(monitor->log, "%p: %s select conf %d", monitor, path, size);
	spa_debug_log_mem(monitor->log, SPA_LOG_LEVEL_DEBUG, 2, cap, (size_t)size);

	/* For codecs sharing the same endpoint, BlueZ-initiated connections
	 * always pick the default one. The session manager will
	 * switch the codec to a saved value after connection, so this generally
	 * does not matter.
	 */
	codec = media_endpoint_to_codec(monitor, path, &sink, NULL);
	spa_log_debug(monitor->log, "%p: %s codec:%s", monitor, path, codec ? codec->name : "<null>");

	if (codec != NULL)
		/* FIXME: We can't determine which device the SelectConfiguration()
		 * call is associated with, therefore device settings are not passed.
		 * This causes inconsistency with SelectConfiguration() triggered
		 * by codec switching.
		  */
		res = codec->select_config(codec, sink ? MEDIA_CODEC_FLAG_SINK : 0, cap, size, &monitor->default_audio_info,
				&monitor->global_settings, config);
	else
		res = -ENOTSUP;

	if (res < 0 || res != size) {
		spa_log_error(monitor->log, "can't select config: %d (%s)",
				res, spa_strerror(res));
		if ((r = dbus_message_new_error(m, "org.bluez.Error.InvalidArguments",
				"Unable to select configuration")) == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		goto exit_send;
	}
	spa_debug_log_mem(monitor->log, SPA_LOG_LEVEL_DEBUG, 2, pconf, (size_t)size);

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_message_append_args(r, DBUS_TYPE_ARRAY,
			DBUS_TYPE_BYTE, &pconf, size, DBUS_TYPE_INVALID))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

exit_send:
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	return DBUS_HANDLER_RESULT_HANDLED;
}

static void append_basic_variant_dict_entry(DBusMessageIter *dict, const char* key, int variant_type_int, const char* variant_type_str, void* variant);
static void append_basic_array_variant_dict_entry(DBusMessageIter *dict, const char* key, const char* variant_type_str, const char* array_type_str, int array_type_int, void* data, int data_size);
static struct spa_bt_remote_endpoint *remote_endpoint_find(struct spa_bt_monitor *monitor, const char *path);

static bool check_iter_signature(DBusMessageIter *it, const char *sig)
{
	char *v;
	bool res;
	v = dbus_message_iter_get_signature(it);
	res = spa_streq(v, sig);
	dbus_free(v);
	return res;
}

static void parse_codec_qos(struct spa_bt_monitor *monitor, DBusMessageIter *iter, struct bap_codec_qos_full *qos)
{
	DBusMessageIter dict_iter = *iter;

	memset(qos, 0, sizeof(*qos));
	qos->cig = 0xff;
	qos->cis = 0xff;
	qos->big = 0xff;
	qos->bis = 0xff;

	if (!check_iter_signature(&dict_iter, "{sv}")) {
		spa_log_warn(monitor->log, "Invalid BAP QoS in DBus");
		return;
	}

	while (dbus_message_iter_get_arg_type(&dict_iter) != DBUS_TYPE_INVALID) {
		DBusMessageIter it[2];
		const char *key;
		int type;

		dbus_message_iter_recurse(&dict_iter, &it[0]);
		dbus_message_iter_get_basic(&it[0], &key);
		dbus_message_iter_next(&it[0]);
		dbus_message_iter_recurse(&it[0], &it[1]);

		type = dbus_message_iter_get_arg_type(&it[1]);

		if (type == DBUS_TYPE_BYTE) {
			uint8_t value;

			dbus_message_iter_get_basic(&it[1], &value);
			spa_log_debug(monitor->log, "qos: %s=%d", key, (int)value);

			if (spa_streq(key, "PHY"))
				qos->qos.phy = value;
			else if (spa_streq(key, "Retransmissions"))
				qos->qos.retransmission = value;
			else if (spa_streq(key, "CIG"))
				qos->cig = value;
			else if (spa_streq(key, "CIS"))
				qos->cis = value;
			else if (spa_streq(key, "BIG"))
				qos->big = value;
			else if (spa_streq(key, "BIS"))
				qos->bis = value;
			else if (spa_streq(key, "TargetLatency"))
				qos->qos.target_latency = value;
			else if (spa_streq(key, "Framing"))
				qos->qos.framing = value;
		}
		else if (type == DBUS_TYPE_UINT16) {
			dbus_uint16_t value;

			dbus_message_iter_get_basic(&it[1], &value);
			spa_log_debug(monitor->log, "qos: %s=%d", key, (int)value);

			if (spa_streq(key, "SDU"))
				qos->qos.sdu = value;
			else if (spa_streq(key, "Latency") || spa_streq(key, "MaximumLatency"))
				qos->qos.latency = value;
		}
		else if (type == DBUS_TYPE_UINT32) {
			dbus_uint32_t value;

			dbus_message_iter_get_basic(&it[1], &value);
			spa_log_debug(monitor->log, "qos: %s=%d", key, (int)value);

			if (spa_streq(key, "Interval"))
				qos->qos.interval = value;
			else if (spa_streq(key, "PresentationDelay"))
				qos->qos.delay = value;
		}

		dbus_message_iter_next(&dict_iter);
	}
}

static void parse_endpoint_qos(struct spa_bt_monitor *monitor, DBusMessageIter *iter,
		struct bap_endpoint_qos *qos)
{
	DBusMessageIter dict_iter = *iter;

	if (!check_iter_signature(&dict_iter, "{sv}")) {
		spa_log_warn(monitor->log, "Invalid BAP Endpoint QoS in DBus");
		return;
	}

	while (dbus_message_iter_get_arg_type(&dict_iter) != DBUS_TYPE_INVALID) {
		DBusMessageIter it[2];
		const char *key;
		int type;

		dbus_message_iter_recurse(&dict_iter, &it[0]);
		dbus_message_iter_get_basic(&it[0], &key);
		dbus_message_iter_next(&it[0]);
		dbus_message_iter_recurse(&it[0], &it[1]);

		type = dbus_message_iter_get_arg_type(&it[1]);

		if (type == DBUS_TYPE_BYTE) {
			uint8_t value;

			dbus_message_iter_get_basic(&it[1], &value);
			spa_log_debug(monitor->log, "ep qos: %s=%d", key, (int)value);

			if (spa_streq(key, "Framing"))
				qos->framing = value;
			else if (spa_streq(key, "PHY"))
				qos->phy = value;
			else if (spa_streq(key, "Retransmissions"))
				qos->retransmission = value;
		} else if (type == DBUS_TYPE_UINT16) {
			dbus_uint16_t value;

			dbus_message_iter_get_basic(&it[1], &value);
			spa_log_debug(monitor->log, "ep qos: %s=%d", key, (int)value);

			if (spa_streq(key, "Latency") || spa_streq(key, "MaximumLatency"))
				qos->latency = value;
			else if (spa_streq(key, "Context"))
				qos->context = value;
			else if (spa_streq(key, "SupportedContext"))
				qos->supported_context = value;
		} else if (type == DBUS_TYPE_UINT32) {
			dbus_uint32_t value;

			dbus_message_iter_get_basic(&it[1], &value);
			spa_log_debug(monitor->log, "ep qos: %s=%d", key, (int)value);

			if (spa_streq(key, "MinimumDelay"))
				qos->delay_min = value;
			else if (spa_streq(key, "MaximumDelay"))
				qos->delay_max = value;
			else if (spa_streq(key, "PreferredMinimumDelay"))
				qos->preferred_delay_min = value;
			else if (spa_streq(key, "PreferredMaximumDelay"))
				qos->preferred_delay_max = value;
			else if (spa_streq(key, "Locations") || spa_streq(key, "Location"))
				qos->locations = value;
		}

		dbus_message_iter_next(&dict_iter);
	}
}

static int parse_endpoint_props(struct spa_bt_monitor *monitor, DBusMessageIter *iter,
		uint8_t caps[A2DP_MAX_CAPS_SIZE], int *caps_size, const char **endpoint_path,
		struct bap_endpoint_qos *qos)
{
	DBusMessageIter dict_iter = *iter;
	const char *key = NULL;
	int type = 0;

	memset(caps, 0, A2DP_MAX_CAPS_SIZE);
	*endpoint_path = NULL;
	memset(qos, 0, sizeof(*qos));

	if (!check_iter_signature(&dict_iter, "{sv}")) {
		spa_log_warn(monitor->log, "Invalid BAP Endpoint QoS in DBus");
		return -EINVAL;
	}

	while (dbus_message_iter_get_arg_type(&dict_iter) != DBUS_TYPE_INVALID) {
		DBusMessageIter it[3];

		dbus_message_iter_recurse(&dict_iter, &it[0]);
		dbus_message_iter_get_basic(&it[0], &key);
		dbus_message_iter_next(&it[0]);
		dbus_message_iter_recurse(&it[0], &it[1]);

		type = dbus_message_iter_get_arg_type(&it[1]);

		if (spa_streq(key, "Capabilities")) {
			uint8_t *buf;

			if (type != DBUS_TYPE_ARRAY)
				goto bad_property;

			dbus_message_iter_recurse(&it[1], &it[2]);
			type = dbus_message_iter_get_arg_type(&it[2]);
			if (type != DBUS_TYPE_BYTE)
				goto bad_property;

			dbus_message_iter_get_fixed_array(&it[2], &buf, caps_size);
			if (*caps_size > A2DP_MAX_CAPS_SIZE) {
				spa_log_error(monitor->log, "%s size:%d too large", key, (int)*caps_size);
				return -EINVAL;
			}
			memcpy(caps, buf, *caps_size);

			spa_log_info(monitor->log, "%p: %s size:%d", monitor, key, *caps_size);
			spa_debug_log_mem(monitor->log, SPA_LOG_LEVEL_DEBUG, ' ', caps, (size_t)*caps_size);
		} else if (spa_streq(key, "Endpoint")) {
			if (type != DBUS_TYPE_OBJECT_PATH)
				goto bad_property;

			dbus_message_iter_get_basic(&it[1], endpoint_path);

			spa_log_info(monitor->log, "%p: %s %s", monitor, key, *endpoint_path);
		} else if (spa_streq(key, "QoS")) {
			if (!check_iter_signature(&it[1], "a{sv}"))
				goto bad_property;

			dbus_message_iter_recurse(&it[1], &it[2]);
			parse_endpoint_qos(monitor, &it[2], qos);
		} else if (spa_streq(key, "Locations")) {
			dbus_uint32_t value;

			if (type != DBUS_TYPE_UINT32)
				goto bad_property;

			dbus_message_iter_get_basic(&it[1], &value);
			spa_log_debug(monitor->log, "ep qos: %s=%d", key, (int)value);
			qos->locations = value;
		}

		dbus_message_iter_next(&dict_iter);
	}

	return 0;

bad_property:
	spa_log_error(monitor->log, "Property %s of wrong type %c", key, (char)type);
	return -EINVAL;
}

static DBusHandlerResult endpoint_select_properties(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	const char *path;
	DBusMessageIter args, props, iter;
	spa_autoptr(DBusMessage) r = NULL;
	int res;
	const struct media_codec *codec;
	struct spa_bt_remote_endpoint *ep;
	bool sink;
	const char *err_msg = "Unknown error";
	struct spa_dict settings;
	struct spa_dict_item setting_items[SPA_N_ELEMENTS(monitor->global_setting_items) + 2];
	int i;

	const char *endpoint_path = NULL;
	uint8_t caps[A2DP_MAX_CAPS_SIZE];
	uint8_t config[A2DP_MAX_CAPS_SIZE];
	char locations[64] = {0};
	int caps_size = 0;
	int conf_size;
	DBusMessageIter dict;
	struct bap_endpoint_qos endpoint_qos;

	spa_zero(endpoint_qos);

	if (!dbus_message_iter_init(m, &args) || !spa_streq(dbus_message_get_signature(m), "a{sv}")) {
		spa_log_error(monitor->log, "Invalid signature for method SelectProperties()");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_message_iter_recurse(&args, &props);
	if (dbus_message_iter_get_arg_type(&props) != DBUS_TYPE_DICT_ENTRY)
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	path = dbus_message_get_path(m);

	/* TODO: for codecs with shared endpoint, this currently always picks the default
	 * one. However, currently we don't have BAP codecs with shared endpoint, so
	 * this does not matter, but in case they are needed later we should pick the
	 * right one here.
	 */
	codec = media_endpoint_to_codec(monitor, path, &sink, NULL);
	spa_log_debug(monitor->log, "%p: %s codec:%s", monitor, path, codec ? codec->name : "<null>");
	if (!codec || !codec->bap || !codec->get_qos) {
		spa_log_error(monitor->log, "Unsupported codec");
		err_msg = "Unsupported codec";
		goto error;
	}

	/* Parse endpoint properties */
	if (parse_endpoint_props(monitor, &props, caps, &caps_size, &endpoint_path, &endpoint_qos) < 0)
		goto error_invalid;
	if (endpoint_qos.locations)
		spa_scnprintf(locations, sizeof(locations), "%"PRIu32, endpoint_qos.locations);

	ep = remote_endpoint_find(monitor, endpoint_path);
	if (!ep) {
		spa_log_warn(monitor->log, "Unable to find remote endpoint for %s", endpoint_path);
		goto error_invalid;
	}

	/* Call of SelectProperties means that local device acts as an initiator
	 * and therefor remote endpoint is an acceptor
	 */
	ep->acceptor = true;

	for (i = 0; i < (int)monitor->global_settings.n_items; ++i)
		setting_items[i] = monitor->global_settings.items[i];
	setting_items[i++] = SPA_DICT_ITEM_INIT("bluez5.bap.locations", locations);
	setting_items[i++] = SPA_DICT_ITEM_INIT("bluez5.bap.debug", "true");
	settings = SPA_DICT_INIT(setting_items, i);

	conf_size = codec->select_config(codec, 0, caps, caps_size, &monitor->default_audio_info, &settings, config);
	if (conf_size < 0) {
		spa_log_error(monitor->log, "can't select config: %d (%s)",
				conf_size, spa_strerror(conf_size));
		goto error_invalid;
	}
	spa_log_info(monitor->log, "%p: selected conf %d", monitor, conf_size);
	spa_debug_log_mem(monitor->log, SPA_LOG_LEVEL_DEBUG, ' ', (uint8_t *)config, (size_t)conf_size);

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	dbus_message_iter_init_append(r, &iter);

	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING
			DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
			&dict);
	append_basic_array_variant_dict_entry(&dict, "Capabilities", "ay", "y", DBUS_TYPE_BYTE, &config, conf_size);

	{
		struct bap_codec_qos qos;
		DBusMessageIter entry, variant, qos_dict;
		const char *entry_key = "QoS";

		spa_zero(qos);

		res = codec->get_qos(codec, config, conf_size, &endpoint_qos, &qos);
		if (res < 0) {
			spa_log_error(monitor->log, "can't select QOS config: %d (%s)",
					res, spa_strerror(res));
			goto error_invalid;
		}

		spa_log_debug(monitor->log, "select qos: interval:%d framing:%d phy:%d sdu:%d "
				"rtn:%d latency:%d delay:%d target_latency:%d",
				qos.interval, qos.framing, qos.phy, qos.sdu, qos.retransmission,
				qos.latency, (int)qos.delay, qos.target_latency);

		dbus_message_iter_open_container(&dict, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
		dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &entry_key);
		dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a{sv}", &variant);

		dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY,
			DBUS_DICT_ENTRY_BEGIN_CHAR_AS_STRING
			DBUS_TYPE_STRING_AS_STRING
			DBUS_TYPE_VARIANT_AS_STRING
			DBUS_DICT_ENTRY_END_CHAR_AS_STRING,
			&qos_dict);

		append_basic_variant_dict_entry(&qos_dict, "Interval", DBUS_TYPE_UINT32, "u", &qos.interval);
		append_basic_variant_dict_entry(&qos_dict, "Framing", DBUS_TYPE_BYTE, "y", &qos.framing);
		append_basic_variant_dict_entry(&qos_dict, "PHY", DBUS_TYPE_BYTE, "y", &qos.phy);
		append_basic_variant_dict_entry(&qos_dict, "SDU", DBUS_TYPE_UINT16, "q", &qos.sdu);
		append_basic_variant_dict_entry(&qos_dict, "Retransmissions", DBUS_TYPE_BYTE, "y", &qos.retransmission);
		append_basic_variant_dict_entry(&qos_dict, "Latency", DBUS_TYPE_UINT16, "q", &qos.latency);
		append_basic_variant_dict_entry(&qos_dict, "PresentationDelay", DBUS_TYPE_UINT32, "u", &qos.delay);
		append_basic_variant_dict_entry(&qos_dict, "TargetLatency", DBUS_TYPE_BYTE, "y", &qos.target_latency);

		dbus_message_iter_close_container(&variant, &qos_dict);
		dbus_message_iter_close_container(&entry, &variant);
		dbus_message_iter_close_container(&dict, &entry);
	}

	dbus_message_iter_close_container(&iter, &dict);

	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	return DBUS_HANDLER_RESULT_HANDLED;

error_invalid:
	err_msg = "Invalid property";
	goto error;

error:
	if (!reply_with_error(conn, m, "org.bluez.Error.InvalidArguments", err_msg))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	return DBUS_HANDLER_RESULT_HANDLED;
}

static struct spa_bt_adapter *adapter_find(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_adapter *d;
	spa_list_for_each(d, &monitor->adapter_list, link)
		if (spa_streq(d->path, path))
			return d;
	return NULL;
}

static int parse_modalias(const char *modalias, uint16_t *source, uint16_t *vendor,
		uint16_t *product, uint16_t *version)
{
	char *pos;
	unsigned int src, i, j, k;

	if (spa_strstartswith(modalias, "bluetooth:"))
		src = SOURCE_ID_BLUETOOTH;
	else if (spa_strstartswith(modalias, "usb:"))
		src = SOURCE_ID_USB;
	else
		return -EINVAL;

	pos = strchr(modalias, ':');
	if (pos == NULL)
		return -EINVAL;

	if (sscanf(pos + 1, "v%04Xp%04Xd%04X", &i, &j, &k) != 3)
		return -EINVAL;

	/* Ignore BlueZ placeholder value */
	if (src == SOURCE_ID_USB && i == 0x1d6b && j == 0x0246)
		return -ENXIO;

	*source = src;
	*vendor = i;
	*product = j;
	*version = k;

	return 0;
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

			if (spa_streq(key, "Alias")) {
				free(adapter->alias);
				adapter->alias = strdup(value);
			}
			else if (spa_streq(key, "Name")) {
				free(adapter->name);
				adapter->name = strdup(value);
			}
			else if (spa_streq(key, "Address")) {
				free(adapter->address);
				adapter->address = strdup(value);
			}
			else if (spa_streq(key, "Modalias")) {
				int ret;
				ret = parse_modalias(value, &adapter->source_id, &adapter->vendor_id,
						&adapter->product_id, &adapter->version_id);
				if (ret < 0)
					spa_log_debug(monitor->log, "adapter %p: %s=%s ignored: %s",
							adapter, key, value, spa_strerror(ret));
			}
		}
		else if (type == DBUS_TYPE_UINT32) {
			uint32_t value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "adapter %p: %s=%d", adapter, key, value);

			if (spa_streq(key, "Class"))
				adapter->bluetooth_class = value;

		}
		else if (type == DBUS_TYPE_BOOLEAN) {
			int value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "adapter %p: %s=%d", adapter, key, value);

			if (spa_streq(key, "Powered")) {
				adapter->powered = value;
			}
		}
		else if (spa_streq(key, "UUIDs")) {
			DBusMessageIter iter;

			if (!check_iter_signature(&it[1], "as"))
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
				} else if (strcasecmp(uuid, SPA_BT_UUID_PACS) == 0 &&
				           (adapter->profiles & SPA_BT_PROFILE_BAP_SINK) == 0) {
					spa_log_debug(monitor->log, "adapter %p: add UUID=%s", adapter, SPA_BT_UUID_BAP_SINK);
					adapter->profiles |= SPA_BT_PROFILE_BAP_SINK;
					spa_log_debug(monitor->log, "adapter %p: add UUID=%s", adapter, SPA_BT_UUID_BAP_SOURCE);
					adapter->profiles |= SPA_BT_PROFILE_BAP_SOURCE;
					spa_log_debug(monitor->log, "adapter %p: add UUID=%s", adapter, SPA_BT_UUID_BAP_BROADCAST_SOURCE);
					adapter->profiles |= SPA_BT_PROFILE_BAP_BROADCAST_SOURCE;
					spa_log_debug(monitor->log, "adapter %p: add UUID=%s", adapter, SPA_BT_UUID_BAP_BROADCAST_SINK);
					adapter->profiles |= SPA_BT_PROFILE_BAP_BROADCAST_SINK;
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

static int adapter_media_update_props(struct spa_bt_adapter *adapter,
				DBusMessageIter *props_iter,
				DBusMessageIter *invalidated_iter)
{
	/* Handle org.bluez.Media1 interface properties of .Adapter1 objects */
	struct spa_bt_monitor *monitor = adapter->monitor;

	while (dbus_message_iter_get_arg_type(props_iter) != DBUS_TYPE_INVALID) {
		DBusMessageIter it[2];
		const char *key;

		dbus_message_iter_recurse(props_iter, &it[0]);
		dbus_message_iter_get_basic(&it[0], &key);
		dbus_message_iter_next(&it[0]);
		dbus_message_iter_recurse(&it[0], &it[1]);

		if (spa_streq(key, "SupportedUUIDs")) {
			DBusMessageIter iter;

			if (!check_iter_signature(&it[1], "as"))
				goto next;

			dbus_message_iter_recurse(&it[1], &iter);

			while (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INVALID) {
				const char *uuid;

				dbus_message_iter_get_basic(&iter, &uuid);

				if (spa_streq(uuid, SPA_BT_UUID_BAP_SINK)) {
					adapter->le_audio_supported = true;
					spa_log_info(monitor->log, "Adapter %s: LE Audio supported",
							adapter->path);
				}

				if (spa_streq(uuid, SPA_BT_UUID_BAP_BROADCAST_SOURCE) ||
					spa_streq(uuid, SPA_BT_UUID_BAP_BROADCAST_SINK)) {
					adapter->le_audio_bcast_supported = true;
					spa_log_info(monitor->log, "Adapter %s: LE Broadcast Audio supported",
							adapter->path);
				}

				dbus_message_iter_next(&iter);
			}
		}
		else
			spa_log_debug(monitor->log, "media: unhandled key %s", key);

next:
		dbus_message_iter_next(props_iter);
	}
	return 0;
}

static void adapter_update_devices(struct spa_bt_adapter *adapter)
{
	struct spa_bt_monitor *monitor = adapter->monitor;
	struct spa_bt_device *device;

	/*
	 * Update devices when new adapter appears.
	 * Devices may appear on DBus before or after the adapter does.
	 */

	spa_list_for_each(device, &monitor->device_list, link) {
		if (device->adapter == NULL && spa_streq(device->adapter_path, adapter->path))
			device->adapter = adapter;
	}
}

static void adapter_register_player(struct spa_bt_adapter *adapter)
{
	if (adapter->player_registered || !adapter->monitor->dummy_avrcp_player)
		return;

	if (spa_bt_player_register(adapter->dummy_player, adapter->path) == 0)
		adapter->player_registered = true;
}

static int adapter_init_bus_type(struct spa_bt_monitor *monitor, struct spa_bt_adapter *d)
{
	char path[1024], buf[1024];
	const char *str;
	ssize_t res = -EINVAL;

	d->bus_type = BUS_TYPE_OTHER;

	str = strrchr(d->path, '/');  /* hciXX */
	if (str == NULL)
		return -ENOENT;

	snprintf(path, sizeof(path), "/sys/class/bluetooth/%s/device/subsystem", str);
	if ((res = readlink(path, buf, sizeof(buf)-1)) < 0)
		return -errno;
	buf[res] = '\0';

	str = strrchr(buf, '/');
	if (str && spa_streq(str, "/usb"))
		d->bus_type = BUS_TYPE_USB;
	return 0;
}

static int adapter_init_modalias(struct spa_bt_monitor *monitor, struct spa_bt_adapter *d)
{
	char path[1024];
	int vendor_id, product_id;
	const char *str;

	/* Lookup vendor/product id for the device, if present */
	str = strrchr(d->path, '/');  /* hciXX */
	if (str == NULL)
		return -EINVAL;

	snprintf(path, sizeof(path), "/sys/class/bluetooth/%s/device/modalias", str);

	spa_autoptr(FILE) f = fopen(path, "rbe");
	if (f == NULL)
		return -errno;

	if (fscanf(f, "usb:v%04Xp%04X",  &vendor_id, &product_id) != 2)
		return -EINVAL;

	d->source_id = SOURCE_ID_USB;
	d->vendor_id = vendor_id;
	d->product_id = product_id;

	spa_log_debug(monitor->log, "adapter %p: usb vendor:%04x product:%04x",
			d, vendor_id, product_id);
	return 0;
}

static struct spa_bt_adapter *adapter_create(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_adapter *d;

	d = calloc(1, sizeof(struct spa_bt_adapter));
	if (d == NULL)
		return NULL;

	d->dummy_player = spa_bt_player_new(monitor->conn, monitor->log);
	if (d->dummy_player == NULL) {
		free(d);
		return NULL;
	}

	d->monitor = monitor;
	d->path = strdup(path);

	spa_list_prepend(&monitor->adapter_list, &d->link);

	adapter_init_bus_type(monitor, d);
	adapter_init_modalias(monitor, d);

	return d;
}

static void device_free(struct spa_bt_device *device);

static void adapter_free(struct spa_bt_adapter *adapter)
{
	struct spa_bt_monitor *monitor = adapter->monitor;
	struct spa_bt_device *d, *td;

	spa_log_debug(monitor->log, "%p", adapter);

	/* Devices should be destroyed before their assigned adapter */
	spa_list_for_each_safe(d, td, &monitor->device_list, link)
		if (d->adapter == adapter)
			device_free(d);

	spa_bt_player_destroy(adapter->dummy_player);

	spa_list_remove(&adapter->link);
	free(adapter->alias);
	free(adapter->name);
	free(adapter->address);
	free(adapter->path);
	free(adapter);
}

static uint32_t adapter_connectable_profiles(struct spa_bt_adapter *adapter)
{
	const uint32_t profiles = adapter->profiles;
	uint32_t mask = 0;

	if (profiles & SPA_BT_PROFILE_A2DP_SINK)
		mask |= SPA_BT_PROFILE_A2DP_SOURCE;
	if (profiles & SPA_BT_PROFILE_A2DP_SOURCE)
		mask |= SPA_BT_PROFILE_A2DP_SINK;

	if (profiles & SPA_BT_PROFILE_BAP_SINK)
		mask |= SPA_BT_PROFILE_BAP_SOURCE;
	if (profiles & SPA_BT_PROFILE_BAP_SOURCE)
		mask |= SPA_BT_PROFILE_BAP_SINK;

	if (profiles & SPA_BT_PROFILE_BAP_BROADCAST_SINK)
		mask |= SPA_BT_PROFILE_BAP_BROADCAST_SOURCE;
	if (profiles & SPA_BT_PROFILE_BAP_BROADCAST_SOURCE)
		mask |= SPA_BT_PROFILE_BAP_BROADCAST_SINK;

	if (profiles & SPA_BT_PROFILE_HSP_AG)
		mask |= SPA_BT_PROFILE_HSP_HS;
	if (profiles & SPA_BT_PROFILE_HSP_HS)
		mask |= SPA_BT_PROFILE_HSP_AG;

	if (profiles & SPA_BT_PROFILE_HFP_AG)
		mask |= SPA_BT_PROFILE_HFP_HF;
	if (profiles & SPA_BT_PROFILE_HFP_HF)
		mask |= SPA_BT_PROFILE_HFP_AG;

	return mask;
}

struct spa_bt_device *spa_bt_device_find(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_device *d;
	spa_list_for_each(d, &monitor->device_list, link)
		if (spa_streq(d->path, path))
			return d;
	return NULL;
}

struct spa_bt_device *spa_bt_device_find_by_address(struct spa_bt_monitor *monitor, const char *remote_address, const char *local_address)
{
	struct spa_bt_device *d;
	spa_list_for_each(d, &monitor->device_list, link)
		if (spa_streq(d->address, remote_address) && spa_streq(d->adapter->address, local_address))
			return d;
	return NULL;
}

static uint64_t get_time_now(struct spa_bt_monitor *monitor)
{
	struct timespec ts;

	spa_system_clock_gettime(monitor->main_system, CLOCK_MONOTONIC, &ts);
	return SPA_TIMESPEC_TO_NSEC(&ts);
}

void spa_bt_device_update_last_bluez_action_time(struct spa_bt_device *device)
{
	device->last_bluez_action_time = get_time_now(device->monitor);
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
	d->battery_path = battery_get_name(d->path);
	d->reconnect_profiles = DEFAULT_RECONNECT_PROFILES;
	d->hw_volume_profiles = DEFAULT_HW_VOLUME_PROFILES;

	spa_list_init(&d->remote_endpoint_list);
	spa_list_init(&d->transport_list);
	spa_list_init(&d->codec_switch_list);
	spa_list_init(&d->set_membership_list);

	spa_hook_list_init(&d->listener_list);

	spa_list_prepend(&monitor->device_list, &d->link);

	spa_bt_device_update_last_bluez_action_time(d);

	return d;
}

static void device_clear_sub(struct spa_bt_device *device)
{
	battery_remove(device);
	spa_bt_device_release_transports(device);
}

static void device_free(struct spa_bt_device *device)
{
	struct spa_bt_remote_endpoint *ep, *tep;
	struct spa_bt_media_codec_switch *sw;
	struct spa_bt_transport *t, *tt;
	struct spa_bt_monitor *monitor = device->monitor;
	struct spa_bt_set_membership *s;

	spa_log_debug(monitor->log, "%p", device);

	spa_bt_device_emit_destroy(device);

	device_clear_sub(device);
	device_stop_timer(device);

	if (device->added) {
		spa_device_emit_object_info(&monitor->hooks, device->id, NULL);
	}

	spa_list_for_each_safe(ep, tep, &device->remote_endpoint_list, device_link) {
		if (ep->device == device) {
			spa_list_remove(&ep->device_link);
			ep->device = NULL;
		}
	}

	spa_list_for_each_safe(t, tt, &device->transport_list, device_link) {
		if (t->device == device) {
			spa_list_remove(&t->device_link);
			t->device = NULL;
		}
	}

	spa_list_consume(sw, &device->codec_switch_list, device_link)
		media_codec_switch_free(sw);

	spa_list_consume(s, &device->set_membership_list, link) {
		spa_list_remove(&s->link);
		spa_list_remove(&s->others);
		free(s->path);
		free(s);
	}

	spa_list_remove(&device->link);
	free(device->path);
	free(device->alias);
	free(device->address);
	free(device->adapter_path);
	free(device->battery_path);
	free(device->name);
	free(device->icon);
	free(device);
}

static struct spa_bt_set_membership *device_set_find(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_device *d;

	spa_list_for_each(d, &monitor->device_list, link) {
		struct spa_bt_set_membership *s;

		spa_list_for_each(s, &d->set_membership_list, link) {
			if (spa_streq(s->path, path))
				return s;
		}
	}

	return NULL;
}

static int device_add_device_set(struct spa_bt_device *device, const char *path, uint8_t rank)
{
	struct spa_bt_monitor *monitor = device->monitor;
	struct spa_bt_set_membership *s, *set;

	spa_list_for_each(s, &device->set_membership_list, link) {
		if (spa_streq(s->path, path)) {
			if (rank)
				s->rank = rank;
			return 0;
		}
	}

	s = calloc(1, sizeof(struct spa_bt_set_membership));
	if (s == NULL)
		return -ENOMEM;

	s->path = strdup(path);
	if (!s->path) {
		free(s);
		return -ENOMEM;
	}

	s->device = device;
	s->rank = rank;

	spa_list_init(&s->others);

	/* Join with other set members, if any */
	set = device_set_find(monitor, path);
	if (set)
		spa_list_append(&set->others, &s->others);

	spa_list_append(&device->set_membership_list, &s->link);

	spa_log_debug(monitor->log, "device %p: add %s to device set %s", device,
			device->path, path);

	return 1;
}

static bool device_remove_device_set(struct spa_bt_device *device, const char *path)
{
	struct spa_bt_monitor *monitor = device->monitor;
	struct spa_bt_set_membership *s;

	spa_list_for_each(s, &device->set_membership_list, link) {
		if (spa_streq(s->path, path)) {
			spa_log_debug(monitor->log,
					"device %p: remove %s from device set %s", device,
					device->path, path);
			spa_list_remove(&s->link);
			spa_list_remove(&s->others);
			free(s->path);
			free(s);
			return true;
		}
	}

	return false;
}

int spa_bt_format_vendor_product_id(uint16_t source_id, uint16_t vendor_id, uint16_t product_id,
		char *vendor_str, int vendor_str_size, char *product_str, int product_str_size)
{
	const char *source_str;

	switch (source_id) {
	case SOURCE_ID_USB:
		source_str = "usb";
		break;
	case SOURCE_ID_BLUETOOTH:
		source_str = "bluetooth";
		break;
	default:
		return -EINVAL;
	}

	spa_scnprintf(vendor_str, vendor_str_size, "%s:%04x", source_str, (unsigned int)vendor_id);
	spa_scnprintf(product_str, product_str_size, "%04x", (unsigned int)product_id);
	return 0;
}

static void emit_device_info(struct spa_bt_monitor *monitor,
		struct spa_bt_device *device, bool with_connection)
{
	struct spa_device_object_info info;
	char dev[32], name[128], class[16], vendor_id[64], product_id[64], product_id_tot[67];
	struct spa_dict_item items[23];
	uint32_t n_items = 0;

	info = SPA_DEVICE_OBJECT_INFO_INIT();
	info.type = SPA_TYPE_INTERFACE_Device;
	info.factory_name = SPA_NAME_API_BLUEZ5_DEVICE;
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_FLAGS |
		SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.flags = 0;

	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_API, "bluez5");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_BUS, "bluetooth");
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_MEDIA_CLASS, "Audio/Device");
	snprintf(name, sizeof(name), "bluez_card.%s", device->address);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_NAME, name);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_DESCRIPTION, device->alias);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_ALIAS, device->name);
	if (spa_bt_format_vendor_product_id(
				device->source_id, device->vendor_id, device->product_id,
				vendor_id, sizeof(vendor_id), product_id, sizeof(product_id)) == 0) {
		snprintf(product_id_tot, sizeof(product_id_tot), "0x%s", product_id);
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_VENDOR_ID, vendor_id);
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_PRODUCT_ID, product_id_tot);
	}
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_FORM_FACTOR,
			spa_bt_form_factor_name(
				spa_bt_form_factor_from_class(device->bluetooth_class)));
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_STRING, device->address);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_ICON, device->icon);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_PATH, device->path);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_ADDRESS, device->address);
	snprintf(dev, sizeof(dev), "pointer:%p", device);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_DEVICE, dev);
	snprintf(class, sizeof(class), "0x%06x", device->bluetooth_class);
	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_CLASS, class);

	if (with_connection) {
		items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_CONNECTION,
					device->connected ? "connected": "disconnected");
	}

	info.props = &SPA_DICT_INIT(items, n_items);
	spa_device_emit_object_info(&monitor->hooks, device->id, &info);
}

static int device_connected_old(struct spa_bt_monitor *monitor,
		struct spa_bt_device *device, int connected)
{

	if (connected == BT_DEVICE_INIT)
		return 0;

	device->connected = connected;

	if (device->connected) {
		emit_device_info(monitor, device, false);
		device->added = true;
	} else {
		if (!device->added)
			return 0;

		device_clear_sub(device);
		spa_device_emit_object_info(&monitor->hooks, device->id, NULL);
		device->added = false;
	}

	return 0;
}

enum {
	BT_DEVICE_RECONNECT_INIT = 0,
	BT_DEVICE_RECONNECT_PROFILE,
	BT_DEVICE_RECONNECT_STOP
};

static int device_connected(struct spa_bt_monitor *monitor,
		struct spa_bt_device *device, int status)
{
	bool connected, init = (status == BT_DEVICE_INIT);

	connected = init ? 0 : status;

	if (!init) {
		device->reconnect_state =
			connected ? BT_DEVICE_RECONNECT_STOP
				  : BT_DEVICE_RECONNECT_PROFILE;
	}

	if ((device->connected_profiles != 0) ^ connected) {
		spa_log_error(monitor->log,
			"device %p: unexpected call, connected_profiles:%08x connected:%d",
			device, device->connected_profiles, device->connected);
		return -EINVAL;
	}

	if (!monitor->connection_info_supported)
		return device_connected_old(monitor, device, status);

	if (init) {
		device->connected = connected;
	} else {
		if (!device->added || !(connected ^ device->connected))
			return 0;

		device->connected = connected;
		spa_bt_device_emit_connected(device, device->connected);

		if (!device->connected)
			device_clear_sub(device);
	}

	emit_device_info(monitor, device, true);
	device->added = true;

	return 0;
}

/*
 * Add profile to device based on bluez actions
 * (update property UUIDs, trigger profile handlers),
 * in case UUIDs is empty on signal InterfaceAdded for
 * org.bluez.Device1. And emit device info if there is
 * at least 1 profile on device. This should be called
 * before any device setting accessing.
 */
int spa_bt_device_add_profile(struct spa_bt_device *device, enum spa_bt_profile profile)
{
	struct spa_bt_monitor *monitor = device->monitor;

	if (profile && (device->profiles & profile) == 0) {
		spa_log_info(monitor->log, "device %p: add new profile %08x", device, profile);
		device->profiles |= profile;
	}

	if (!device->added && device->profiles) {
		device_connected(monitor, device, BT_DEVICE_INIT);
		if (device->reconnect_state == BT_DEVICE_RECONNECT_INIT)
			device_start_timer(device);
	}

	return 0;
}


static int device_try_connect_profile(struct spa_bt_device *device,
				      const char *profile_uuid)
{
	struct spa_bt_monitor *monitor = device->monitor;
	spa_autoptr(DBusMessage) m = NULL;

	spa_log_info(monitor->log, "device %p %s: profile %s not connected; try ConnectProfile()",
	             device, device->path, profile_uuid);

	/* Call org.bluez.Device1.ConnectProfile() on device, ignoring result */

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
					 device->path,
					 BLUEZ_DEVICE_INTERFACE,
					 "ConnectProfile");
	if (m == NULL)
		return -ENOMEM;
	dbus_message_append_args(m, DBUS_TYPE_STRING, &profile_uuid, DBUS_TYPE_INVALID);
	if (!dbus_connection_send(monitor->conn, m, NULL))
		return -EIO;

	return 0;
}

static int reconnect_device_profiles(struct spa_bt_device *device)
{
	struct spa_bt_monitor *monitor = device->monitor;
	struct spa_bt_device *d;
	uint32_t reconnect = device->profiles
			& device->reconnect_profiles
			& (device->connected_profiles ^ device->profiles);

	/* Don't try to connect to same device via multiple adapters */
	spa_list_for_each(d, &monitor->device_list, link) {
		if (d != device && spa_streq(d->address, device->address)) {
			if (d->paired && d->trusted && !d->blocked &&
					d->reconnect_state == BT_DEVICE_RECONNECT_STOP)
				reconnect &= ~d->reconnect_profiles;
			if (d->connected_profiles)
				reconnect = 0;
		}
	}

	/* Connect only profiles the adapter has a counterpart for */
	if (device->adapter)
		reconnect &= adapter_connectable_profiles(device->adapter);

	if (!(device->connected_profiles & SPA_BT_PROFILE_HEADSET_HEAD_UNIT)) {
		if (reconnect & SPA_BT_PROFILE_HFP_HF) {
			SPA_FLAG_CLEAR(reconnect, SPA_BT_PROFILE_HSP_HS);
		} else if (reconnect & SPA_BT_PROFILE_HSP_HS) {
			SPA_FLAG_CLEAR(reconnect, SPA_BT_PROFILE_HFP_HF);
		}
	} else
		SPA_FLAG_CLEAR(reconnect, SPA_BT_PROFILE_HEADSET_HEAD_UNIT);

	if (!(device->connected_profiles & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY)) {
		if (reconnect & SPA_BT_PROFILE_HFP_AG)
			SPA_FLAG_CLEAR(reconnect, SPA_BT_PROFILE_HSP_AG);
		else if (reconnect & SPA_BT_PROFILE_HSP_AG)
			SPA_FLAG_CLEAR(reconnect, SPA_BT_PROFILE_HFP_AG);
	} else
		SPA_FLAG_CLEAR(reconnect, SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY);

	if (reconnect & SPA_BT_PROFILE_HFP_HF)
		device_try_connect_profile(device, SPA_BT_UUID_HFP_HF);
	if (reconnect & SPA_BT_PROFILE_HSP_HS)
		device_try_connect_profile(device, SPA_BT_UUID_HSP_HS);
	if (reconnect & SPA_BT_PROFILE_HFP_AG)
		device_try_connect_profile(device, SPA_BT_UUID_HFP_AG);
	if (reconnect & SPA_BT_PROFILE_HSP_AG)
		device_try_connect_profile(device, SPA_BT_UUID_HSP_AG);
	if (reconnect & SPA_BT_PROFILE_A2DP_SINK)
		device_try_connect_profile(device, SPA_BT_UUID_A2DP_SINK);
	if (reconnect & SPA_BT_PROFILE_A2DP_SOURCE)
		device_try_connect_profile(device, SPA_BT_UUID_A2DP_SOURCE);
	if (reconnect & SPA_BT_PROFILE_BAP_SINK)
		device_try_connect_profile(device, SPA_BT_UUID_BAP_SINK);
	if (reconnect & SPA_BT_PROFILE_BAP_SOURCE)
		device_try_connect_profile(device, SPA_BT_UUID_BAP_SOURCE);
	if (reconnect & SPA_BT_PROFILE_BAP_BROADCAST_SINK)
		device_try_connect_profile(device, SPA_BT_UUID_BAP_BROADCAST_SINK);
	if (reconnect & SPA_BT_PROFILE_BAP_BROADCAST_SOURCE)
		device_try_connect_profile(device, SPA_BT_UUID_BAP_BROADCAST_SOURCE);

	return reconnect;
}

#define DEVICE_RECONNECT_TIMEOUT_SEC 2
#define DEVICE_PROFILE_TIMEOUT_SEC 6

static void device_timer_event(struct spa_source *source)
{
	struct spa_bt_device *device = source->data;
	struct spa_bt_monitor *monitor = device->monitor;
	uint64_t exp;

	if (spa_system_timerfd_read(monitor->main_system, source->fd, &exp) < 0)
		spa_log_warn(monitor->log, "error reading timerfd: %s", strerror(errno));

	spa_log_debug(monitor->log, "device %p: timeout %08x %08x",
			device, device->profiles, device->connected_profiles);
	device_stop_timer(device);
	if (BT_DEVICE_RECONNECT_STOP != device->reconnect_state) {
		device->reconnect_state = BT_DEVICE_RECONNECT_STOP;
		if (device->paired
			&& device->trusted
			&& !device->blocked
			&& device->reconnect_profiles != 0
			&& reconnect_device_profiles(device))
		{
			device_start_timer(device);
			return;
		}
	}
	if (device->connected_profiles)
		device_connected(device->monitor, device, BT_DEVICE_CONNECTED);
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
	ts.it_value.tv_sec = device->reconnect_state == BT_DEVICE_RECONNECT_STOP
				? DEVICE_PROFILE_TIMEOUT_SEC
				: DEVICE_RECONNECT_TIMEOUT_SEC;
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

int spa_bt_device_check_profiles(struct spa_bt_device *device, bool force)
{
	struct spa_bt_monitor *monitor = device->monitor;
	struct spa_bt_set_membership *s, *set;
	uint32_t connected_profiles = device->connected_profiles;
	uint32_t connectable_profiles =
		device->adapter ? adapter_connectable_profiles(device->adapter) : 0;
	uint32_t direction_masks[3] = {
		SPA_BT_PROFILE_MEDIA_SINK | SPA_BT_PROFILE_HEADSET_HEAD_UNIT,
		SPA_BT_PROFILE_MEDIA_SOURCE,
		SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY,
	};
	bool direction_connected = false;
	bool set_connected = true;
	bool all_connected;
	size_t i;

	if (connected_profiles & SPA_BT_PROFILE_HEADSET_HEAD_UNIT)
		connected_profiles |= SPA_BT_PROFILE_HEADSET_HEAD_UNIT;
	if (connected_profiles & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY)
		connected_profiles |= SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY;

	for (i = 0; i < SPA_N_ELEMENTS(direction_masks); ++i) {
		uint32_t mask = direction_masks[i] & device->profiles & connectable_profiles;
		if (mask && (connected_profiles & mask) == mask)
			direction_connected = true;
	}

	all_connected = (device->profiles & connected_profiles) == device->profiles;

	spa_list_for_each(set, &device->set_membership_list, link)
		spa_bt_for_each_set_member(s, set)
			if ((s->device->connected_profiles & s->device->profiles) != s->device->profiles)
				set_connected = false;

	spa_log_debug(monitor->log, "device %p: profiles %08x %08x connectable:%08x added:%d all:%d dir:%d set:%d",
			device, device->profiles, connected_profiles, connectable_profiles,
			device->added, all_connected, direction_connected, set_connected);

	if (connected_profiles == 0 && spa_list_is_empty(&device->codec_switch_list)) {
		device_stop_timer(device);
		device_connected(monitor, device, BT_DEVICE_DISCONNECTED);
	} else if (force || ((direction_connected || all_connected) && set_connected)) {
		device_stop_timer(device);
		device_connected(monitor, device, BT_DEVICE_CONNECTED);
	} else {
		/* The initial reconnect event has not been triggered,
		 * the connecting is triggered by bluez. */
		if (device->reconnect_state == BT_DEVICE_RECONNECT_INIT)
			device->reconnect_state = BT_DEVICE_RECONNECT_PROFILE;
		device_start_timer(device);
	}
	return 0;
}

static void device_set_connected(struct spa_bt_device *device, int connected)
{
	struct spa_bt_monitor *monitor = device->monitor;

	if (device->connected && !connected)
		device->connected_profiles = 0;

	if (connected)
		spa_bt_device_check_profiles(device, false);
	else {
		/* Stop codec switch on disconnect */
		struct spa_bt_media_codec_switch *sw;
		spa_list_consume(sw, &device->codec_switch_list, device_link)
			media_codec_switch_free(sw);

		if (device->reconnect_state != BT_DEVICE_RECONNECT_INIT)
			device_stop_timer(device);
		device_connected(monitor, device, BT_DEVICE_DISCONNECTED);
	}
}

static void device_update_set_status(struct spa_bt_device *device, bool force, const char *path);

int spa_bt_device_connect_profile(struct spa_bt_device *device, enum spa_bt_profile profile)
{
	uint32_t prev_connected = device->connected_profiles;
	device->connected_profiles |= profile;
	if ((prev_connected ^ device->connected_profiles) & SPA_BT_PROFILE_BAP_DUPLEX)
		device_update_set_status(device, true, NULL);
	spa_bt_device_check_profiles(device, false);
	if (device->connected_profiles != prev_connected)
		spa_bt_device_emit_profiles_changed(device, device->profiles, prev_connected);
	return 0;
}

static void device_update_hw_volume_profiles(struct spa_bt_device *device)
{
	struct spa_bt_monitor *monitor = device->monitor;
	uint32_t bt_features = 0;

	if (!monitor->quirks)
		return;

	if (spa_bt_quirks_get_features(monitor->quirks, device->adapter, device, &bt_features) != 0)
		return;

	if (!(bt_features & SPA_BT_FEATURE_HW_VOLUME))
		device->hw_volume_profiles = 0;

	spa_log_debug(monitor->log, "hw-volume-profiles:%08x", (int)device->hw_volume_profiles);
}

static bool device_set_update_leader(struct spa_bt_set_membership *set)
{
	struct spa_bt_set_membership *s, *leader = NULL;

	/* Make minimum rank device the leader, so that device set nodes always
	 * appear under a specific device.
	 */
	spa_bt_for_each_set_member(s, set) {
		if (!(s->device->connected_profiles & SPA_BT_PROFILE_BAP_DUPLEX))
			continue;

		if (leader == NULL || s->rank < leader->rank ||
				(s->rank == leader->rank && s->leader))
			leader = s;
	}

	if (leader == NULL || (leader && leader->leader))
		return false;

	spa_bt_for_each_set_member(s, set)
		s->leader = false;

	leader->leader = true;

	spa_log_debug(leader->device->monitor->log,
			"device set %p %s: leader is %s",
			set, leader->path, leader->device->path);

	return true;
}

static void device_update_set_status(struct spa_bt_device *device, bool force, const char *path)
{
	struct spa_bt_set_membership *s, *set;

	spa_list_for_each(set, &device->set_membership_list, link) {
		if (path && !spa_streq(set->path, path))
			continue;

		if (device_set_update_leader(set) || force) {
			spa_bt_for_each_set_member(s, set)
				if (!s->leader)
					spa_bt_device_emit_device_set_changed(s->device);
			spa_bt_for_each_set_member(s, set)
				if (s->leader)
					spa_bt_device_emit_device_set_changed(s->device);
		}
	}
}

static int device_set_update_props(struct spa_bt_monitor *monitor,
		const char *path, DBusMessageIter *props_iter, DBusMessageIter *invalidated_iter)
{
	struct spa_bt_device *old[256];
	struct spa_bt_device *new[256];
	struct spa_bt_set_membership *set;
	size_t num_old = 0, num_new = 0;
	size_t i;

	if (!props_iter)
		goto done;

	/* Find current devices */
	while (dbus_message_iter_get_arg_type(props_iter) != DBUS_TYPE_INVALID) {
		DBusMessageIter it[2];
		const char *key;

		dbus_message_iter_recurse(props_iter, &it[0]);
		dbus_message_iter_get_basic(&it[0], &key);
		dbus_message_iter_next(&it[0]);
		dbus_message_iter_recurse(&it[0], &it[1]);

		if (spa_streq(key, "Devices")) {
			DBusMessageIter iter;
			int i = 0;

			if (!check_iter_signature(&it[1], "ao"))
				goto next;

			dbus_message_iter_recurse(&it[1], &iter);

			while (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INVALID) {
				struct spa_bt_device *d;
				const char *dev_path;

				dbus_message_iter_get_basic(&iter, &dev_path);

				spa_log_debug(monitor->log, "device set %s: Devices[%d]=%s",
						path, i++, dev_path);

				if (num_new >= SPA_N_ELEMENTS(new))
					break;
				d = spa_bt_device_find(monitor, dev_path);
				if (d)
					new[num_new++] = d;

				dbus_message_iter_next(&iter);
			}

		}
		else
			spa_log_debug(monitor->log, "device set %s: unhandled key %s",
					path, key);

next:
		dbus_message_iter_next(props_iter);
	}

done:
	/* Find devices to remove */
	set = device_set_find(monitor, path);
	if (set) {
		struct spa_bt_set_membership *s;

		spa_bt_for_each_set_member(s, set) {
			for (i = 0; i < num_new; ++i)
				if (s->device == new[i])
					break;
			if (i == num_new) {
				if (num_old >= SPA_N_ELEMENTS(old))
					break;
				old[num_old++] = s->device;
			}
		}
	}

	/* Remove old devices */
	for (i = 0; i < num_old; ++i)
		device_remove_device_set(old[i], path);

	/* Add new devices */
	for (i = 0; i < num_new; ++i)
		device_add_device_set(new[i], path, 0);

	/* Emit signals & update set leader */
	for (i = 0; i < num_old; ++i)
		spa_bt_device_emit_device_set_changed(old[i]);

	if (num_new > 0)
		device_update_set_status(new[0], true, path);

	return 0;
}

static int device_update_device_sets_prop(struct spa_bt_device *device,
		DBusMessageIter *iter)
{
	struct spa_bt_monitor *monitor = device->monitor;
	DBusMessageIter it[5];
	bool changed = false;

	if (!check_iter_signature(iter, "a{oa{sv}}"))
		return -EINVAL;

	dbus_message_iter_recurse(iter, &it[0]);

	while (dbus_message_iter_get_arg_type(&it[0]) != DBUS_TYPE_INVALID) {
		uint8_t rank = 0;
		const char *set_path;

		dbus_message_iter_recurse(&it[0], &it[1]);
		dbus_message_iter_get_basic(&it[1], &set_path);
		dbus_message_iter_next(&it[1]);
		dbus_message_iter_recurse(&it[1], &it[2]);

		while (dbus_message_iter_get_arg_type(&it[2]) != DBUS_TYPE_INVALID) {
			const char *key;
			int type;

			dbus_message_iter_recurse(&it[2], &it[3]);
			dbus_message_iter_get_basic(&it[3], &key);
			dbus_message_iter_next(&it[3]);
			dbus_message_iter_recurse(&it[3], &it[4]);

			type = dbus_message_iter_get_arg_type(&it[4]);

			if (spa_streq(key, "Rank") && type == DBUS_TYPE_BYTE)
				dbus_message_iter_get_basic(&it[4], &rank);

			dbus_message_iter_next(&it[2]);
		}

		spa_log_debug(monitor->log, "device %p: path %s device set %s rank %d",
				device, device->path, set_path, (int)rank);

		/* Only add. Removals are handled in device set updates. */
		if (device_add_device_set(device, set_path, rank) == 1)
			changed = true;

		dbus_message_iter_next(&it[0]);
	}

	/* Emit change signals */
	device_update_set_status(device, changed, NULL);

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

			if (spa_streq(key, "Alias")) {
				free(device->alias);
				device->alias = strdup(value);
			}
			else if (spa_streq(key, "Name")) {
				free(device->name);
				device->name = strdup(value);
			}
			else if (spa_streq(key, "Address")) {
				free(device->address);
				device->address = strdup(value);
			}
			else if (spa_streq(key, "Adapter")) {
				free(device->adapter_path);
				device->adapter_path = strdup(value);

				device->adapter = adapter_find(monitor, value);
				if (device->adapter == NULL) {
					spa_log_info(monitor->log, "unknown adapter %s", value);
				}
			}
			else if (spa_streq(key, "Icon")) {
				free(device->icon);
				device->icon = strdup(value);
			}
			else if (spa_streq(key, "Modalias")) {
				int ret;
				ret = parse_modalias(value, &device->source_id, &device->vendor_id,
						&device->product_id, &device->version_id);
				if (ret < 0)
					spa_log_debug(monitor->log, "device %p: %s=%s ignored: %s",
							device, key, value, spa_strerror(ret));
			}
		}
		else if (type == DBUS_TYPE_UINT32) {
			uint32_t value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "device %p: %s=%08x", device, key, value);

			if (spa_streq(key, "Class"))
				device->bluetooth_class = value;
		}
		else if (type == DBUS_TYPE_UINT16) {
			uint16_t value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "device %p: %s=%d", device, key, value);

			if (spa_streq(key, "Appearance"))
				device->appearance = value;
		}
		else if (type == DBUS_TYPE_INT16) {
			int16_t value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "device %p: %s=%d", device, key, value);

			if (spa_streq(key, "RSSI"))
				device->RSSI = value;
		}
		else if (type == DBUS_TYPE_BOOLEAN) {
			int value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "device %p: %s=%d", device, key, value);

			if (spa_streq(key, "Paired")) {
				device->paired = value;
			}
			else if (spa_streq(key, "Trusted")) {
				device->trusted = value;
			}
			else if (spa_streq(key, "Connected")) {
				device_set_connected(device, value);
			}
			else if (spa_streq(key, "Blocked")) {
				device->blocked = value;
			}
			else if (spa_streq(key, "ServicesResolved")) {
				if (value)
					spa_bt_device_check_profiles(device, false);
			}
		}
		else if (spa_streq(key, "UUIDs")) {
			DBusMessageIter iter;
			uint32_t prev_profiles = device->profiles;

			if (!check_iter_signature(&it[1], "as"))
				goto next;

			dbus_message_iter_recurse(&it[1], &iter);

			while (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INVALID) {
				const char *uuid;
				enum spa_bt_profile profile;

				dbus_message_iter_get_basic(&iter, &uuid);

				profile = spa_bt_profile_from_uuid(uuid);

				/* Only add A2DP/BAP profiles if HSP/HFP backed is none.
				 * This allows BT device to connect instantly instead of waiting for
				 * profile timeout, because all available profiles are connected.
				 */
				if (monitor->backend_selection != BACKEND_NONE || (monitor->backend_selection == BACKEND_NONE &&
						profile & (SPA_BT_PROFILE_MEDIA_SINK | SPA_BT_PROFILE_MEDIA_SOURCE))) {
					if (profile && (device->profiles & profile) == 0) {
						spa_log_debug(monitor->log, "device %p: add UUID=%s", device, uuid);
						device->profiles |= profile;
					}
				}
				dbus_message_iter_next(&iter);
			}

			if (device->profiles != prev_profiles)
				spa_bt_device_emit_profiles_changed(
					device, prev_profiles, device->connected_profiles);
		}
		else if (spa_streq(key, "Sets")) {
			device_update_device_sets_prop(device, &it[1]);
		}
		else
			spa_log_debug(monitor->log, "device %p: unhandled key %s type %d", device, key, type);

next:
		dbus_message_iter_next(props_iter);
	}
	return 0;
}

static bool device_props_ready(struct spa_bt_device *device)
{
	/*
	 * In some cases, BlueZ device props may be missing part of
	 * the information required when the interface first appears.
	 */
	return device->adapter && device->address;
}

bool spa_bt_device_supports_media_codec(struct spa_bt_device *device, const struct media_codec *codec, enum spa_bt_profile profile)
{
	struct spa_bt_monitor *monitor = device->monitor;
	struct spa_bt_remote_endpoint *ep;
	enum spa_bt_profile codec_target_profile;
	struct spa_bt_transport *t;
	const struct { enum spa_bluetooth_audio_codec codec; uint32_t mask; } quirks[] = {
		{ SPA_BLUETOOTH_AUDIO_CODEC_SBC_XQ, SPA_BT_FEATURE_SBC_XQ },
		{ SPA_BLUETOOTH_AUDIO_CODEC_FASTSTREAM, SPA_BT_FEATURE_FASTSTREAM },
		{ SPA_BLUETOOTH_AUDIO_CODEC_FASTSTREAM_DUPLEX, SPA_BT_FEATURE_FASTSTREAM },
		{ SPA_BLUETOOTH_AUDIO_CODEC_APTX_LL_DUPLEX, SPA_BT_FEATURE_A2DP_DUPLEX },
		{ SPA_BLUETOOTH_AUDIO_CODEC_FASTSTREAM_DUPLEX, SPA_BT_FEATURE_A2DP_DUPLEX },
	};
	size_t i;

	if (!is_media_codec_enabled(device->monitor, codec))
		return false;

	if (!device->adapter->a2dp_application_registered && !codec->bap) {
		/* Codec switching not supported: only plain SBC allowed */
		return (codec->codec_id == A2DP_CODEC_SBC && spa_streq(codec->name, "sbc") &&
				device->adapter->legacy_endpoints_registered);
	}
	if (!device->adapter->bap_application_registered && codec->bap)
		return false;

	/* Check codec quirks */
	for (i = 0; i < SPA_N_ELEMENTS(quirks); ++i) {
		uint32_t bt_features;

		if (codec->id != quirks[i].codec)
			continue;
		if (monitor->quirks == NULL)
			break;
		if (spa_bt_quirks_get_features(monitor->quirks, device->adapter, device, &bt_features) < 0)
			break;
		if (!(bt_features & quirks[i].mask))
			return false;
	}

	for (i = 0, codec_target_profile = 0; i < (size_t)SPA_BT_MEDIA_DIRECTION_LAST; ++i)
		if (codec_has_direction(codec, i))
			codec_target_profile |= swap_profile(get_codec_profile(codec, i));

	spa_list_for_each(ep, &device->remote_endpoint_list, device_link) {
		enum spa_bt_profile ep_profile = spa_bt_profile_from_uuid(ep->uuid);

		if (!(ep_profile & codec_target_profile & profile))
			continue;

		if (media_codec_check_caps(codec, ep->codec, ep->capabilities, ep->capabilities_len,
						&ep->monitor->default_audio_info, &monitor->global_settings))
			return true;
	}

	/* Codecs on configured transports are always supported.
	 *
	 * Remote BAP endpoints correspond to capabilities of the remote
	 * BAP Server, not to remote BAP Client, and need not be the same.
	 * BAP Clients may not have any remote endpoints. In this case we
	 * can only know that the currently configured codec is supported.
	 */
	spa_list_for_each(t, &device->transport_list, device_link) {
		if (!(t->profile & codec_target_profile & profile))
			continue;

		if (codec == t->media_codec)
			return true;
	}

	return false;
}

const struct media_codec **spa_bt_device_get_supported_media_codecs(struct spa_bt_device *device, size_t *count)
{
	struct spa_bt_monitor *monitor = device->monitor;
	const struct media_codec * const * const media_codecs = monitor->media_codecs;
	spa_autofree const struct media_codec **supported_codecs = NULL;
	size_t i, j, size;

	*count = 0;
	size = 8;
	supported_codecs = malloc(size * sizeof(const struct media_codec *));
	if (supported_codecs == NULL)
		return NULL;

	j = 0;
	for (i = 0; media_codecs[i] != NULL; ++i) {
		if (spa_bt_device_supports_media_codec(device, media_codecs[i], device->connected_profiles)) {
			supported_codecs[j] = media_codecs[i];
			++j;
		}

		if (j >= size) {
			const struct media_codec **p;
			size = size * 2;
#ifdef HAVE_REALLOCARRAY
			p = reallocarray(supported_codecs, size, sizeof(const struct media_codec *));
#else
			p = realloc(supported_codecs, size * sizeof(const struct media_codec *));
#endif
			if (p == NULL)
				return NULL;

			supported_codecs = p;
		}
	}

	supported_codecs[j] = NULL;
	*count = j;

	return spa_steal_ptr(supported_codecs);
}

static struct spa_bt_remote_endpoint *device_remote_endpoint_find(struct spa_bt_device *device, const char *path)
{
	struct spa_bt_remote_endpoint *ep;
	spa_list_for_each(ep, &device->remote_endpoint_list, device_link)
		if (spa_streq(ep->path, path))
			return ep;
	return NULL;
}

static struct spa_bt_remote_endpoint *remote_endpoint_find(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_remote_endpoint *ep;
	spa_list_for_each(ep, &monitor->remote_endpoint_list, link)
		if (spa_streq(ep->path, path))
			return ep;
	return NULL;
}

static struct spa_bt_device *create_bcast_device(struct spa_bt_monitor *monitor, const char *object_path)
{	
	struct spa_bt_device *d;
	struct spa_bt_adapter *adapter;

	adapter = adapter_find(monitor, object_path);
	if (adapter == NULL) {
		spa_log_warn(monitor->log, "unknown adapter %s", object_path);
		return NULL;
	}

	d = device_create(monitor, object_path);
	if (d == NULL) {
		spa_log_warn(monitor->log, "can't create Bluetooth device %s: %m",
				object_path);
		return NULL;
	}

	d->adapter = adapter;
	d->adapter_path = strdup(adapter->path);
	d->alias = strdup(adapter->alias);
	d->name = strdup(adapter->name);
	d->address = strdup("00:00:00:00:00:00");
	d->reconnect_state = BT_DEVICE_RECONNECT_STOP;

	device_update_hw_volume_profiles(d);

	spa_bt_device_add_profile(d, SPA_BT_PROFILE_NULL);

	return d;
}

static int remote_endpoint_update_props(struct spa_bt_remote_endpoint *remote_endpoint,
				DBusMessageIter *props_iter,
				DBusMessageIter *invalidated_iter)
{
	struct spa_bt_monitor *monitor = remote_endpoint->monitor;

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

			spa_log_debug(monitor->log, "remote_endpoint %p: %s=%s", remote_endpoint, key, value);

			if (spa_streq(key, "UUID")) {
				free(remote_endpoint->uuid);
				remote_endpoint->uuid = strdup(value);
			}
			else if (spa_streq(key, "Device")) {
				struct spa_bt_device *device;

				device = spa_bt_device_find(monitor, value);
				if (device == NULL) {
					/*
					* If a broadcast sink endpoint is detected (over DBus) a new device
					* will be created.  This device will be our simulated remote device. 
					* This is done because BlueZ sets the adapter as the device
					* that is connected to for a broadcast sink endpoint/transport.
					*/
					if (spa_streq(remote_endpoint->uuid, SPA_BT_UUID_BAP_BROADCAST_SINK)) {
						device = create_bcast_device(monitor, value);
						if (device == NULL)
							goto next;

						remote_endpoint->acceptor = true;
						device_set_connected(device, 1);
					} else {
						goto next;
					}
				}

				spa_log_debug(monitor->log, "remote_endpoint %p: device -> %p", remote_endpoint, device);

				if (remote_endpoint->device != device) {
					if (remote_endpoint->device != NULL)
						spa_list_remove(&remote_endpoint->device_link);
					remote_endpoint->device = device;
					if (device != NULL)
						spa_list_append(&device->remote_endpoint_list, &remote_endpoint->device_link);
				}
			}
		}
		else if (type == DBUS_TYPE_BOOLEAN) {
			int value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "remote_endpoint %p: %s=%d", remote_endpoint, key, value);

			if (spa_streq(key, "DelayReporting")) {
				remote_endpoint->delay_reporting = value;
			}
		}
		else if (type == DBUS_TYPE_BYTE) {
			uint8_t value;

			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "remote_endpoint %p: %s=%02x", remote_endpoint, key, value);

			if (spa_streq(key, "Codec")) {
				remote_endpoint->codec = value;
			}
		}
		else if (spa_streq(key, "Capabilities")) {
			DBusMessageIter iter;
			uint8_t *value;
			int len;

			if (!check_iter_signature(&it[1], "ay"))
				goto next;

			dbus_message_iter_recurse(&it[1], &iter);
			dbus_message_iter_get_fixed_array(&iter, &value, &len);

			spa_log_debug(monitor->log, "remote_endpoint %p: %s=%d", remote_endpoint, key, len);
			spa_debug_log_mem(monitor->log, SPA_LOG_LEVEL_DEBUG, 2, value, (size_t)len);

			free(remote_endpoint->capabilities);
			remote_endpoint->capabilities_len = 0;

			remote_endpoint->capabilities = malloc(len);
			if (remote_endpoint->capabilities) {
				memcpy(remote_endpoint->capabilities, value, len);
				remote_endpoint->capabilities_len = len;
			}
		}
		else
			spa_log_debug(monitor->log, "remote_endpoint %p: unhandled key %s", remote_endpoint, key);

next:
		dbus_message_iter_next(props_iter);
	}
	return 0;
}

static struct spa_bt_remote_endpoint *remote_endpoint_create(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_remote_endpoint *ep;

	ep = calloc(1, sizeof(struct spa_bt_remote_endpoint));
	if (ep == NULL)
		return NULL;

	ep->monitor = monitor;
	ep->path = strdup(path);

	spa_list_prepend(&monitor->remote_endpoint_list, &ep->link);

	return ep;
}

static void remote_endpoint_free(struct spa_bt_remote_endpoint *remote_endpoint)
{
	struct spa_bt_monitor *monitor = remote_endpoint->monitor;

	spa_log_debug(monitor->log, "remote endpoint %p: free %s",
	              remote_endpoint, remote_endpoint->path);

	if (remote_endpoint->device)
		spa_list_remove(&remote_endpoint->device_link);

	spa_list_remove(&remote_endpoint->link);
	free(remote_endpoint->path);
	free(remote_endpoint->uuid);
	free(remote_endpoint->capabilities);
	free(remote_endpoint);
}

struct spa_bt_transport *spa_bt_transport_find(struct spa_bt_monitor *monitor, const char *path)
{
	struct spa_bt_transport *t;
	spa_list_for_each(t, &monitor->transport_list, link)
		if (spa_streq(t->path, path))
			return t;
	return NULL;
}

struct spa_bt_transport *spa_bt_transport_find_full(struct spa_bt_monitor *monitor,
						    bool (*callback) (struct spa_bt_transport *t, const void *data),
						    const void *data)
{
	struct spa_bt_transport *t;

	spa_list_for_each(t, &monitor->transport_list, link)
		if (callback(t, data) == true)
			return t;
	return NULL;
}


struct spa_bt_transport *spa_bt_transport_create(struct spa_bt_monitor *monitor, char *path, size_t extra)
{
	struct spa_bt_transport *t;

	t = calloc(1, sizeof(struct spa_bt_transport) + extra);
	if (t == NULL)
		return NULL;

	t->acquire_refcount = 0;
	t->monitor = monitor;
	t->path = path;
	t->fd = -1;
	t->sco_io = NULL;
	t->delay_us = SPA_BT_UNKNOWN_DELAY;
	t->latency_us = SPA_BT_UNKNOWN_DELAY;
	t->bap_cig = 0xff;
	t->bap_cis = 0xff;
	t->bap_big = 0xff;
	t->bap_bis = 0xff;
	t->user_data = SPA_PTROFF(t, sizeof(struct spa_bt_transport), void);
	spa_hook_list_init(&t->listener_list);
	spa_list_init(&t->bap_transport_linked);

	spa_list_append(&monitor->transport_list, &t->link);

	return t;
}

bool spa_bt_transport_volume_enabled(struct spa_bt_transport *transport)
{
	return transport->device != NULL
		&& (transport->device->hw_volume_profiles & transport->profile);
}

static void transport_sync_volume(struct spa_bt_transport *transport)
{
	if (!spa_bt_transport_volume_enabled(transport))
		return;

	for (int i = 0; i < SPA_BT_VOLUME_ID_TERM; ++i)
		spa_bt_transport_set_volume(transport, i, transport->volumes[i].volume);
	spa_bt_transport_emit_volume_changed(transport);
}

void spa_bt_transport_set_state(struct spa_bt_transport *transport, enum spa_bt_transport_state state)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	enum spa_bt_transport_state old = transport->state;

	if (old != state) {
		transport->state = state;
		spa_log_debug(monitor->log, "transport %p: %s state changed %d -> %d",
				transport, transport->path, old, state);
		spa_bt_transport_emit_state_changed(transport, old, state);
		if (state >= SPA_BT_TRANSPORT_STATE_PENDING && old < SPA_BT_TRANSPORT_STATE_PENDING)
			transport_sync_volume(transport);

		if (state < SPA_BT_TRANSPORT_STATE_ACTIVE) {
			/* If transport becomes inactive, do any pending releases
			 * immediately, since the fd is not usable any more.
			 */
			spa_bt_transport_commit_release_timer(transport);
		}

		if (state == SPA_BT_TRANSPORT_STATE_ERROR) {
			uint64_t now = get_time_now(monitor);

			if (now > transport->last_error_time + TRANSPORT_ERROR_TIMEOUT) {
				spa_log_error(monitor->log, "Failure in Bluetooth audio transport %s",
						transport->path);
			}

			transport->last_error_time = now;
			++transport->error_count;
		}
	}
}

void spa_bt_transport_free(struct spa_bt_transport *transport)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	struct spa_bt_device *device = transport->device;
	uint32_t prev_connected = 0;

	spa_log_debug(monitor->log, "transport %p: free %s", transport, transport->path);

	spa_bt_transport_set_state(transport, SPA_BT_TRANSPORT_STATE_IDLE);

	spa_bt_transport_keepalive(transport, false);

	spa_bt_transport_emit_destroy(transport);

	spa_bt_transport_stop_volume_timer(transport);
	spa_bt_transport_stop_release_timer(transport);

	if (transport->sco_io) {
		spa_bt_sco_io_destroy(transport->sco_io);
		transport->sco_io = NULL;
	}

	if (transport->iso_io)
		spa_bt_iso_io_destroy(transport->iso_io);

	spa_bt_transport_destroy(transport);

	cancel_and_unref(&transport->acquire_call);
	cancel_and_unref(&transport->volume_call);

	if (transport->fd >= 0) {
		spa_bt_player_set_state(transport->device->adapter->dummy_player, SPA_BT_PLAYER_STOPPED);

		shutdown(transport->fd, SHUT_RDWR);
		close(transport->fd);
		transport->fd = -1;
	}

	spa_list_remove(&transport->link);
	if (transport->device) {
		prev_connected = transport->device->connected_profiles;
		transport->device->connected_profiles &= ~transport->profile;
		spa_list_remove(&transport->device_link);
	}

	if (device && device->connected_profiles != prev_connected) {
		if ((prev_connected ^ device->connected_profiles) & SPA_BT_PROFILE_BAP_DUPLEX)
			device_update_set_status(device, true, NULL);
		spa_bt_device_emit_profiles_changed(device, device->profiles, prev_connected);
	}

	spa_list_remove(&transport->bap_transport_linked);

	free(transport->configuration);
	free(transport->endpoint_path);
	free(transport->path);
	free(transport);
}

int spa_bt_transport_keepalive(struct spa_bt_transport *t, bool keepalive)
{
	if (keepalive) {
		t->keepalive = true;
		return 0;
	}

	t->keepalive = false;

	if (t->acquire_refcount == 0 && t->acquired) {
		t->acquire_refcount = 1;
		return spa_bt_transport_release(t);
	}

	return 0;
}

int spa_bt_transport_acquire(struct spa_bt_transport *transport, bool optional)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	int res;

	if (transport->acquire_refcount > 0) {
		spa_log_debug(monitor->log, "transport %p: incref %s", transport, transport->path);
		transport->acquire_refcount += 1;
		spa_bt_transport_emit_state_changed(transport, transport->state, transport->state);
		return 0;
	}
	spa_assert(transport->acquire_refcount == 0);

	/* If we are getting into error state too often, stop trying */
	if (get_time_now(monitor) > transport->last_error_time + TRANSPORT_ERROR_TIMEOUT)
		transport->error_count = 0;
	if (transport->error_count >= TRANSPORT_ERROR_MAX_RETRY)
		return -EIO;

	if (!transport->acquired)
		res = spa_bt_transport_impl(transport, acquire, 0, optional);
	else
		res = 0;

	if (res >= 0) {
		transport->acquire_refcount = 1;
		transport->acquired = true;
	}

	return res;
}

static void spa_bt_transport_do_release(struct spa_bt_transport *transport)
{
	struct spa_bt_monitor *monitor = transport->monitor;

	spa_assert(transport->acquire_refcount >= 1);
	spa_assert(transport->acquired);

	if (transport->acquire_refcount == 1) {
		if (!transport->keepalive) {
			spa_bt_transport_impl(transport, release, 0);
			transport->acquired = false;
		} else {
			spa_log_debug(monitor->log, "transport %p: keepalive %s on release",
					transport, transport->path);
		}
	} else {
		spa_log_debug(monitor->log, "transport %p: delayed decref %s", transport, transport->path);
	}
	transport->acquire_refcount -= 1;
}

int spa_bt_transport_release(struct spa_bt_transport *transport)
{
	struct spa_bt_monitor *monitor = transport->monitor;

	if (transport->acquire_refcount > 1) {
		spa_log_debug(monitor->log, "transport %p: decref %s", transport, transport->path);
		transport->acquire_refcount -= 1;
		spa_bt_transport_emit_state_changed(transport, transport->state, transport->state);
		return 0;
	}
	else if (transport->acquire_refcount == 0) {
		spa_log_info(monitor->log, "transport %s already released", transport->path);
		return 0;
	}
	spa_assert(transport->acquire_refcount == 1);
	spa_assert(transport->acquired);

	/* Postpone active transport releases, since we might need it again soon.
	 * If not active, release now since it has to be reacquired before using again.
	 */
	if (transport->state == SPA_BT_TRANSPORT_STATE_ACTIVE &&
			!SPA_BT_TRANSPORT_IS_A2DP(transport)) {
		return spa_bt_transport_start_release_timer(transport);
	} else {
		spa_bt_transport_do_release(transport);
		return 0;
	}
}

static int spa_bt_transport_release_now(struct spa_bt_transport *transport)
{
	int res;

	if (!transport->acquired)
		return 0;

	spa_bt_transport_stop_release_timer(transport);
	res = spa_bt_transport_impl(transport, release, 0);
	if (res >= 0) {
		transport->acquire_refcount = 0;
		transport->acquired = false;
	}

	return res;
}

int spa_bt_device_release_transports(struct spa_bt_device *device)
{
	struct spa_bt_transport *t;
	spa_list_for_each(t, &device->transport_list, device_link)
		spa_bt_transport_release_now(t);
	return 0;
}

static int start_timeout_timer(struct spa_bt_monitor *monitor,
		struct spa_source *timer, spa_source_func_t timer_event,
		time_t timeout_msec, void *data)
{
	struct itimerspec ts;
	if (timer->data == NULL) {
		timer->data = data;
		timer->func = timer_event;
		timer->fd = spa_system_timerfd_create(
			monitor->main_system, CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
		timer->mask = SPA_IO_IN;
		timer->rmask = 0;
		spa_loop_add_source(monitor->main_loop, timer);
	}
	ts.it_value.tv_sec = timeout_msec / SPA_MSEC_PER_SEC;
	ts.it_value.tv_nsec = (timeout_msec % SPA_MSEC_PER_SEC) * SPA_NSEC_PER_MSEC;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(monitor->main_system, timer->fd, 0, &ts, NULL);
	return 0;
}

static int stop_timeout_timer(struct spa_bt_monitor *monitor, struct spa_source *timer)
{
	struct itimerspec ts;

	if (timer->data == NULL)
		return 0;

	spa_loop_remove_source(monitor->main_loop, timer);
	ts.it_value.tv_sec = 0;
	ts.it_value.tv_nsec = 0;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(monitor->main_system, timer->fd, 0, &ts, NULL);
	spa_system_close(monitor->main_system, timer->fd);
	timer->data = NULL;
	return 0;
}

static void spa_bt_transport_release_timer_event(struct spa_source *source)
{
	struct spa_bt_transport *transport = source->data;

	spa_bt_transport_stop_release_timer(transport);
	spa_bt_transport_do_release(transport);
}

static int spa_bt_transport_start_release_timer(struct spa_bt_transport *transport)
{
	return start_timeout_timer(transport->monitor,
		&transport->release_timer,
		spa_bt_transport_release_timer_event,
		TRANSPORT_RELEASE_TIMEOUT_MSEC, transport);
}

static int spa_bt_transport_stop_release_timer(struct spa_bt_transport *transport)
{
	return stop_timeout_timer(transport->monitor, &transport->release_timer);
}

static void spa_bt_transport_commit_release_timer(struct spa_bt_transport *transport)
{
	struct spa_bt_monitor *monitor = transport->monitor;

	/* Do release now if it is pending */
	if (transport->release_timer.data) {
		spa_log_debug(monitor->log, "transport %p: commit pending release", transport);
		spa_bt_transport_release_timer_event(&transport->release_timer);
	}
}

static void spa_bt_transport_volume_changed(struct spa_bt_transport *transport)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	struct spa_bt_transport_volume * t_volume;
	int volume_id;

	if (transport->profile & SPA_BT_PROFILE_A2DP_SINK)
		volume_id = SPA_BT_VOLUME_ID_TX;
	else if (transport->profile & SPA_BT_PROFILE_A2DP_SOURCE)
		volume_id = SPA_BT_VOLUME_ID_RX;
	else
		return;

	t_volume = &transport->volumes[volume_id];

	if (t_volume->hw_volume != t_volume->new_hw_volume) {
		t_volume->hw_volume = t_volume->new_hw_volume;
		t_volume->volume = spa_bt_volume_hw_to_linear(t_volume->hw_volume,
					t_volume->hw_volume_max);
		spa_log_debug(monitor->log, "transport %p: volume changed %d(%f) ",
			transport, t_volume->new_hw_volume, t_volume->volume);
		if (spa_bt_transport_volume_enabled(transport)) {
			transport->device->a2dp_volume_active[volume_id] = true;
			spa_bt_transport_emit_volume_changed(transport);
		}
	}
}

static void spa_bt_transport_volume_timer_event(struct spa_source *source)
{
	struct spa_bt_transport *transport = source->data;
	struct spa_bt_monitor *monitor = transport->monitor;
	uint64_t exp;

	if (spa_system_timerfd_read(monitor->main_system, source->fd, &exp) < 0)
		spa_log_warn(monitor->log, "error reading timerfd: %s", strerror(errno));

	spa_bt_transport_volume_changed(transport);
}

static int spa_bt_transport_start_volume_timer(struct spa_bt_transport *transport)
{
	return start_timeout_timer(transport->monitor,
			&transport->volume_timer,
			spa_bt_transport_volume_timer_event,
			TRANSPORT_VOLUME_TIMEOUT_MSEC, transport);
}

static int spa_bt_transport_stop_volume_timer(struct spa_bt_transport *transport)
{
	return stop_timeout_timer(transport->monitor, &transport->volume_timer);
}


int spa_bt_transport_ensure_sco_io(struct spa_bt_transport *t, struct spa_loop *data_loop)
{
	if (t->sco_io == NULL) {
		t->sco_io = spa_bt_sco_io_create(data_loop,
						 t->fd,
						 t->read_mtu,
						 t->write_mtu);
		if (t->sco_io == NULL)
			return -ENOMEM;
	}
	return 0;
}

int64_t spa_bt_transport_get_delay_nsec(struct spa_bt_transport *t)
{
	if (t->delay_us != SPA_BT_UNKNOWN_DELAY) {
		/* end-to-end delay = (presentation) delay + transport latency
		 *
		 * For BAP, see Core v5.3 Vol 6/G Sec 3.2.2 Fig. 3.2 &
		 * BAP v1.0 Sec 7.1.1.
		 */
		int64_t delay = t->delay_us;
		if (t->latency_us != SPA_BT_UNKNOWN_DELAY)
			delay += t->latency_us;
		return delay * SPA_NSEC_PER_USEC;
	}

	/* Fallback values when device does not provide information */

	if (t->media_codec == NULL)
		return 30 * SPA_NSEC_PER_MSEC;

	switch (t->media_codec->id) {
	case SPA_BLUETOOTH_AUDIO_CODEC_SBC:
	case SPA_BLUETOOTH_AUDIO_CODEC_SBC_XQ:
		return 200 * SPA_NSEC_PER_MSEC;
	case SPA_BLUETOOTH_AUDIO_CODEC_MPEG:
	case SPA_BLUETOOTH_AUDIO_CODEC_AAC:
		return 200 * SPA_NSEC_PER_MSEC;
	case SPA_BLUETOOTH_AUDIO_CODEC_APTX:
	case SPA_BLUETOOTH_AUDIO_CODEC_APTX_HD:
		return 150 * SPA_NSEC_PER_MSEC;
	case SPA_BLUETOOTH_AUDIO_CODEC_LDAC:
		return 175 * SPA_NSEC_PER_MSEC;
	case SPA_BLUETOOTH_AUDIO_CODEC_APTX_LL:
	case SPA_BLUETOOTH_AUDIO_CODEC_APTX_LL_DUPLEX:
	case SPA_BLUETOOTH_AUDIO_CODEC_FASTSTREAM:
	case SPA_BLUETOOTH_AUDIO_CODEC_FASTSTREAM_DUPLEX:
	case SPA_BLUETOOTH_AUDIO_CODEC_LC3:
		return 40 * SPA_NSEC_PER_MSEC;
	default:
		break;
	};
	return 150 * SPA_NSEC_PER_MSEC;
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

			if (spa_streq(key, "UUID")) {
				transport->profile = swap_profile(spa_bt_profile_from_uuid(value));
				if (transport->profile == SPA_BT_PROFILE_NULL)
					spa_log_warn(monitor->log, "unknown profile %s", value);
			}
			else if (spa_streq(key, "State")) {
				enum spa_bt_transport_state state  = spa_bt_transport_state_from_string(value);

				/* Transition to active emitted only from acquire callback. */
				if (state != SPA_BT_TRANSPORT_STATE_ACTIVE)
					spa_bt_transport_set_state(transport, state);
			}
			else if (spa_streq(key, "Device")) {
				struct spa_bt_device *device = spa_bt_device_find(monitor, value);
				if (transport->device != device) {
					if (transport->device != NULL)
						spa_list_remove(&transport->device_link);
					transport->device = device;
					if (device != NULL)
						spa_list_append(&device->transport_list, &transport->device_link);
					else
						spa_log_warn(monitor->log, "could not find device %s", value);
				}
			}
			else if (spa_streq(key, "Endpoint")) {
				struct spa_bt_remote_endpoint *ep = remote_endpoint_find(monitor, value);
				if (!ep) {
					spa_log_warn(monitor->log, "Unable to find remote endpoint for %s", value);
					goto next;
				}

				// If the remote endpoint is an acceptor this transport is an initiator
				transport->bap_initiator = ep->acceptor;
			}
		}
		else if (spa_streq(key, "Codec")) {
			uint8_t value;

			if (type != DBUS_TYPE_BYTE)
				goto next;
			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "transport %p: %s=%02x", transport, key, value);

			transport->codec = value;
		}
		else if (spa_streq(key, "Configuration")) {
			DBusMessageIter iter;
			uint8_t *value;
			int len;

			if (!check_iter_signature(&it[1], "ay"))
				goto next;

			dbus_message_iter_recurse(&it[1], &iter);
			dbus_message_iter_get_fixed_array(&iter, &value, &len);

			spa_log_debug(monitor->log, "transport %p: %s=%d", transport, key, len);
			spa_debug_log_mem(monitor->log, SPA_LOG_LEVEL_DEBUG, 2, value, (size_t)len);

			free(transport->configuration);
			transport->configuration_len = 0;

			transport->configuration = malloc(len);
			if (transport->configuration) {
				memcpy(transport->configuration, value, len);
				transport->configuration_len = len;
			}
		}
		else if (spa_streq(key, "Volume")) {
			uint16_t value;
			struct spa_bt_transport_volume * t_volume;

			if (type != DBUS_TYPE_UINT16)
				goto next;
			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "transport %p: %s=%d", transport, key, value);

			if (transport->profile & SPA_BT_PROFILE_A2DP_SINK)
				t_volume = &transport->volumes[SPA_BT_VOLUME_ID_TX];
			else if (transport->profile & SPA_BT_PROFILE_A2DP_SOURCE)
				t_volume = &transport->volumes[SPA_BT_VOLUME_ID_RX];
			else
				goto next;

			t_volume->active = true;
			t_volume->new_hw_volume = value;

			if (transport->profile & SPA_BT_PROFILE_A2DP_SINK)
				spa_bt_transport_start_volume_timer(transport);
			else
				spa_bt_transport_volume_changed(transport);
		}
		else if (spa_streq(key, "Delay")) {
			uint16_t value;

			if (type != DBUS_TYPE_UINT16)
				goto next;
			dbus_message_iter_get_basic(&it[1], &value);

			spa_log_debug(monitor->log, "transport %p: %s=%d", transport, key, (int)value);

			transport->delay_us = value * 100;

			spa_bt_transport_emit_delay_changed(transport);
		}
		else if (spa_streq(key, "QoS")) {
			struct bap_codec_qos_full qos;
			DBusMessageIter value;

			if (!check_iter_signature(&it[1], "a{sv}"))
				goto next;

			dbus_message_iter_recurse(&it[1], &value);
			parse_codec_qos(monitor, &value, &qos);

			transport->bap_cig = qos.cig;
			transport->bap_cis = qos.cis;
			transport->bap_big = qos.big;
			transport->bap_bis = qos.bis;
			transport->delay_us = qos.qos.delay;
			transport->latency_us = (unsigned int)qos.qos.latency * 1000;
			transport->bap_interval = qos.qos.interval;

			spa_bt_transport_emit_delay_changed(transport);
		}
		else if (spa_streq(key, "Links")) {
			DBusMessageIter iter;

			if (!check_iter_signature(&it[1], "ao"))
				goto next;

			dbus_message_iter_recurse(&it[1], &iter);
			while (dbus_message_iter_get_arg_type(&iter) != DBUS_TYPE_INVALID) {
				const char *transport_path;
				struct spa_bt_transport *t;

				dbus_message_iter_get_basic(&iter, &transport_path);

				spa_log_debug(monitor->log, "transport %p: Linked with=%s", transport, transport_path);
				t = spa_bt_transport_find(monitor, transport_path);
				if (!t) {
					spa_log_warn(monitor->log, "Unable to find linked transport");
					dbus_message_iter_next(&iter);
					continue;
				}

				if (spa_list_is_empty(&t->bap_transport_linked))
				    spa_list_append(&transport->bap_transport_linked, &t->bap_transport_linked);
				else if (spa_list_is_empty(&transport->bap_transport_linked))
				    spa_list_append(&t->bap_transport_linked, &transport->bap_transport_linked);

				dbus_message_iter_next(&iter);
			}
		}

next:
		dbus_message_iter_next(props_iter);
	}
	return 0;
}

static void transport_set_property_volume_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_transport *transport = user_data;
	struct spa_bt_monitor *monitor = transport->monitor;
	spa_auto(DBusError) err = DBUS_ERROR_INIT;

	spa_assert(transport->volume_call == pending);
	spa_autoptr(DBusMessage) r = steal_reply_and_unref(&transport->volume_call);

	if (dbus_set_error_from_message(&err, r)) {
		spa_log_info(monitor->log, "transport %p: set volume failed for transport %s: %s",
				transport, transport->path, err.message);
	} else {
		spa_log_debug(monitor->log, "transport %p: set volume complete",
				transport);
	}
}

static void transport_set_property_volume(struct spa_bt_transport *transport, uint16_t value)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	spa_autoptr(DBusMessage) m = NULL;
	DBusMessageIter it[2];
	const char *interface = BLUEZ_MEDIA_TRANSPORT_INTERFACE;
	const char *name = "Volume";
	int res = 0;

	cancel_and_unref(&transport->volume_call);

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
					 transport->path,
	                                 DBUS_INTERFACE_PROPERTIES,
					 "Set");
	if (m == NULL) {
		res = -ENOMEM;
		goto fail;
	}

	dbus_message_iter_init_append(m, &it[0]);
	dbus_message_iter_append_basic(&it[0], DBUS_TYPE_STRING, &interface);
	dbus_message_iter_append_basic(&it[0], DBUS_TYPE_STRING, &name);
	dbus_message_iter_open_container(&it[0], DBUS_TYPE_VARIANT,
					DBUS_TYPE_UINT16_AS_STRING, &it[1]);
	dbus_message_iter_append_basic(&it[1], DBUS_TYPE_UINT16, &value);
	dbus_message_iter_close_container(&it[0], &it[1]);

	transport->volume_call = send_with_reply(monitor->conn, m, transport_set_property_volume_reply, transport);
	if (!transport->volume_call) {
		res = -EIO;
		goto fail;
	}

	spa_log_debug(monitor->log, "transport %p: setting volume to %d", transport, value);
	return;

fail:
	spa_log_debug(monitor->log, "transport %p: failed to set volume %d: %s",
			transport, value, spa_strerror(res));
}

static int transport_set_volume(void *data, int id, float volume)
{
	struct spa_bt_transport *transport = data;
	struct spa_bt_transport_volume *t_volume = &transport->volumes[id];
	uint16_t value;

	if (!t_volume->active || !spa_bt_transport_volume_enabled(transport))
		return -ENOTSUP;

	value = spa_bt_volume_linear_to_hw(volume, 127);
	t_volume->volume = volume;

	/* AVRCP volume would not applied on remote sink device
	 * if transport is not acquired (idle). */
	if (transport->fd < 0 && (transport->profile & SPA_BT_PROFILE_A2DP_SINK)) {
		t_volume->hw_volume = SPA_BT_VOLUME_INVALID;
		return 0;
	} else if (t_volume->hw_volume != value) {
		t_volume->hw_volume = value;
		spa_bt_transport_stop_volume_timer(transport);
		transport_set_property_volume(transport, value);
	}
	return 0;
}

static int transport_create_iso_io(struct spa_bt_transport *transport)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	struct spa_bt_transport *t;

	if (!(transport->profile & (SPA_BT_PROFILE_BAP_SINK | SPA_BT_PROFILE_BAP_SOURCE |
		SPA_BT_PROFILE_BAP_BROADCAST_SINK | SPA_BT_PROFILE_BAP_BROADCAST_SOURCE)))
		return 0;

	if ((transport->profile == SPA_BT_PROFILE_BAP_BROADCAST_SINK) ||
		(transport->profile == SPA_BT_PROFILE_BAP_BROADCAST_SOURCE)) {
		if (transport->bap_big == 0xff || transport->bap_bis == 0xff)
			return -EINVAL;
	} else {
		if (transport->bap_cig == 0xff || transport->bap_cis == 0xff)
			return -EINVAL;
	}

	if (transport->iso_io) {
		spa_log_debug(monitor->log, "transport %p: remove ISO IO", transport);
		spa_bt_iso_io_destroy(transport->iso_io);
		transport->iso_io = NULL;
	}

	/* Transports in same connected iso group share the same i/o */
	spa_list_for_each(t, &monitor->transport_list, link) {
		if (!(t->profile & (SPA_BT_PROFILE_BAP_SINK | SPA_BT_PROFILE_BAP_SOURCE |
				SPA_BT_PROFILE_BAP_BROADCAST_SINK | SPA_BT_PROFILE_BAP_BROADCAST_SOURCE)))
			continue;

		if (t->device->adapter != transport->device->adapter)
			continue;

		if ((transport->profile == SPA_BT_PROFILE_BAP_BROADCAST_SINK) ||
			(transport->profile == SPA_BT_PROFILE_BAP_BROADCAST_SOURCE)) {
			if (t->bap_big != transport->bap_big)
				continue;
		} else {
			if (t->bap_cig != transport->bap_cig)
				continue;
		}

		if (t->iso_io) {
			spa_log_debug(monitor->log, "transport %p: attach ISO IO to %p",
					transport, t);
			transport->iso_io = spa_bt_iso_io_attach(t->iso_io, transport);
			if (transport->iso_io == NULL)
				return -errno;
			return 0;
		}
	}

	spa_log_debug(monitor->log, "transport %p: new ISO IO", transport);
	transport->iso_io = spa_bt_iso_io_create(transport, monitor->log, monitor->data_loop, monitor->data_system);
	if (transport->iso_io == NULL)
		return -errno;

	return 0;
}

static bool transport_in_same_cig(struct spa_bt_transport *transport, struct spa_bt_transport *other)
{
	return (other->profile & (SPA_BT_PROFILE_BAP_SINK | SPA_BT_PROFILE_BAP_SOURCE)) &&
		other->bap_cig == transport->bap_cig &&
		other->bap_initiator;
}

static void transport_acquire_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_transport *transport = user_data;
	struct spa_bt_monitor *monitor = transport->monitor;
	struct spa_bt_device *device = transport->device;
	int ret = 0;
	spa_auto(DBusError) err = DBUS_ERROR_INIT;
	struct spa_bt_transport *t, *t_linked;

	spa_assert(transport->acquire_call == pending);
	spa_autoptr(DBusMessage) r = steal_reply_and_unref(&transport->acquire_call);

	spa_bt_device_update_last_bluez_action_time(device);

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(monitor->log, "Acquire %s returned error: %s",
				transport->path,
				dbus_message_get_error_name(r));
		ret = -EIO;
		goto finish;
	}

	if (transport->fd >= 0) {
		spa_log_error(monitor->log, "transport %p: invalid duplicate acquire", transport);
		ret = -EINVAL;
		goto finish;
	}

	if (!dbus_message_get_args(r, &err,
				   DBUS_TYPE_UNIX_FD, &transport->fd,
				   DBUS_TYPE_UINT16, &transport->read_mtu,
				   DBUS_TYPE_UINT16, &transport->write_mtu,
				   DBUS_TYPE_INVALID)) {
		spa_log_error(monitor->log, "Failed to parse Acquire %s reply: %s",
				transport->path, err.message);
		ret = -EIO;
		goto finish;
	}

	spa_log_debug(monitor->log, "transport %p: Acquired %s, fd %d MTU %d:%d", transport,
			transport->path, transport->fd, transport->read_mtu, transport->write_mtu);

	spa_bt_player_set_state(transport->device->adapter->dummy_player, SPA_BT_PLAYER_PLAYING);

	transport_sync_volume(transport);

finish:
	if (ret < 0)
		spa_bt_transport_set_state(transport, SPA_BT_TRANSPORT_STATE_ERROR);
	else {
		if (transport_create_iso_io(transport) < 0)
			spa_log_error(monitor->log, "transport %p: transport_create_iso_io failed",
					transport);
		/* For broadcast the initiator moves the transport state to SPA_BT_TRANSPORT_STATE_ACTIVE */
		/* TODO: handeling multiple BIGs support */
		if ((transport->profile == SPA_BT_PROFILE_BAP_BROADCAST_SINK) ||
			(transport->profile == SPA_BT_PROFILE_BAP_BROADCAST_SOURCE))	{
			spa_bt_transport_set_state(transport, SPA_BT_TRANSPORT_STATE_ACTIVE);
		} else {
			if (!transport->bap_initiator)
				spa_bt_transport_set_state(transport, SPA_BT_TRANSPORT_STATE_ACTIVE);
		}
	}

	/* For LE Audio, multiple transport from the same device may share the same
	 * stream (CIS) and group (CIG) but for different direction, e.g. a speaker and
	 * a microphone. In this case they are linked, and we need to set the values
	 * for all of them here.
	 */
	spa_list_for_each(t_linked, &transport->bap_transport_linked, bap_transport_linked) {
		if (ret < 0) {
			spa_bt_transport_set_state(t_linked, SPA_BT_TRANSPORT_STATE_ERROR);
			continue;
		}

		t_linked->fd = transport->fd;
		t_linked->read_mtu = transport->read_mtu;
		t_linked->write_mtu = transport->write_mtu;
		spa_log_debug(monitor->log, "transport %p: linked Acquired %s, fd %d MTU %d:%d", t_linked,
				t_linked->path, t_linked->fd, t_linked->read_mtu, t_linked->write_mtu);

		if (transport_create_iso_io(t_linked) < 0)
			spa_log_error(monitor->log, "transport %p: transport_create_iso_io failed",
					t_linked);

		/* For broadcast there initiator moves the transport state to SPA_BT_TRANSPORT_STATE_ACTIVE */
		if ((transport->profile == SPA_BT_PROFILE_BAP_BROADCAST_SINK) ||
			(transport->profile == SPA_BT_PROFILE_BAP_BROADCAST_SOURCE))	{
			spa_bt_transport_set_state(t_linked, SPA_BT_TRANSPORT_STATE_ACTIVE);
		} else {
			if (!transport->bap_initiator)
				spa_bt_transport_set_state(t_linked, SPA_BT_TRANSPORT_STATE_ACTIVE);
		}
	}

	/*
	 * Transports in same CIG emit state change events at the same time,
	 * after all pending acquires complete.
	 */
	if (transport->bap_initiator) {
		spa_list_for_each(t, &monitor->transport_list, link) {
			if (!transport_in_same_cig(transport, t))
				continue;
			if (t->acquire_call)
				return;
		}
		spa_list_for_each(t, &monitor->transport_list, link) {
			if (!transport_in_same_cig(transport, t))
				continue;
			if (t->fd >= 0)
				spa_bt_transport_set_state(t, SPA_BT_TRANSPORT_STATE_ACTIVE);
		}
	}
}

static int do_transport_acquire(struct spa_bt_transport *transport)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	spa_autoptr(DBusMessage) m = NULL;
	struct spa_bt_transport *t_linked;

	spa_list_for_each(t_linked, &transport->bap_transport_linked, bap_transport_linked) {
		/* If a linked transport has been acquired, it will do all the work */
		if (t_linked->acquire_call || t_linked->acquired) {
			spa_log_debug(monitor->log, "Acquiring %s: use linked transport %s",
					transport->path, t_linked->path);
			spa_bt_transport_emit_state_changed(transport, transport->state, transport->state);
			return 0;
		}
	}

	if (transport->acquire_call)
		return -EBUSY;

	spa_log_info(monitor->log, "Acquiring transport %s", transport->path);

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
					 transport->path,
					 BLUEZ_MEDIA_TRANSPORT_INTERFACE,
					 "Acquire");
	if (m == NULL)
		return -ENOMEM;

	transport->acquire_call = send_with_reply(monitor->conn, m, transport_acquire_reply, transport);
	if (!transport->acquire_call)
		return -EIO;

	return 0;
}

static bool another_cig_transport_active(struct spa_bt_transport *transport)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	struct spa_bt_transport *t;

	spa_list_for_each(t, &monitor->transport_list, link) {
		if (!transport_in_same_cig(transport, t) || t == transport)
			continue;
		if (t->acquired)
			return true;
	}

	return false;
}

static int transport_acquire(void *data, bool optional)
{
	struct spa_bt_transport *transport = data;
	struct spa_bt_monitor *monitor = transport->monitor;

	/*
	 * XXX: When as BAP Central, all CIS in a CIG must be acquired at the same time.
	 * XXX: This is because of kernel ISO socket limitations, which does not handle
	 * XXX: currently starting streams in the group one by one.
	 */
	if (transport->bap_initiator && !another_cig_transport_active(transport)) {
		struct spa_bt_transport *t;

		spa_list_for_each(t, &monitor->transport_list, link) {
			if (!transport_in_same_cig(transport, t) || t == transport)
				continue;

			spa_log_debug(monitor->log, "Acquire CIG %d: transport %s",
					transport->bap_cig, t->path);

			do_transport_acquire(t);
		}

		spa_log_debug(monitor->log, "Acquire CIG %d: transport %s",
				transport->bap_cig, transport->path);
	}
	if (transport->bap_initiator &&
			(transport->fd >= 0 || transport->acquire_call)) {
		/* Already acquired/acquiring */
		spa_log_debug(monitor->log, "Acquiring %s: was in acquired CIG", transport->path);
		spa_bt_transport_emit_state_changed(transport, transport->state, transport->state);
		return 0;
	}

	return do_transport_acquire(data);
}

static int do_transport_release(struct spa_bt_transport *transport)
{
	struct spa_bt_monitor *monitor = transport->monitor;
	spa_autoptr(DBusMessage) m = NULL, r = NULL;
	struct spa_bt_transport *t_linked;
	bool is_idle = (transport->state == SPA_BT_TRANSPORT_STATE_IDLE);
	bool linked = false;

	spa_log_debug(monitor->log, "transport %p: Release %s",
			transport, transport->path);

	spa_bt_player_set_state(transport->device->adapter->dummy_player, SPA_BT_PLAYER_STOPPED);

	spa_bt_transport_set_state(transport, SPA_BT_TRANSPORT_STATE_IDLE);

	cancel_and_unref(&transport->acquire_call);

	if (transport->iso_io) {
		spa_log_debug(monitor->log, "transport %p: remove ISO IO", transport);
		spa_bt_iso_io_destroy(transport->iso_io);
		transport->iso_io = NULL;
	}

	/* For LE Audio, multiple transport stream (CIS) can be linked together (CIG).
	 * If they are part of the same device they re-use the same fd, and call to
	 * release should be done for the last one only.
	 */
	spa_list_for_each(t_linked, &transport->bap_transport_linked, bap_transport_linked) {
		if (t_linked->acquire_call || t_linked->acquired) {
			linked = true;
			break;
		}
	}
	if (linked) {
		spa_log_info(monitor->log, "Linked transport %s released", transport->path);
		transport->fd = -1;
		return 0;
	}

	if (transport->fd >= 0) {
		close(transport->fd);
		transport->fd = -1;
	}

	spa_log_info(monitor->log, "Releasing transport %s", transport->path);

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
					 transport->path,
					 BLUEZ_MEDIA_TRANSPORT_INTERFACE,
					 "Release");
	if (m == NULL)
		return -ENOMEM;

	spa_auto(DBusError) err = DBUS_ERROR_INIT;
	r = dbus_connection_send_with_reply_and_block(monitor->conn, m, -1, &err);
	if (r == NULL)  {
		if (is_idle) {
			/* XXX: The fd always needs to be closed. However, Release()
			 * XXX: apparently doesn't need to be called on idle transports
			 * XXX: and fails. We call it just to be sure (e.g. in case
			 * XXX: there's a race with updating the property), but tone down the error.
			 */
			spa_log_debug(monitor->log, "Failed to release idle transport %s: %s",
			              transport->path, err.message);
		} else {
			spa_log_error(monitor->log, "Failed to release transport %s: %s",
			              transport->path, err.message);
		}
	} else {
		spa_log_info(monitor->log, "Transport %s released", transport->path);
	}

	return 0;
}

static int transport_release(void *data)
{
	struct spa_bt_transport *transport = data;
	struct spa_bt_monitor *monitor = transport->monitor;
	struct spa_bt_transport *t;

	/*
	 * XXX: When as BAP Central, release CIS in a CIG when the last transport
	 * XXX: goes away.
	 */
	if (transport->bap_initiator) {
		/* Check if another transport is alive */
		if (another_cig_transport_active(transport)) {
			spa_log_debug(monitor->log, "Releasing %s: wait for CIG %d",
					transport->path, transport->bap_cig);
			return 0;
		}

		/* Release remaining transports in CIG */
		spa_list_for_each(t, &monitor->transport_list, link) {
			if (!transport_in_same_cig(transport, t) || t == transport)
				continue;

			spa_log_debug(monitor->log, "Release CIG %d: transport %s",
					transport->bap_cig, t->path);

			if (t->fd >= 0)
				do_transport_release(t);
		}

		spa_log_debug(monitor->log, "Release CIG %d: transport %s",
				transport->bap_cig, transport->path);
	}

	return do_transport_release(data);
}


static const struct spa_bt_transport_implementation transport_impl = {
	SPA_VERSION_BT_TRANSPORT_IMPLEMENTATION,
	.acquire = transport_acquire,
	.release = transport_release,
	.set_volume = transport_set_volume,
};

static void media_codec_switch_reply(DBusPendingCall *pending, void *userdata);

static int media_codec_switch_cmp(const void *a, const void *b);

static struct spa_bt_media_codec_switch *media_codec_switch_cmp_sw;  /* global for qsort */

static int media_codec_switch_start_timer(struct spa_bt_media_codec_switch *sw, uint64_t timeout);

static int media_codec_switch_stop_timer(struct spa_bt_media_codec_switch *sw);

static void media_codec_switch_free(struct spa_bt_media_codec_switch *sw)
{
	char **p;

	media_codec_switch_stop_timer(sw);

	cancel_and_unref(&sw->pending);

	if (sw->device != NULL)
		spa_list_remove(&sw->device_link);

	if (sw->paths != NULL)
		for (p = sw->paths; *p != NULL; ++p)
			free(*p);

	free(sw->paths);
	free(sw->codecs);
	free(sw);
}

static void media_codec_switch_next(struct spa_bt_media_codec_switch *sw)
{
	spa_assert(*sw->codec_iter != NULL && *sw->path_iter != NULL);

	++sw->path_iter;
	if (*sw->path_iter == NULL) {
		++sw->codec_iter;
		sw->path_iter = sw->paths;
	}

	sw->retries = CODEC_SWITCH_RETRIES;
}

static bool media_codec_switch_process_current(struct spa_bt_media_codec_switch *sw)
{
	struct spa_bt_remote_endpoint *ep;
	struct spa_bt_transport *t;
	const struct media_codec *codec;
	uint8_t config[A2DP_MAX_CAPS_SIZE];
	enum spa_bt_media_direction direction;
	spa_autofree char *local_endpoint = NULL;
	int res, config_size;
	spa_autoptr(DBusMessage) m = NULL;
	DBusMessageIter iter, d;
	int i;
	bool sink;

	/* Try setting configuration for current codec on current endpoint in list */

	codec = *sw->codec_iter;

	spa_log_debug(sw->device->monitor->log, "media codec switch %p: consider codec %s for remote endpoint %s",
	              sw, (*sw->codec_iter)->name, *sw->path_iter);

	ep = device_remote_endpoint_find(sw->device, *sw->path_iter);

	if (ep == NULL || ep->capabilities == NULL || ep->uuid == NULL) {
		spa_log_debug(sw->device->monitor->log, "media codec switch %p: endpoint %s not valid, try next",
		              sw, *sw->path_iter);
		return false;
	}

	/* Setup and check compatible configuration */
	if (ep->codec != codec->codec_id) {
		spa_log_debug(sw->device->monitor->log, "media codec switch %p: different codec, try next", sw);
		return false;
	}

	if (!(sw->profile & spa_bt_profile_from_uuid(ep->uuid))) {
		spa_log_debug(sw->device->monitor->log, "media codec switch %p: wrong uuid (%s) for profile, try next",
		              sw, ep->uuid);
		return false;
	}

	if ((sw->profile & SPA_BT_PROFILE_A2DP_SINK) || (sw->profile & SPA_BT_PROFILE_BAP_SINK) ) {
		direction = SPA_BT_MEDIA_SOURCE;
		sink = false;
	} else if ((sw->profile & SPA_BT_PROFILE_A2DP_SOURCE) || (sw->profile & SPA_BT_PROFILE_BAP_SOURCE) ) {
		direction = SPA_BT_MEDIA_SINK;
		sink = true;
	} else {
		spa_log_debug(sw->device->monitor->log, "media codec switch %p: bad profile (%d), try next",
		              sw, sw->profile);
		return false;
	}

	if (media_codec_to_endpoint(codec, direction, &local_endpoint) < 0) {
		spa_log_debug(sw->device->monitor->log, "media codec switch %p: no endpoint for codec %s, try next",
		              sw, codec->name);
		return false;
	}

	/* Each endpoint can be used by only one device at a time (on each adapter) */
	spa_list_for_each(t, &sw->device->monitor->transport_list, link) {
		if (t->device == sw->device)
			continue;
		if (t->device->adapter != sw->device->adapter)
			continue;
		if (spa_streq(t->endpoint_path, local_endpoint)) {
			spa_log_debug(sw->device->monitor->log, "media codec switch %p: endpoint %s in use, try next",
					sw, local_endpoint);
			return false;
		}
	}

	res = codec->select_config(codec, sink ? MEDIA_CODEC_FLAG_SINK : 0, ep->capabilities, ep->capabilities_len,
				   &sw->device->monitor->default_audio_info,
				   &sw->device->monitor->global_settings, config);
	if (res < 0) {
		spa_log_debug(sw->device->monitor->log, "media codec switch %p: incompatible capabilities (%d), try next",
		              sw, res);
		return false;
	}
	config_size = res;

	spa_log_debug(sw->device->monitor->log, "media codec switch %p: configuration %d", sw, config_size);
	for (i = 0; i < config_size; i++)
		spa_log_debug(sw->device->monitor->log, "media codec switch %p:     %d: %02x", sw, i, config[i]);

	/* Codecs may share the same endpoint, so indicate which one we are using */
	sw->device->preferred_codec = codec;

	/* org.bluez.MediaEndpoint1.SetConfiguration on remote endpoint */
	m = dbus_message_new_method_call(BLUEZ_SERVICE, ep->path, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "SetConfiguration");
	if (m == NULL) {
		spa_log_debug(sw->device->monitor->log, "media codec switch %p: dbus allocation failure, try next", sw);
		return false;
	}

	spa_bt_device_update_last_bluez_action_time(sw->device);

	spa_log_info(sw->device->monitor->log, "media codec switch %p: trying codec %s for endpoint %s, local endpoint %s",
	             sw, codec->name, ep->path, local_endpoint);

	dbus_message_iter_init_append(m, &iter);
	dbus_message_iter_append_basic(&iter, DBUS_TYPE_OBJECT_PATH, &local_endpoint);
	dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{sv}", &d);
	append_basic_array_variant_dict_entry(&d, "Capabilities", "ay", "y", DBUS_TYPE_BYTE, config, config_size);
	dbus_message_iter_close_container(&iter, &d);

	spa_assert(sw->pending == NULL);
	sw->pending = send_with_reply(sw->device->monitor->conn, m, media_codec_switch_reply, sw);
	if (!sw->pending) {
		spa_log_error(sw->device->monitor->log, "media codec switch %p: dbus call failure, try next", sw);
		return false;
	}

	return true;
}

static void media_codec_switch_process(struct spa_bt_media_codec_switch *sw)
{
	while (*sw->codec_iter != NULL && *sw->path_iter != NULL) {
		uint64_t now, threshold;

		/* Rate limit BlueZ calls */
		now = get_time_now(sw->device->monitor);
		threshold = sw->device->last_bluez_action_time + BLUEZ_ACTION_RATE_MSEC * SPA_NSEC_PER_MSEC;
		if (now < threshold) {
			/* Wait for timeout */
			media_codec_switch_start_timer(sw, threshold - now);
			return;
		}

		if (sw->path_iter == sw->paths && (*sw->codec_iter)->caps_preference_cmp) {
			/* Sort endpoints according to codec preference, when at a new codec. */
			media_codec_switch_cmp_sw = sw;
			qsort(sw->paths, sw->num_paths, sizeof(char *), media_codec_switch_cmp);
		}

		if (media_codec_switch_process_current(sw)) {
			/* Wait for dbus reply */
			return;
		}

		media_codec_switch_next(sw);
	};

	/* Didn't find any suitable endpoint. Report failure. */
	spa_log_info(sw->device->monitor->log, "media codec switch %p: failed to get an endpoint", sw);
	spa_bt_device_emit_codec_switched(sw->device, -ENODEV);
	spa_bt_device_check_profiles(sw->device, false);
	media_codec_switch_free(sw);
}

static bool media_codec_switch_goto_active(struct spa_bt_media_codec_switch *sw)
{
	struct spa_bt_device *device = sw->device;
	struct spa_bt_media_codec_switch *active_sw;

	active_sw = spa_list_first(&device->codec_switch_list, struct spa_bt_media_codec_switch, device_link);

	if (active_sw != sw) {
		struct spa_bt_media_codec_switch *t;

		/* This codec switch has been canceled. Switch to the newest one. */
		spa_log_debug(sw->device->monitor->log,
			      "media codec switch %p: canceled, go to new switch", sw);

		spa_list_for_each_safe(sw, t, &device->codec_switch_list, device_link) {
			if (sw != active_sw)
				media_codec_switch_free(sw);
		}

		media_codec_switch_process(active_sw);
		return false;
	}

	return true;
}

static void media_codec_switch_timer_event(struct spa_source *source)
{
	struct spa_bt_media_codec_switch *sw = source->data;
	struct spa_bt_device *device = sw->device;
	struct spa_bt_monitor *monitor = device->monitor;
	uint64_t exp;

	if (spa_system_timerfd_read(monitor->main_system, source->fd, &exp) < 0)
		spa_log_warn(monitor->log, "error reading timerfd: %s", strerror(errno));

	spa_log_debug(monitor->log, "media codec switch %p: rate limit timer event", sw);

	media_codec_switch_stop_timer(sw);

	if (!media_codec_switch_goto_active(sw))
		return;

	media_codec_switch_process(sw);
}

static void media_codec_switch_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_media_codec_switch *sw = user_data;
	struct spa_bt_device *device = sw->device;

	spa_assert(sw->pending == pending);
	spa_autoptr(DBusMessage) r = steal_reply_and_unref(&sw->pending);

	spa_bt_device_update_last_bluez_action_time(device);

	if (!media_codec_switch_goto_active(sw))
		return;

	if (r == NULL) {
		spa_log_error(sw->device->monitor->log,
		              "media codec switch %p: empty reply from dbus, trying next",
		              sw);
		goto next;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_debug(sw->device->monitor->log,
		              "media codec switch %p: failed (%s), trying next",
		              sw, dbus_message_get_error_name(r));
		goto next;
	}

	/* Success */
	spa_log_info(sw->device->monitor->log, "media codec switch %p: success", sw);
	spa_bt_device_emit_codec_switched(sw->device, 0);
	spa_bt_device_check_profiles(sw->device, false);
	media_codec_switch_free(sw);
	return;

next:
	if (sw->retries > 0)
		--sw->retries;
	else
		media_codec_switch_next(sw);

	media_codec_switch_process(sw);
	return;
}

static int media_codec_switch_start_timer(struct spa_bt_media_codec_switch *sw, uint64_t timeout)
{
	struct spa_bt_monitor *monitor = sw->device->monitor;
	struct itimerspec ts;

	spa_assert(sw->timer.data == NULL);

	spa_log_debug(monitor->log, "media codec switch %p: starting rate limit timer", sw);

	if (sw->timer.data == NULL) {
		sw->timer.data = sw;
		sw->timer.func = media_codec_switch_timer_event;
		sw->timer.fd = spa_system_timerfd_create(monitor->main_system,
				CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
		sw->timer.mask = SPA_IO_IN;
		sw->timer.rmask = 0;
		spa_loop_add_source(monitor->main_loop, &sw->timer);
	}
	ts.it_value.tv_sec = timeout / SPA_NSEC_PER_SEC;
	ts.it_value.tv_nsec = timeout % SPA_NSEC_PER_SEC;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(monitor->main_system, sw->timer.fd, 0, &ts, NULL);
	return 0;
}

static int media_codec_switch_stop_timer(struct spa_bt_media_codec_switch *sw)
{
	struct spa_bt_monitor *monitor = sw->device->monitor;
	struct itimerspec ts;

	if (sw->timer.data == NULL)
		return 0;

	spa_log_debug(monitor->log, "media codec switch %p: stopping rate limit timer", sw);

	spa_loop_remove_source(monitor->main_loop, &sw->timer);
	ts.it_value.tv_sec = 0;
	ts.it_value.tv_nsec = 0;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(monitor->main_system, sw->timer.fd, 0, &ts, NULL);
	spa_system_close(monitor->main_system, sw->timer.fd);
	sw->timer.data = NULL;
	return 0;
}

static int media_codec_switch_cmp(const void *a, const void *b)
{
	struct spa_bt_media_codec_switch *sw = media_codec_switch_cmp_sw;
	const struct media_codec *codec = *sw->codec_iter;
	const char *path1 = *(char **)a, *path2 = *(char **)b;
	struct spa_bt_remote_endpoint *ep1, *ep2;
	uint32_t flags;

	ep1 = device_remote_endpoint_find(sw->device, path1);
	ep2 = device_remote_endpoint_find(sw->device, path2);

	if (ep1 != NULL && (ep1->uuid == NULL || ep1->codec != codec->codec_id || ep1->capabilities == NULL))
		ep1 = NULL;
	if (ep2 != NULL && (ep2->uuid == NULL || ep2->codec != codec->codec_id || ep2->capabilities == NULL))
		ep2 = NULL;
	if (ep1 && ep2 && !spa_streq(ep1->uuid, ep2->uuid)) {
		ep1 = NULL;
		ep2 = NULL;
	}

	if (ep1 == NULL && ep2 == NULL)
		return 0;
	else if (ep1 == NULL)
		return 1;
	else if (ep2 == NULL)
		return -1;

	if (codec->bap)
		flags = spa_streq(ep1->uuid, SPA_BT_UUID_BAP_SOURCE) ? MEDIA_CODEC_FLAG_SINK : 0;
	else
		flags = spa_streq(ep1->uuid, SPA_BT_UUID_A2DP_SOURCE) ? MEDIA_CODEC_FLAG_SINK : 0;

	return codec->caps_preference_cmp(codec, flags, ep1->capabilities, ep1->capabilities_len,
			ep2->capabilities, ep2->capabilities_len, &sw->device->monitor->default_audio_info,
			&sw->device->monitor->global_settings);
}

/* Ensure there's a transport for at least one of the listed codecs */
int spa_bt_device_ensure_media_codec(struct spa_bt_device *device, const struct media_codec * const *codecs)
{
	struct spa_bt_media_codec_switch *sw;
	struct spa_bt_remote_endpoint *ep;
	struct spa_bt_transport *t;
	const struct media_codec *preferred_codec = NULL;
	size_t i, j, num_codecs, num_eps;

	if (!device->adapter->a2dp_application_registered &&
			!device->adapter->bap_application_registered) {
		/* Codec switching not supported */
		return -ENOTSUP;
	}

	for (i = 0; codecs[i] != NULL; ++i) {
		if (spa_bt_device_supports_media_codec(device, codecs[i], device->connected_profiles)) {
			preferred_codec = codecs[i];
			break;
		}
	}

	/* Check if we already have an enabled transport for the most preferred codec.
	 * However, if there already was a codec switch running, these transports may
	 * disappear soon. In that case, we have to do the full thing.
	 */
	if (spa_list_is_empty(&device->codec_switch_list) && preferred_codec != NULL) {
		spa_list_for_each(t, &device->transport_list, device_link) {
			if (t->media_codec != preferred_codec)
				continue;

			if ((device->connected_profiles & t->profile) != t->profile)
				continue;

			spa_bt_device_emit_codec_switched(device, 0);
			return 0;
		}
	}

	/* Setup and start iteration */

	sw = calloc(1, sizeof(struct spa_bt_media_codec_switch));
	if (sw == NULL)
		return -ENOMEM;

	num_eps = 0;
	spa_list_for_each(ep, &device->remote_endpoint_list, device_link)
		++num_eps;

	num_codecs = 0;
	while (codecs[num_codecs] != NULL)
		++num_codecs;

	sw->codecs = calloc(num_codecs + 1, sizeof(const struct media_codec *));
	sw->paths = calloc(num_eps + 1, sizeof(char *));
	sw->num_paths = num_eps;

	if (sw->codecs == NULL || sw->paths == NULL) {
		media_codec_switch_free(sw);
		return -ENOMEM;
	}

	for (i = 0, j = 0; i < num_codecs; ++i) {
		if (is_media_codec_enabled(device->monitor, codecs[i])) {
			sw->codecs[j] = codecs[i];
			++j;
		}
	}
	sw->codecs[j] = NULL;

	i = 0;
	spa_list_for_each(ep, &device->remote_endpoint_list, device_link) {
		sw->paths[i] = strdup(ep->path);
		if (sw->paths[i] == NULL) {
			media_codec_switch_free(sw);
			return -ENOMEM;
		}
		++i;
	}
	sw->paths[i] = NULL;

	sw->codec_iter = sw->codecs;
	sw->path_iter = sw->paths;
	sw->retries = CODEC_SWITCH_RETRIES;

	sw->profile = device->connected_profiles;

	sw->device = device;

	if (!spa_list_is_empty(&device->codec_switch_list)) {
		/*
		 * There's a codec switch already running, either waiting for timeout or
		 * BlueZ reply.
		 *
		 * BlueZ does not appear to allow calling dbus_pending_call_cancel on an
		 * active request, so we have to wait for the reply to arrive first, and
		 * only then start processing this request. The timeout we would also have
		 * to wait to pass in any case, so we don't cancel it either.
		 */
		spa_log_debug(sw->device->monitor->log,
				"media codec switch %p: already in progress, canceling previous",
				sw);

		spa_list_prepend(&device->codec_switch_list, &sw->device_link);
	} else {
		spa_list_prepend(&device->codec_switch_list, &sw->device_link);
		media_codec_switch_process(sw);
	}

	return 0;
}

int spa_bt_device_ensure_hfp_codec(struct spa_bt_device *device, unsigned int codec)
{
	struct spa_bt_monitor *monitor = device->monitor;
	return spa_bt_backend_ensure_codec(monitor->backend, device, codec);
}

int spa_bt_device_supports_hfp_codec(struct spa_bt_device *device, unsigned int codec)
{
	struct spa_bt_monitor *monitor = device->monitor;
	return spa_bt_backend_supports_codec(monitor->backend, device, codec);
}

static DBusHandlerResult endpoint_set_configuration(DBusConnection *conn,
		const char *path, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	const char *transport_path, *endpoint;
	DBusMessageIter it[2];
	spa_autoptr(DBusMessage) r = NULL;
	struct spa_bt_transport *transport;
	const struct media_codec *codec;
	int profile;
	bool sink;

	if (!dbus_message_has_signature(m, "oa{sv}")) {
		spa_log_warn(monitor->log, "invalid SetConfiguration() signature");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}
	endpoint = dbus_message_get_path(m);

	profile = media_endpoint_to_profile(endpoint);
	codec = media_endpoint_to_codec(monitor, endpoint, &sink, NULL);
	if (codec == NULL) {
		spa_log_warn(monitor->log, "unknown SetConfiguration() codec");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	dbus_message_iter_init(m, &it[0]);
	dbus_message_iter_get_basic(&it[0], &transport_path);
	dbus_message_iter_next(&it[0]);
	dbus_message_iter_recurse(&it[0], &it[1]);

	transport = spa_bt_transport_find(monitor, transport_path);

	if (transport == NULL) {
		char *tpath = strdup(transport_path);

		transport = spa_bt_transport_create(monitor, tpath, 0);
		if (transport == NULL) {
			free(tpath);
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		}

		spa_bt_transport_set_implementation(transport, &transport_impl, transport);

		if (profile & SPA_BT_PROFILE_A2DP_SOURCE) {
			transport->volumes[SPA_BT_VOLUME_ID_RX].volume = DEFAULT_AG_VOLUME;
			transport->volumes[SPA_BT_VOLUME_ID_TX].volume = DEFAULT_AG_VOLUME;
		} else {
			transport->volumes[SPA_BT_VOLUME_ID_RX].volume = DEFAULT_RX_VOLUME;
			transport->volumes[SPA_BT_VOLUME_ID_TX].volume = DEFAULT_TX_VOLUME;
		}
	}

	for (int i = 0; i < SPA_BT_VOLUME_ID_TERM; ++i) {
		transport->volumes[i].hw_volume = SPA_BT_VOLUME_INVALID;
		transport->volumes[i].hw_volume_max = SPA_BT_VOLUME_A2DP_MAX;
	}

	free(transport->endpoint_path);
	transport->endpoint_path = strdup(endpoint);
	transport->profile = profile;
	transport->media_codec = codec;
	transport_update_props(transport, &it[1], NULL);

	if (transport->device == NULL || transport->device->adapter == NULL) {
		spa_log_warn(monitor->log, "no device found for transport");
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	/* If multiple codecs share the endpoint, pick the one we wanted */
	transport->media_codec = codec = media_endpoint_to_codec(monitor, endpoint, &sink,
			transport->device->preferred_codec);
	spa_assert(codec != NULL);
	spa_log_debug(monitor->log, "%p: %s codec:%s", monitor, path, codec ? codec->name : "<null>");

	spa_bt_device_update_last_bluez_action_time(transport->device);

	if (profile & SPA_BT_PROFILE_A2DP_SOURCE) {
		/* PW is the rendering device so it's responsible for reporting hardware volume. */
		transport->volumes[SPA_BT_VOLUME_ID_RX].active = true;
	} else if (profile & SPA_BT_PROFILE_A2DP_SINK) {
		transport->volumes[SPA_BT_VOLUME_ID_TX].active
			|= transport->device->a2dp_volume_active[SPA_BT_VOLUME_ID_TX];
	}

	if (codec->validate_config) {
		struct spa_audio_info info;
		if (codec->validate_config(codec, sink ? MEDIA_CODEC_FLAG_SINK : 0,
					transport->configuration, transport->configuration_len,
					&info) < 0) {
			spa_log_error(monitor->log, "invalid transport configuration");
			return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
		}
		transport->n_channels = info.info.raw.channels;
		memcpy(transport->channels, info.info.raw.position,
				transport->n_channels * sizeof(uint32_t));
	} else {
		transport->n_channels = 2;
		transport->channels[0] = SPA_AUDIO_CHANNEL_FL;
		transport->channels[1] = SPA_AUDIO_CHANNEL_FR;
	}
	spa_log_info(monitor->log, "%p: %s validate conf channels:%d",
			monitor, path, transport->n_channels);

	spa_bt_device_add_profile(transport->device, transport->profile);

	spa_bt_device_connect_profile(transport->device, transport->profile);

	/* Sync initial volumes */
	transport_sync_volume(transport);

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult endpoint_clear_configuration(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	spa_auto(DBusError) err = DBUS_ERROR_INIT;
	spa_autoptr(DBusMessage) r = NULL;
	const char *transport_path;
	struct spa_bt_transport *transport;

	if (!dbus_message_get_args(m, &err,
				   DBUS_TYPE_OBJECT_PATH, &transport_path,
				   DBUS_TYPE_INVALID)) {
		spa_log_warn(monitor->log, "Bad ClearConfiguration method call: %s",
			err.message);
		return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
	}

	transport = spa_bt_transport_find(monitor, transport_path);

	if (transport != NULL) {
		struct spa_bt_device *device = transport->device;

		spa_log_debug(monitor->log, "transport %p: free %s",
			transport, transport->path);

		spa_bt_transport_free(transport);
		if (device != NULL)
			spa_bt_device_check_profiles(device, false);
	}

	if ((r = dbus_message_new_method_return(m)) == NULL)
		return DBUS_HANDLER_RESULT_NEED_MEMORY;
	if (!dbus_connection_send(conn, r, NULL))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult endpoint_release(DBusConnection *conn, DBusMessage *m, void *userdata)
{
	if (!reply_with_error(conn, m, BLUEZ_MEDIA_ENDPOINT_INTERFACE ".Error.NotImplemented", "Method not implemented"))
		return DBUS_HANDLER_RESULT_NEED_MEMORY;

	return DBUS_HANDLER_RESULT_HANDLED;
}

static DBusHandlerResult endpoint_handler(DBusConnection *c, DBusMessage *m, void *userdata)
{
	struct spa_bt_monitor *monitor = userdata;
	const char *path, *interface, *member;
	DBusHandlerResult res;

	path = dbus_message_get_path(m);
	interface = dbus_message_get_interface(m);
	member = dbus_message_get_member(m);

	spa_log_debug(monitor->log, "dbus: path=%s, interface=%s, member=%s", path, interface, member);

	if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
		const char *xml = ENDPOINT_INTROSPECT_XML;
		spa_autoptr(DBusMessage) r = NULL;

		if ((r = dbus_message_new_method_return(m)) == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_connection_send(monitor->conn, r, NULL))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		res = DBUS_HANDLER_RESULT_HANDLED;
	}
	else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "SetConfiguration"))
		res = endpoint_set_configuration(c, path, m, userdata);
	else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "SelectConfiguration"))
		res = endpoint_select_configuration(c, m, userdata);
	else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "SelectProperties"))
		res = endpoint_select_properties(c, m, userdata);
	else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "ClearConfiguration"))
		res = endpoint_clear_configuration(c, m, userdata);
	else if (dbus_message_is_method_call(m, BLUEZ_MEDIA_ENDPOINT_INTERFACE, "Release"))
		res = endpoint_release(c, m, userdata);
	else
		res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	return res;
}

static void bluez_register_endpoint_legacy_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_adapter *adapter = user_data;
	struct spa_bt_monitor *monitor = adapter->monitor;

	spa_autoptr(DBusMessage) r = steal_reply_and_unref(&pending);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(monitor->log, "BlueZ D-Bus ObjectManager not available");
		return;
	}
	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(monitor->log, "RegisterEndpoint() failed: %s",
				dbus_message_get_error_name(r));
		return;
	}

	adapter->legacy_endpoints_registered = true;
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

static void append_basic_array_variant_dict_entry(DBusMessageIter *dict, const char* key, const char* variant_type_str, const char* array_type_str, int array_type_int, void* data, int data_size) {
	DBusMessageIter dict_entry_it, variant_it, array_it;
	dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, NULL, &dict_entry_it);
	dbus_message_iter_append_basic(&dict_entry_it, DBUS_TYPE_STRING, &key);

	dbus_message_iter_open_container(&dict_entry_it, DBUS_TYPE_VARIANT, variant_type_str, &variant_it);
	dbus_message_iter_open_container(&variant_it, DBUS_TYPE_ARRAY, array_type_str, &array_it);
	dbus_message_iter_append_fixed_array (&array_it, array_type_int, &data, data_size);
	dbus_message_iter_close_container(&variant_it, &array_it);
	dbus_message_iter_close_container(&dict_entry_it, &variant_it);
	dbus_message_iter_close_container(dict, &dict_entry_it);
}

static int bluez_register_endpoint_legacy(struct spa_bt_adapter *adapter,
				   enum spa_bt_media_direction direction,
				   const char *uuid, const struct media_codec *codec)
{
	struct spa_bt_monitor *monitor = adapter->monitor;
	const char *path = adapter->path;
	spa_autofree char *object_path = NULL;
	spa_autoptr(DBusMessage) m = NULL;
	DBusMessageIter object_it, dict_it;
	uint8_t caps[A2DP_MAX_CAPS_SIZE];
	int ret, caps_size;
	uint16_t codec_id = codec->codec_id;
	bool sink = (direction == SPA_BT_MEDIA_SINK);

	spa_assert(codec->fill_caps);

	ret = media_codec_to_endpoint(codec, direction, &object_path);
	if (ret < 0)
		return ret;

	ret = caps_size = codec->fill_caps(codec, sink ? MEDIA_CODEC_FLAG_SINK : 0, caps);
	if (ret < 0)
		return ret;

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
	                                 path,
	                                 BLUEZ_MEDIA_INTERFACE,
	                                 "RegisterEndpoint");
	if (m == NULL)
		return -EIO;

	dbus_message_iter_init_append(m, &object_it);
	dbus_message_iter_append_basic(&object_it, DBUS_TYPE_OBJECT_PATH, &object_path);

	dbus_message_iter_open_container(&object_it, DBUS_TYPE_ARRAY, "{sv}", &dict_it);

	append_basic_variant_dict_entry(&dict_it,"UUID", DBUS_TYPE_STRING, "s", &uuid);
	append_basic_variant_dict_entry(&dict_it, "Codec", DBUS_TYPE_BYTE, "y", &codec_id);
	append_basic_array_variant_dict_entry(&dict_it, "Capabilities", "ay", "y", DBUS_TYPE_BYTE, caps, caps_size);

	dbus_message_iter_close_container(&object_it, &dict_it);

	if (!send_with_reply(monitor->conn, m, bluez_register_endpoint_legacy_reply, adapter))
		return -EIO;

	return 0;
}

static int adapter_register_endpoints_legacy(struct spa_bt_adapter *a)
{
	struct spa_bt_monitor *monitor = a->monitor;
	const struct media_codec * const * const media_codecs = monitor->media_codecs;
	int i;
	int err = 0;
	bool registered = false;

	if (a->legacy_endpoints_registered)
	    return err;

	/* The legacy bluez5 api doesn't support codec switching
	 * It doesn't make sense to register codecs other than SBC
	 * as bluez5 will probably use SBC anyway and we have no control over it
	 * let's incentivize users to upgrade their bluez5 daemon
	 * if they want proper media codec support
	 * */
	spa_log_warn(monitor->log,
		     "Using legacy bluez5 API for A2DP - only SBC will be supported. "
		     "Please upgrade bluez5.");

	for (i = 0; media_codecs[i]; i++) {
		const struct media_codec *codec = media_codecs[i];

		if (codec->id != SPA_BLUETOOTH_AUDIO_CODEC_SBC)
			continue;

		if (endpoint_should_be_registered(monitor, codec, SPA_BT_MEDIA_SOURCE)) {
			if ((err = bluez_register_endpoint_legacy(a, SPA_BT_MEDIA_SOURCE,
									SPA_BT_UUID_A2DP_SOURCE,
									codec)))
				goto out;
		}

		if (endpoint_should_be_registered(monitor, codec, SPA_BT_MEDIA_SINK)) {
			if ((err = bluez_register_endpoint_legacy(a, SPA_BT_MEDIA_SINK,
									SPA_BT_UUID_A2DP_SINK,
									codec)))
				goto out;
		}

		registered = true;
		break;
	}

	if (!registered) {
		/* Should never happen as SBC support is always enabled */
		spa_log_error(monitor->log, "Broken PipeWire build - unable to locate SBC codec");
		err = -ENOSYS;
	}

out:
	if (err) {
		spa_log_error(monitor->log, "Failed to register bluez5 endpoints");
	}
	return err;
}

static void append_media_object(DBusMessageIter *iter, const char *endpoint,
		const char *uuid, uint8_t codec_id, uint8_t *caps, size_t caps_size)
{
	const char *interface_name = BLUEZ_MEDIA_ENDPOINT_INTERFACE;
	DBusMessageIter object, array, entry, dict;

	dbus_message_iter_open_container(iter, DBUS_TYPE_DICT_ENTRY, NULL, &object);
	dbus_message_iter_append_basic(&object, DBUS_TYPE_OBJECT_PATH, &endpoint);

	dbus_message_iter_open_container(&object, DBUS_TYPE_ARRAY, "{sa{sv}}", &array);

	dbus_message_iter_open_container(&array, DBUS_TYPE_DICT_ENTRY, NULL, &entry);
	dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &interface_name);

	dbus_message_iter_open_container(&entry, DBUS_TYPE_ARRAY, "{sv}", &dict);

	append_basic_variant_dict_entry(&dict, "UUID", DBUS_TYPE_STRING, "s", &uuid);
	append_basic_variant_dict_entry(&dict, "Codec", DBUS_TYPE_BYTE, "y", &codec_id);
	append_basic_array_variant_dict_entry(&dict, "Capabilities", "ay", "y", DBUS_TYPE_BYTE, caps, caps_size);

	if (spa_bt_profile_from_uuid(uuid) & SPA_BT_PROFILE_A2DP_SOURCE) {
		dbus_bool_t delay_reporting = TRUE;

		append_basic_variant_dict_entry(&dict, "DelayReporting", DBUS_TYPE_BOOLEAN, "b", &delay_reporting);
	}
	if (spa_bt_profile_from_uuid(uuid) & (SPA_BT_PROFILE_BAP_SINK | SPA_BT_PROFILE_BAP_SOURCE)) {
		dbus_uint32_t locations;
		dbus_uint16_t supported_context, context;

		locations = BAP_CHANNEL_ALL;
		if (spa_bt_profile_from_uuid(uuid) & SPA_BT_PROFILE_BAP_SINK) {
			supported_context = context = BAP_CONTEXT_ALL;
		} else {
			supported_context = context = (BAP_CONTEXT_UNSPECIFIED | BAP_CONTEXT_CONVERSATIONAL |
					BAP_CONTEXT_MEDIA | BAP_CONTEXT_GAME);
		}

		append_basic_variant_dict_entry(&dict, "Locations", DBUS_TYPE_UINT32, "u", &locations);
		append_basic_variant_dict_entry(&dict, "Context", DBUS_TYPE_UINT16, "q", &context);
		append_basic_variant_dict_entry(&dict, "SupportedContext", DBUS_TYPE_UINT16, "q", &supported_context);
	}

	dbus_message_iter_close_container(&entry, &dict);
	dbus_message_iter_close_container(&array, &entry);
	dbus_message_iter_close_container(&object, &array);
	dbus_message_iter_close_container(iter, &object);
}

static DBusHandlerResult object_manager_handler(DBusConnection *c, DBusMessage *m, void *user_data, bool is_bap)
{
	struct spa_bt_monitor *monitor = user_data;
	const struct media_codec * const * const media_codecs = monitor->media_codecs;
	const char *path, *interface, *member;
	DBusMessageIter iter, array;
	DBusHandlerResult res;
	int i;

	path = dbus_message_get_path(m);
	interface = dbus_message_get_interface(m);
	member = dbus_message_get_member(m);

	spa_log_debug(monitor->log, "dbus: path=%s, interface=%s, member=%s", path, interface, member);

	if (dbus_message_is_method_call(m, "org.freedesktop.DBus.Introspectable", "Introspect")) {
		const char *xml = OBJECT_MANAGER_INTROSPECT_XML;
		spa_autoptr(DBusMessage) r = NULL;

		if ((r = dbus_message_new_method_return(m)) == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_message_append_args(r, DBUS_TYPE_STRING, &xml, DBUS_TYPE_INVALID))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		if (!dbus_connection_send(monitor->conn, r, NULL))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		res = DBUS_HANDLER_RESULT_HANDLED;
	}
	else if (dbus_message_is_method_call(m, "org.freedesktop.DBus.ObjectManager", "GetManagedObjects")) {
		spa_autoptr(DBusMessage) r = NULL;
		struct spa_bt_adapter *a;
		bool register_bcast = false;

		if ((r = dbus_message_new_method_return(m)) == NULL)
			return DBUS_HANDLER_RESULT_NEED_MEMORY;

		dbus_message_iter_init_append(r, &iter);
		dbus_message_iter_open_container(&iter, DBUS_TYPE_ARRAY, "{oa{sa{sv}}}", &array);

		/*
		 * Verify if an adapter exists that supports bap broadcast.
		 * If this adapter exists will register the broadcast endpoint.
		 */
		spa_list_for_each(a, &monitor->adapter_list, link) {
			if (a->le_audio_bcast_supported) {
					register_bcast = true;
					break;
				}
		}

		for (i = 0; media_codecs[i]; i++) {
			const struct media_codec *codec = media_codecs[i];
			uint8_t caps[A2DP_MAX_CAPS_SIZE];
			int caps_size, ret;
			uint16_t codec_id = codec->codec_id;

			if (codec->bap != is_bap)
				continue;

			if (!is_media_codec_enabled(monitor, codec))
				continue;

			if (endpoint_should_be_registered(monitor, codec, SPA_BT_MEDIA_SINK)) {
				caps_size = codec->fill_caps(codec, MEDIA_CODEC_FLAG_SINK, caps);
				if (caps_size < 0)
					continue;

				spa_autofree char *endpoint = NULL;
				ret = media_codec_to_endpoint(codec, SPA_BT_MEDIA_SINK, &endpoint);
				if (ret == 0) {
					spa_log_info(monitor->log, "register media sink codec %s: %s", media_codecs[i]->name, endpoint);
					append_media_object(&array, endpoint,
					        codec->bap ? SPA_BT_UUID_BAP_SINK : SPA_BT_UUID_A2DP_SINK,
							codec_id, caps, caps_size);
				}
			}

			if (endpoint_should_be_registered(monitor, codec, SPA_BT_MEDIA_SOURCE)) {
				caps_size = codec->fill_caps(codec, 0, caps);
				if (caps_size < 0)
					continue;

				spa_autofree char *endpoint = NULL;
				ret = media_codec_to_endpoint(codec, SPA_BT_MEDIA_SOURCE, &endpoint);
				if (ret == 0) {
					spa_log_info(monitor->log, "register media source codec %s: %s", media_codecs[i]->name, endpoint);
					append_media_object(&array, endpoint,
					        codec->bap ? SPA_BT_UUID_BAP_SOURCE : SPA_BT_UUID_A2DP_SOURCE,
							codec_id, caps, caps_size);
				}
			}

			if (codec->bap && register_bcast) {
				if (endpoint_should_be_registered(monitor, codec, SPA_BT_MEDIA_SOURCE_BROADCAST)) {
					caps_size = codec->fill_caps(codec, 0, caps);
					if (caps_size < 0)
						continue;

					spa_autofree char *endpoint = NULL;
					ret = media_codec_to_endpoint(codec, SPA_BT_MEDIA_SOURCE_BROADCAST, &endpoint);
						if (ret == 0) {
							spa_log_info(monitor->log, "register media source codec %s: %s", media_codecs[i]->name, endpoint);
							append_media_object(&array, endpoint,
									SPA_BT_UUID_BAP_BROADCAST_SOURCE,
									codec_id, caps, caps_size);
						}
				}
				
				if (endpoint_should_be_registered(monitor, codec, SPA_BT_MEDIA_SINK_BROADCAST)) {
					caps_size = codec->fill_caps(codec, MEDIA_CODEC_FLAG_SINK, caps);
					if (caps_size < 0)
						continue;

					spa_autofree char *endpoint = NULL;
					ret = media_codec_to_endpoint(codec, SPA_BT_MEDIA_SINK_BROADCAST, &endpoint);
					if (ret == 0) {
						spa_log_info(monitor->log, "register broadcast media sink codec %s: %s", media_codecs[i]->name, endpoint);
						append_media_object(&array, endpoint,
								SPA_BT_UUID_BAP_BROADCAST_SINK,
								codec_id, caps, caps_size);
					}
				}
			}
		}

		dbus_message_iter_close_container(&iter, &array);
		if (!dbus_connection_send(monitor->conn, r, NULL))
			return DBUS_HANDLER_RESULT_NEED_MEMORY;
		res = DBUS_HANDLER_RESULT_HANDLED;
	}
	else
		res = DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

	return res;
}

static DBusHandlerResult object_manager_handler_a2dp(DBusConnection *c, DBusMessage *m, void *user_data)
{
	return object_manager_handler(c, m, user_data, false);
}

static DBusHandlerResult object_manager_handler_bap(DBusConnection *c, DBusMessage *m, void *user_data)
{
	return object_manager_handler(c, m, user_data, true);
}

static void bluez_register_application_a2dp_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_adapter *adapter = user_data;
	struct spa_bt_monitor *monitor = adapter->monitor;
	bool fallback = true;

	spa_autoptr(DBusMessage) r = steal_reply_and_unref(&pending);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, BLUEZ_ERROR_NOT_SUPPORTED)) {
		spa_log_warn(monitor->log, "Registering media applications for adapter %s is disabled in bluez5", adapter->path);
		goto finish;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(monitor->log, "RegisterApplication() failed: %s",
		        dbus_message_get_error_name(r));
		goto finish;
	}

	fallback = false;
	adapter->a2dp_application_registered = true;

finish:
	if (fallback)
		adapter_register_endpoints_legacy(adapter);
}

static void bluez_register_application_bap_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_adapter *adapter = user_data;
	struct spa_bt_monitor *monitor = adapter->monitor;

	spa_autoptr(DBusMessage) r = steal_reply_and_unref(&pending);
	if (r == NULL)
		return;

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(monitor->log, "RegisterApplication() failed: %s",
		        dbus_message_get_error_name(r));
		return;
	}

	adapter->bap_application_registered = true;
}

static int register_media_endpoint(struct spa_bt_monitor *monitor,
				   const struct media_codec *codec,
				   enum spa_bt_media_direction direction)
{
	static const DBusObjectPathVTable vtable_endpoint = {
		.message_function = endpoint_handler,
	};

	if (!endpoint_should_be_registered(monitor, codec, direction))
		return 0;

	spa_autofree char *object_path = NULL;
	int ret = media_codec_to_endpoint(codec, direction, &object_path);
	if (ret < 0)
		return ret;

	spa_log_info(monitor->log, "Registering DBus media endpoint: %s", object_path);

	if (!dbus_connection_register_object_path(monitor->conn,
						  object_path,
						  &vtable_endpoint, monitor))
		return -EIO;

	return 0;
}

static int register_media_application(struct spa_bt_monitor * monitor)
{
	const struct media_codec * const * const media_codecs = monitor->media_codecs;
	const DBusObjectPathVTable vtable_object_manager_a2dp = {
		.message_function = object_manager_handler_a2dp,
	};
	const DBusObjectPathVTable vtable_object_manager_bap = {
		.message_function = object_manager_handler_bap,
	};

	spa_log_info(monitor->log, "Registering DBus media object manager: %s",
			A2DP_OBJECT_MANAGER_PATH);

	if (!dbus_connection_register_object_path(monitor->conn,
	                                          A2DP_OBJECT_MANAGER_PATH,
	                                          &vtable_object_manager_a2dp, monitor))
		return -EIO;

	spa_log_info(monitor->log, "Registering DBus media object manager: %s",
			BAP_OBJECT_MANAGER_PATH);

	if (!dbus_connection_register_object_path(monitor->conn,
	                                          BAP_OBJECT_MANAGER_PATH,
	                                          &vtable_object_manager_bap, monitor))
		return -EIO;

	for (int i = 0; media_codecs[i]; i++) {
		const struct media_codec *codec = media_codecs[i];

		register_media_endpoint(monitor, codec, SPA_BT_MEDIA_SOURCE);
		register_media_endpoint(monitor, codec, SPA_BT_MEDIA_SINK);
		if (codec->bap) {
			register_media_endpoint(monitor, codec, SPA_BT_MEDIA_SOURCE_BROADCAST);
			register_media_endpoint(monitor, codec, SPA_BT_MEDIA_SINK_BROADCAST);
		}
	}

	return 0;
}

static void unregister_media_endpoint(struct spa_bt_monitor *monitor,
				      const struct media_codec *codec,
				      enum spa_bt_media_direction direction)
{
	if (!endpoint_should_be_registered(monitor, codec, direction))
		return;

	spa_autofree char *object_path = NULL;
	int ret = media_codec_to_endpoint(codec, direction, &object_path);
	if (ret < 0)
		return;

	spa_log_info(monitor->log, "unregistering endpoint: %s", object_path);

	if (!dbus_connection_unregister_object_path(monitor->conn, object_path))
		spa_log_warn(monitor->log, "failed to unregister %s\n", object_path);
}

static void unregister_media_application(struct spa_bt_monitor * monitor)
{
	const struct media_codec * const * const media_codecs = monitor->media_codecs;

	for (int i = 0; media_codecs[i]; i++) {
		const struct media_codec *codec = media_codecs[i];

		unregister_media_endpoint(monitor, codec, SPA_BT_MEDIA_SOURCE);
		unregister_media_endpoint(monitor, codec, SPA_BT_MEDIA_SINK);
		if (codec->bap) {
			unregister_media_endpoint(monitor, codec, SPA_BT_MEDIA_SOURCE_BROADCAST);
			unregister_media_endpoint(monitor, codec, SPA_BT_MEDIA_SINK_BROADCAST);
		}
	}

	dbus_connection_unregister_object_path(monitor->conn, BAP_OBJECT_MANAGER_PATH);
	dbus_connection_unregister_object_path(monitor->conn, A2DP_OBJECT_MANAGER_PATH);
}

static bool have_codec_endpoints(struct spa_bt_monitor *monitor, bool bap)
{
	const struct media_codec * const * const media_codecs = monitor->media_codecs;
	int i;

	for (i = 0; media_codecs[i]; i++) {
		const struct media_codec *codec = media_codecs[i];

		if (codec->bap != bap)
			continue;
		if (endpoint_should_be_registered(monitor, codec, SPA_BT_MEDIA_SINK) ||
				endpoint_should_be_registered(monitor, codec, SPA_BT_MEDIA_SOURCE) ||
				endpoint_should_be_registered(monitor, codec, SPA_BT_MEDIA_SOURCE_BROADCAST) || 
				endpoint_should_be_registered(monitor, codec, SPA_BT_MEDIA_SINK_BROADCAST))
			return true;
	}
	return false;
}

static int adapter_register_application(struct spa_bt_adapter *a, bool bap)
{
	const char *object_manager_path = bap ? BAP_OBJECT_MANAGER_PATH : A2DP_OBJECT_MANAGER_PATH;
	struct spa_bt_monitor *monitor = a->monitor;
	const char *ep_type_name = (bap ? "LE Audio" : "A2DP");
	spa_autoptr(DBusMessage) m = NULL;
	DBusMessageIter i, d;

	if (bap && a->bap_application_registered)
		return 0;
	if (!bap && a->a2dp_application_registered)
		return 0;

	if ((bap && !a->le_audio_supported) && (bap && !a->le_audio_bcast_supported)) {
		spa_log_info(monitor->log, "Adapter %s indicates LE Audio unsupported: not registering application",
				a->path);
		return -ENOTSUP;
	}

	if (!have_codec_endpoints(monitor, bap)) {
		spa_log_warn(monitor->log, "No available %s codecs to register on adapter %s",
				ep_type_name, a->path);
		return -ENOENT;
	}

	spa_log_debug(monitor->log, "Registering bluez5 %s media application on adapter %s",
			ep_type_name, a->path);

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
	                                 a->path,
	                                 BLUEZ_MEDIA_INTERFACE,
	                                 "RegisterApplication");
	if (m == NULL)
		return -EIO;

	dbus_message_iter_init_append(m, &i);
	dbus_message_iter_append_basic(&i, DBUS_TYPE_OBJECT_PATH, &object_manager_path);
	dbus_message_iter_open_container(&i, DBUS_TYPE_ARRAY, "{sv}", &d);
	dbus_message_iter_close_container(&i, &d);

	if (!send_with_reply(monitor->conn, m, bap ? bluez_register_application_bap_reply : bluez_register_application_a2dp_reply, a))
		return -EIO;

	return 0;
}

static int switch_backend(struct spa_bt_monitor *monitor, struct spa_bt_backend *backend)
{
	int res;
	size_t i;

	spa_return_val_if_fail(backend != NULL, -EINVAL);

	if (!backend->available)
		return -ENODEV;

	for (i = 0; i < SPA_N_ELEMENTS(monitor->backends); ++i) {
		struct spa_bt_backend *b = monitor->backends[i];
		if (backend != b && b && b->available && b->exclusive)
			spa_log_warn(monitor->log,
					"%s running, but not configured as HFP/HSP backend: "
					"it may interfere with HFP/HSP functionality.",
					b->name);
	}

	if (monitor->backend == backend)
		return 0;

	spa_log_info(monitor->log, "Switching to HFP/HSP backend %s", backend->name);

	spa_bt_backend_unregister_profiles(monitor->backend);

	if ((res = spa_bt_backend_register_profiles(backend)) < 0) {
		monitor->backend = NULL;
		return res;
	}

	monitor->backend = backend;
	return 0;
}

static void reselect_backend(struct spa_bt_monitor *monitor, bool silent)
{
	struct spa_bt_backend *backend;
	size_t i;

	spa_log_debug(monitor->log, "re-selecting HFP/HSP backend");

	if (monitor->backend_selection == BACKEND_NONE) {
		spa_bt_backend_unregister_profiles(monitor->backend);
		monitor->backend = NULL;
		return;
	} else if (monitor->backend_selection == BACKEND_ANY) {
		for (i = 0; i < SPA_N_ELEMENTS(monitor->backends); ++i) {
			backend = monitor->backends[i];
			if (backend && switch_backend(monitor, backend) == 0)
				return;
		}
	} else {
		backend = monitor->backends[monitor->backend_selection];
		if (backend && switch_backend(monitor, backend) == 0)
			return;
	}

	spa_bt_backend_unregister_profiles(monitor->backend);
	monitor->backend = NULL;

	if (!silent)
		spa_log_error(monitor->log, "Failed to start HFP/HSP backend %s",
				backend ? backend->name : "none");
}

static void interface_added(struct spa_bt_monitor *monitor,
			    DBusConnection *conn,
			    const char *object_path,
			    const char *interface_name,
			    DBusMessageIter *props_iter)
{
	spa_log_debug(monitor->log, "Found object %s, interface %s", object_path, interface_name);

	if (spa_streq(interface_name, BLUEZ_ADAPTER_INTERFACE) ||
			spa_streq(interface_name, BLUEZ_MEDIA_INTERFACE)) {
		struct spa_bt_adapter *a;

		a = adapter_find(monitor, object_path);
		if (a == NULL) {
			a = adapter_create(monitor, object_path);
			if (a == NULL) {
				spa_log_warn(monitor->log, "can't create adapter: %m");
				return;
			}
		}

		if (spa_streq(interface_name, BLUEZ_ADAPTER_INTERFACE)) {
			adapter_update_props(a, props_iter, NULL);
			a->has_adapter1_interface = true;
		} else {
			adapter_media_update_props(a, props_iter, NULL);
			a->has_media1_interface = true;
		}

		if (a->has_adapter1_interface && a->has_media1_interface) {
			adapter_register_application(a, false);
			adapter_register_application(a, true);
			adapter_register_player(a);
			adapter_update_devices(a);
		}
	}
	else if (spa_streq(interface_name, BLUEZ_PROFILE_MANAGER_INTERFACE)) {
		if (monitor->backends[BACKEND_NATIVE])
			monitor->backends[BACKEND_NATIVE]->available = true;
		reselect_backend(monitor, false);
	}
	else if (spa_streq(interface_name, BLUEZ_DEVICE_INTERFACE)) {
		struct spa_bt_device *d;

		d = spa_bt_device_find(monitor, object_path);
		if (d == NULL) {
			d = device_create(monitor, object_path);
			if (d == NULL) {
				spa_log_warn(monitor->log, "can't create Bluetooth device %s: %m",
						object_path);
				return;
			}
		}

		device_update_props(d, props_iter, NULL);
		d->reconnect_state = BT_DEVICE_RECONNECT_INIT;

		if (!device_props_ready(d))
			return;

		device_update_hw_volume_profiles(d);

		/* Trigger bluez device creation before bluez profile negotiation started so that
		 * profile connection handlers can receive per-device settings during profile negotiation. */
		spa_bt_device_add_profile(d, SPA_BT_PROFILE_NULL);
	}
	else if (spa_streq(interface_name, BLUEZ_DEVICE_SET_INTERFACE)) {
		device_set_update_props(monitor, object_path, props_iter, NULL);
	}
	else if (spa_streq(interface_name, BLUEZ_MEDIA_ENDPOINT_INTERFACE)) {
		struct spa_bt_remote_endpoint *ep;
		struct spa_bt_device *d;

		ep = remote_endpoint_find(monitor, object_path);
		if (ep == NULL) {
			ep = remote_endpoint_create(monitor, object_path);
			if (ep == NULL) {
				spa_log_warn(monitor->log, "can't create Bluetooth remote endpoint %s: %m",
				             object_path);
				return;
			}
		}
		remote_endpoint_update_props(ep, props_iter, NULL);

		d = ep->device;
		if (d)
			spa_bt_device_emit_profiles_changed(d, d->profiles, d->connected_profiles);
	}
}

static void interfaces_added(struct spa_bt_monitor *monitor, DBusMessageIter *arg_iter)
{
	DBusMessageIter it[3];
	const char *object_path;

	dbus_message_iter_get_basic(arg_iter, &object_path);
	dbus_message_iter_next(arg_iter);
	dbus_message_iter_recurse(arg_iter, &it[0]);

	while (dbus_message_iter_get_arg_type(&it[0]) != DBUS_TYPE_INVALID) {
		const char *interface_name;

		dbus_message_iter_recurse(&it[0], &it[1]);
		dbus_message_iter_get_basic(&it[1], &interface_name);
		dbus_message_iter_next(&it[1]);
		dbus_message_iter_recurse(&it[1], &it[2]);

		interface_added(monitor, monitor->conn,
				object_path, interface_name,
				&it[2]);

		dbus_message_iter_next(&it[0]);
	}
}

static void interfaces_removed(struct spa_bt_monitor *monitor, DBusMessageIter *arg_iter)
{
	const char *object_path;
	DBusMessageIter it;

	dbus_message_iter_get_basic(arg_iter, &object_path);
	dbus_message_iter_next(arg_iter);
	dbus_message_iter_recurse(arg_iter, &it);

	while (dbus_message_iter_get_arg_type(&it) != DBUS_TYPE_INVALID) {
		const char *interface_name;

		dbus_message_iter_get_basic(&it, &interface_name);

		spa_log_debug(monitor->log, "Found object %s, interface %s", object_path, interface_name);

		if (spa_streq(interface_name, BLUEZ_DEVICE_INTERFACE)) {
			struct spa_bt_device *d;
			d = spa_bt_device_find(monitor, object_path);
			if (d != NULL)
				device_free(d);
		} else if (spa_streq(interface_name, BLUEZ_DEVICE_SET_INTERFACE)) {
			device_set_update_props(monitor, object_path, NULL, NULL);
		} else if (spa_streq(interface_name, BLUEZ_ADAPTER_INTERFACE) ||
				spa_streq(interface_name, BLUEZ_MEDIA_INTERFACE)) {
			struct spa_bt_adapter *a;
			a = adapter_find(monitor, object_path);
			if (a != NULL)
				adapter_free(a);
		} else if (spa_streq(interface_name, BLUEZ_MEDIA_ENDPOINT_INTERFACE)) {
			struct spa_bt_remote_endpoint *ep;
			ep = remote_endpoint_find(monitor, object_path);
			if (ep != NULL) {
				struct spa_bt_device *d = ep->device;
				remote_endpoint_free(ep);
				if (d)
					spa_bt_device_emit_profiles_changed(d, d->profiles, d->connected_profiles);
			}
		}

		dbus_message_iter_next(&it);
	}
}

static void get_managed_objects_reply(DBusPendingCall *pending, void *user_data)
{
	struct spa_bt_monitor *monitor = user_data;
	DBusMessageIter it[6];

	spa_assert(monitor->get_managed_objects_call == pending);
	spa_autoptr(DBusMessage) r = steal_reply_and_unref(&monitor->get_managed_objects_call);
	if (r == NULL)
		return;

	if (dbus_message_is_error(r, DBUS_ERROR_UNKNOWN_METHOD)) {
		spa_log_warn(monitor->log, "BlueZ D-Bus ObjectManager not available");
		return;
	}

	if (dbus_message_is_error(r, DBUS_ERROR_NAME_HAS_NO_OWNER)) {
		spa_log_warn(monitor->log, "BlueZ system service is not available");
		return;
	}

	if (dbus_message_get_type(r) == DBUS_MESSAGE_TYPE_ERROR) {
		spa_log_error(monitor->log, "GetManagedObjects() failed: %s",
				dbus_message_get_error_name(r));
		return;
	}

	if (!dbus_message_iter_init(r, &it[0]) ||
	    !spa_streq(dbus_message_get_signature(r), "a{oa{sa{sv}}}")) {
		spa_log_error(monitor->log, "Invalid reply signature for GetManagedObjects()");
		return;
	}

	dbus_message_iter_recurse(&it[0], &it[1]);

	while (dbus_message_iter_get_arg_type(&it[1]) != DBUS_TYPE_INVALID) {
		dbus_message_iter_recurse(&it[1], &it[2]);

		interfaces_added(monitor, &it[2]);

		dbus_message_iter_next(&it[1]);
	}

	reselect_backend(monitor, false);

	monitor->objects_listed = true;
}

static void get_managed_objects(struct spa_bt_monitor *monitor)
{
	if (monitor->objects_listed || monitor->get_managed_objects_call)
		return;

	spa_autoptr(DBusMessage) m = NULL;

	m = dbus_message_new_method_call(BLUEZ_SERVICE,
					 "/",
					 "org.freedesktop.DBus.ObjectManager",
					 "GetManagedObjects");

	dbus_message_set_auto_start(m, false);

	monitor->get_managed_objects_call = send_with_reply(monitor->conn, m, get_managed_objects_reply, monitor);
}

static DBusHandlerResult filter_cb(DBusConnection *bus, DBusMessage *m, void *user_data)
{
	struct spa_bt_monitor *monitor = user_data;

	if (dbus_message_is_signal(m, "org.freedesktop.DBus", "NameOwnerChanged")) {
		const char *name, *old_owner, *new_owner;
		spa_auto(DBusError) err = DBUS_ERROR_INIT;

		spa_log_debug(monitor->log, "Name owner changed %s", dbus_message_get_path(m));

		if (!dbus_message_get_args(m, &err,
					   DBUS_TYPE_STRING, &name,
					   DBUS_TYPE_STRING, &old_owner,
					   DBUS_TYPE_STRING, &new_owner,
					   DBUS_TYPE_INVALID)) {
			spa_log_error(monitor->log, "Failed to parse org.freedesktop.DBus.NameOwnerChanged: %s", err.message);
			goto finish;
		}

		if (spa_streq(name, BLUEZ_SERVICE)) {
			bool has_old_owner = old_owner && *old_owner;
			bool has_new_owner = new_owner && *new_owner;

			if (has_old_owner) {
				spa_log_debug(monitor->log, "Bluetooth daemon disappeared");

				if (monitor->backends[BACKEND_NATIVE])
					monitor->backends[BACKEND_NATIVE]->available = false;

				reselect_backend(monitor, true);
			}

			if (has_old_owner || has_new_owner) {
				struct spa_bt_adapter *a;
				struct spa_bt_device *d;
				struct spa_bt_remote_endpoint *ep;
				struct spa_bt_transport *t;

				monitor->objects_listed = false;

				spa_list_consume(t, &monitor->transport_list, link)
					spa_bt_transport_free(t);
				spa_list_consume(ep, &monitor->remote_endpoint_list, link)
					remote_endpoint_free(ep);
				spa_list_consume(d, &monitor->device_list, link)
					device_free(d);
				spa_list_consume(a, &monitor->adapter_list, link)
					adapter_free(a);
			}

			if (has_new_owner) {
				spa_log_debug(monitor->log, "Bluetooth daemon appeared");
				get_managed_objects(monitor);
			}
		} else if (spa_streq(name, OFONO_SERVICE)) {
			if (monitor->backends[BACKEND_OFONO])
				monitor->backends[BACKEND_OFONO]->available = (new_owner && *new_owner);
			reselect_backend(monitor, false);
		} else if (spa_streq(name, HSPHFPD_SERVICE)) {
			if (monitor->backends[BACKEND_HSPHFPD])
				monitor->backends[BACKEND_HSPHFPD]->available = (new_owner && *new_owner);
			reselect_backend(monitor, false);
		}
	} else if (dbus_message_is_signal(m, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded")) {
		DBusMessageIter it;

		spa_log_debug(monitor->log, "interfaces added %s", dbus_message_get_path(m));

		if (!monitor->objects_listed)
			goto finish;

		if (!dbus_message_iter_init(m, &it) || !spa_streq(dbus_message_get_signature(m), "oa{sa{sv}}")) {
			spa_log_error(monitor->log, "Invalid signature found in InterfacesAdded");
			goto finish;
		}

		interfaces_added(monitor, &it);
	} else if (dbus_message_is_signal(m, "org.freedesktop.DBus.ObjectManager", "InterfacesRemoved")) {
		DBusMessageIter it;

		spa_log_debug(monitor->log, "interfaces removed %s", dbus_message_get_path(m));

		if (!monitor->objects_listed)
			goto finish;

		if (!dbus_message_iter_init(m, &it) || !spa_streq(dbus_message_get_signature(m), "oas")) {
			spa_log_error(monitor->log, "Invalid signature found in InterfacesRemoved");
			goto finish;
		}

		interfaces_removed(monitor, &it);
	} else if (dbus_message_is_signal(m, "org.freedesktop.DBus.Properties", "PropertiesChanged")) {
		DBusMessageIter it[2];
		const char *iface, *path;

		if (!monitor->objects_listed)
			goto finish;

		if (!dbus_message_iter_init(m, &it[0]) ||
		    !spa_streq(dbus_message_get_signature(m), "sa{sv}as")) {
			spa_log_error(monitor->log, "Invalid signature found in PropertiesChanged");
			goto finish;
		}
		path = dbus_message_get_path(m);

		dbus_message_iter_get_basic(&it[0], &iface);
		dbus_message_iter_next(&it[0]);
		dbus_message_iter_recurse(&it[0], &it[1]);

		if (spa_streq(iface, BLUEZ_ADAPTER_INTERFACE) ||
				spa_streq(iface, BLUEZ_MEDIA_INTERFACE)) {
			struct spa_bt_adapter *a;

			a = adapter_find(monitor, path);
			if (a == NULL) {
				spa_log_warn(monitor->log,
						"Properties changed in unknown adapter %s", path);
				goto finish;
			}
			spa_log_debug(monitor->log, "Properties changed in adapter %s", path);

			if (spa_streq(iface, BLUEZ_ADAPTER_INTERFACE))
				adapter_update_props(a, &it[1], NULL);
			else
				adapter_media_update_props(a, &it[1], NULL);
		}
		else if (spa_streq(iface, BLUEZ_DEVICE_INTERFACE)) {
			struct spa_bt_device *d;

			d = spa_bt_device_find(monitor, path);
			if (d == NULL) {
				spa_log_debug(monitor->log,
						"Properties changed in unknown device %s", path);
				goto finish;
			}
			spa_log_debug(monitor->log, "Properties changed in device %s", path);

			device_update_props(d, &it[1], NULL);

			if (!device_props_ready(d))
				goto finish;

			device_update_hw_volume_profiles(d);

			spa_bt_device_add_profile(d, SPA_BT_PROFILE_NULL);
		}
		else if (spa_streq(iface, BLUEZ_DEVICE_SET_INTERFACE)) {
			device_set_update_props(monitor, path, &it[1], NULL);
		}
		else if (spa_streq(iface, BLUEZ_MEDIA_ENDPOINT_INTERFACE)) {
			struct spa_bt_remote_endpoint *ep;
			struct spa_bt_device *d;

			ep = remote_endpoint_find(monitor, path);
			if (ep == NULL) {
				spa_log_debug(monitor->log,
						"Properties changed in unknown remote endpoint %s", path);
				goto finish;
			}
			spa_log_debug(monitor->log, "Properties changed in remote endpoint %s", path);

			remote_endpoint_update_props(ep, &it[1], NULL);

			d = ep->device;
			if (d)
				spa_bt_device_emit_profiles_changed(d, d->profiles, d->connected_profiles);
		}
		else if (spa_streq(iface, BLUEZ_MEDIA_TRANSPORT_INTERFACE)) {
			struct spa_bt_transport *transport;

			transport = spa_bt_transport_find(monitor, path);
			if (transport == NULL) {
				spa_log_warn(monitor->log,
						"Properties changed in unknown transport '%s'. "
						"Multiple sound server instances (PipeWire/Pulseaudio/bluez-alsa) are "
						"probably trying to use Bluetooth audio at the same time, which can "
						"cause problems. The system configuration likely should be fixed "
						"to have only one sound server that manages Bluetooth audio.",
						path);
				goto finish;
			}

			spa_log_debug(monitor->log, "Properties changed in transport %s", path);

			transport_update_props(transport, &it[1], NULL);
		}
	}

finish:
	return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void add_filters(struct spa_bt_monitor *this)
{
	if (this->filters_added)
		return;

	if (!dbus_connection_add_filter(this->conn, filter_cb, this, NULL)) {
		spa_log_error(this->log, "failed to add filter function");
		return;
	}

	spa_auto(DBusError) err = DBUS_ERROR_INIT;

	dbus_bus_add_match(this->conn,
			"type='signal',sender='org.freedesktop.DBus',"
			"interface='org.freedesktop.DBus',member='NameOwnerChanged',"
			"arg0='" BLUEZ_SERVICE "'", &err);
#ifdef HAVE_BLUEZ_5_BACKEND_OFONO
	dbus_bus_add_match(this->conn,
			"type='signal',sender='org.freedesktop.DBus',"
			"interface='org.freedesktop.DBus',member='NameOwnerChanged',"
			"arg0='" OFONO_SERVICE "'", &err);
#endif
#ifdef HAVE_BLUEZ_5_BACKEND_HSPHFPD
	dbus_bus_add_match(this->conn,
			"type='signal',sender='org.freedesktop.DBus',"
			"interface='org.freedesktop.DBus',member='NameOwnerChanged',"
			"arg0='" HSPHFPD_SERVICE "'", &err);
#endif
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
			"arg0='" BLUEZ_MEDIA_INTERFACE "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" BLUEZ_SERVICE "',"
			"interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
			"arg0='" BLUEZ_DEVICE_INTERFACE "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" BLUEZ_SERVICE "',"
			"interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
			"arg0='" BLUEZ_DEVICE_SET_INTERFACE "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" BLUEZ_SERVICE "',"
			"interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
			"arg0='" BLUEZ_MEDIA_ENDPOINT_INTERFACE "'", &err);
	dbus_bus_add_match(this->conn,
			"type='signal',sender='" BLUEZ_SERVICE "',"
			"interface='org.freedesktop.DBus.Properties',member='PropertiesChanged',"
			"arg0='" BLUEZ_MEDIA_TRANSPORT_INTERFACE "'", &err);

	this->filters_added = true;
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

	struct spa_bt_device *device;
	spa_list_for_each(device, &this->device_list, link) {
		if (device->added)
			emit_device_info(this, device, this->connection_info_supported);
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
	struct spa_bt_monitor *this;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	this = (struct spa_bt_monitor *) handle;

	if (spa_streq(type, SPA_TYPE_INTERFACE_Device))
		*interface = &this->device;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct spa_bt_monitor *monitor;
	struct spa_bt_adapter *a;
	struct spa_bt_device *d;
	struct spa_bt_remote_endpoint *ep;
	struct spa_bt_transport *t;
	const struct spa_dict_item *it;
	size_t i;

	monitor = (struct spa_bt_monitor *) handle;

	/*
	 * We don't call BlueZ API unregister methods here, since BlueZ generally does the
	 * unregistration when the DBus connection is closed below.  We'll unregister DBus
	 * object managers and filter callbacks though.
	 */

	unregister_media_application(monitor);

	if (monitor->filters_added) {
		dbus_connection_remove_filter(monitor->conn, filter_cb, monitor);
		monitor->filters_added = false;
	}

	cancel_and_unref(&monitor->get_managed_objects_call);

	spa_list_consume(t, &monitor->transport_list, link)
		spa_bt_transport_free(t);
	spa_list_consume(ep, &monitor->remote_endpoint_list, link)
		remote_endpoint_free(ep);
	spa_list_consume(d, &monitor->device_list, link)
		device_free(d);
	spa_list_consume(a, &monitor->adapter_list, link)
		adapter_free(a);

	for (i = 0; i < SPA_N_ELEMENTS(monitor->backends); ++i) {
		spa_bt_backend_free(monitor->backends[i]);
		monitor->backends[i] = NULL;
	}

	spa_dict_for_each(it, &monitor->global_settings) {
		free((void *)it->key);
		free((void *)it->value);
	}

	free((void*)monitor->enabled_codecs.items);
	spa_zero(monitor->enabled_codecs);

	dbus_connection_unref(monitor->conn);
	spa_dbus_connection_destroy(monitor->dbus_connection);
	monitor->dbus_connection = NULL;
	monitor->conn = NULL;

	monitor->objects_listed = false;

	monitor->connection_info_supported = false;

	monitor->backend = NULL;
	monitor->backend_selection = BACKEND_NATIVE;

	spa_bt_quirks_destroy(monitor->quirks);

	free_media_codecs(monitor->media_codecs);

	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct spa_bt_monitor);
}

int spa_bt_profiles_from_json_array(const char *str)
{
	struct spa_json it, it_array;
	char role_name[256];
	enum spa_bt_profile profiles = SPA_BT_PROFILE_NULL;

	spa_json_init(&it, str, strlen(str));

	if (spa_json_enter_array(&it, &it_array) <= 0)
		return -EINVAL;

	while (spa_json_get_string(&it_array, role_name, sizeof(role_name)) > 0) {
		if (spa_streq(role_name, "hsp_hs")) {
			profiles |= SPA_BT_PROFILE_HSP_HS;
		} else if (spa_streq(role_name, "hsp_ag")) {
			profiles |= SPA_BT_PROFILE_HSP_AG;
		} else if (spa_streq(role_name, "hfp_hf")) {
			profiles |= SPA_BT_PROFILE_HFP_HF;
		} else if (spa_streq(role_name, "hfp_ag")) {
			profiles |= SPA_BT_PROFILE_HFP_AG;
		} else if (spa_streq(role_name, "a2dp_sink")) {
			profiles |= SPA_BT_PROFILE_A2DP_SINK;
		} else if (spa_streq(role_name, "a2dp_source")) {
			profiles |= SPA_BT_PROFILE_A2DP_SOURCE;
		} else if (spa_streq(role_name, "bap_sink")) {
			profiles |= SPA_BT_PROFILE_BAP_SINK;
		} else if (spa_streq(role_name, "bap_source")) {
			profiles |= SPA_BT_PROFILE_BAP_SOURCE;
		} else if (spa_streq(role_name, "bap_bcast_source")) {
			profiles |= SPA_BT_PROFILE_BAP_BROADCAST_SOURCE;
		} else if (spa_streq(role_name, "bap_bcast_sink")) {
			profiles |= SPA_BT_PROFILE_BAP_BROADCAST_SINK;
		}
	}

	return profiles;
}

static int parse_roles(struct spa_bt_monitor *monitor, const struct spa_dict *info)
{
	const char *str;
	int res = 0;
	int profiles = SPA_BT_PROFILE_MEDIA_SINK | SPA_BT_PROFILE_MEDIA_SOURCE;

	/* HSP/HFP backends parse this property separately */
	if (info && (str = spa_dict_lookup(info, "bluez5.roles"))) {
		res = spa_bt_profiles_from_json_array(str);
		if (res < 0) {
			spa_log_warn(monitor->log, "malformed bluez5.roles setting ignored");
			goto done;
		}

		profiles &= res;
	}

	res = 0;

done:
	monitor->enabled_profiles = profiles;
	return res;
}

static int parse_codec_array(struct spa_bt_monitor *this, const struct spa_dict *info)
{
	const struct media_codec * const * const media_codecs = this->media_codecs;
	const char *str;
	struct spa_dict_item *codecs;
	struct spa_json it, it_array;
	char codec_name[256];
	size_t num_codecs;
	int i;

	/* Parse bluez5.codecs property to a dict of enabled codecs */

	num_codecs = 0;
	while (media_codecs[num_codecs])
		++num_codecs;

	codecs = calloc(num_codecs, sizeof(struct spa_dict_item));
	if (codecs == NULL)
		return -ENOMEM;

	if (info == NULL || (str = spa_dict_lookup(info, "bluez5.codecs")) == NULL)
		goto fallback;

	spa_json_init(&it, str, strlen(str));

	if (spa_json_enter_array(&it, &it_array) <= 0) {
		spa_log_error(this->log, "property bluez5.codecs '%s' is not an array", str);
		goto fallback;
	}

	this->enabled_codecs = SPA_DICT_INIT(codecs, 0);

	while (spa_json_get_string(&it_array, codec_name, sizeof(codec_name)) > 0) {
		int i;

		for (i = 0; media_codecs[i]; ++i) {
			const struct media_codec *codec = media_codecs[i];

			if (!spa_streq(codec->name, codec_name))
				continue;

			if (spa_dict_lookup_item(&this->enabled_codecs, codec->name) != NULL)
				continue;

			spa_log_debug(this->log, "enabling codec %s", codec->name);

			spa_assert(this->enabled_codecs.n_items < num_codecs);

			codecs[this->enabled_codecs.n_items].key = codec->name;
			codecs[this->enabled_codecs.n_items].value = "true";
			++this->enabled_codecs.n_items;

			break;
		}
	}

	spa_dict_qsort(&this->enabled_codecs);

	for (i = 0; media_codecs[i]; ++i) {
		const struct media_codec *codec = media_codecs[i];
		if (!is_media_codec_enabled(this, codec))
			spa_log_debug(this->log, "disabling codec %s", codec->name);
	}
	return 0;

fallback:
	for (i = 0; media_codecs[i]; ++i) {
		const struct media_codec *codec = media_codecs[i];
		spa_log_debug(this->log, "enabling codec %s", codec->name);
		codecs[i].key = codec->name;
		codecs[i].value = "true";
	}
	this->enabled_codecs = SPA_DICT_INIT(codecs, i);
	spa_dict_qsort(&this->enabled_codecs);
	return 0;
}

static void get_global_settings(struct spa_bt_monitor *this, const struct spa_dict *dict)
{
	uint32_t n_items = 0;
	uint32_t i;

	if (dict == NULL) {
		this->global_settings = SPA_DICT_INIT(this->global_setting_items, 0);
		return;
	}

	for (i = 0; i < dict->n_items && n_items < SPA_N_ELEMENTS(this->global_setting_items); i++) {
		const struct spa_dict_item *it = &dict->items[i];
		if (spa_strstartswith(it->key, "bluez5.") && it->value != NULL)
			this->global_setting_items[n_items++] =
				SPA_DICT_ITEM_INIT(strdup(it->key), strdup(it->value));
	}

	this->global_settings = SPA_DICT_INIT(this->global_setting_items, n_items);
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct spa_bt_monitor *this;
	int res;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct spa_bt_monitor *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	this->dbus = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DBus);
	this->main_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Loop);
	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	this->main_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_System);
	this->data_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataSystem);
	this->plugin_loader = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_PluginLoader);

	spa_log_topic_init(this->log, &log_topic);

	if (this->dbus == NULL) {
		spa_log_error(this->log, "a dbus is needed");
		return -EINVAL;
	}

	if (this->plugin_loader == NULL) {
		spa_log_error(this->log, "a plugin loader is needed");
		return -EINVAL;
	}

	this->media_codecs = NULL;
	this->quirks = NULL;
	this->conn = NULL;
	this->dbus_connection = NULL;

	this->media_codecs = load_media_codecs(this->plugin_loader, this->log);
	if (this->media_codecs == NULL) {
		spa_log_error(this->log, "failed to load required media codec plugins");
		res = -EIO;
		goto fail;
	}

	this->quirks = spa_bt_quirks_create(info, this->log);
	if (this->quirks == NULL) {
		spa_log_error(this->log, "failed to parse quirk table");
		res = -EINVAL;
		goto fail;
	}

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

	/* XXX: We should handle spa_dbus reconnecting, but we don't, so ref
	 * XXX: the handle so that we can keep it if spa_dbus unrefs it.
	 */
	dbus_connection_ref(this->conn);

	spa_hook_list_init(&this->hooks);

	this->device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, this);

	spa_list_init(&this->adapter_list);
	spa_list_init(&this->device_list);
	spa_list_init(&this->remote_endpoint_list);
	spa_list_init(&this->transport_list);

	if ((res = parse_codec_array(this, info)) < 0)
		goto fail;

	parse_roles(this, info);

	this->default_audio_info.rate = A2DP_CODEC_DEFAULT_RATE;
	this->default_audio_info.channels = A2DP_CODEC_DEFAULT_CHANNELS;

	this->backend_selection = BACKEND_NATIVE;

	get_global_settings(this, info);

	if (info) {
		const char *str;
		uint32_t tmp;

		if ((str = spa_dict_lookup(info, "api.bluez5.connection-info")) != NULL &&
		    spa_atob(str))
			this->connection_info_supported = true;

		if ((str = spa_dict_lookup(info, "bluez5.default.rate")) != NULL &&
		    (tmp =  atoi(str)) > 0)
			this->default_audio_info.rate = tmp;

		if ((str = spa_dict_lookup(info, "bluez5.default.channels")) != NULL &&
		    ((tmp =  atoi(str)) > 0))
			this->default_audio_info.channels = tmp;

		if ((str = spa_dict_lookup(info, "bluez5.hfphsp-backend")) != NULL) {
			if (spa_streq(str, "none"))
				this->backend_selection = BACKEND_NONE;
			else if (spa_streq(str, "any"))
				this->backend_selection = BACKEND_ANY;
			else if (spa_streq(str, "ofono"))
				this->backend_selection = BACKEND_OFONO;
			else if (spa_streq(str, "hsphfpd"))
				this->backend_selection = BACKEND_HSPHFPD;
			else if (spa_streq(str, "native"))
				this->backend_selection = BACKEND_NATIVE;
		}

		if ((str = spa_dict_lookup(info, "bluez5.dummy-avrcp-player")) != NULL)
			this->dummy_avrcp_player = spa_atob(str);
		else
			this->dummy_avrcp_player = false;
	}

	register_media_application(this);

	/* Create backends. They're started after we get a reply from Bluez. */
	this->backends[BACKEND_NATIVE] = backend_native_new(this, this->conn, info, this->quirks, support, n_support);
	this->backends[BACKEND_OFONO] = backend_ofono_new(this, this->conn, info, this->quirks, support, n_support);
	this->backends[BACKEND_HSPHFPD] = backend_hsphfpd_new(this, this->conn, info, this->quirks, support, n_support);

	return 0;

fail:
	if (this->media_codecs)
		free_media_codecs(this->media_codecs);
	if (this->quirks)
		spa_bt_quirks_destroy(this->quirks);
	if (this->conn)
		dbus_connection_unref(this->conn);
	if (this->dbus_connection)
		spa_dbus_connection_destroy(this->dbus_connection);
	this->media_codecs = NULL;
	this->quirks = NULL;
	this->conn = NULL;
	this->dbus_connection = NULL;
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

const struct spa_handle_factory spa_bluez5_dbus_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BLUEZ5_ENUM_DBUS,
	NULL,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};

// Report battery percentage to BlueZ using experimental (BlueZ 5.56) Battery Provider API. No-op if no changes occurred.
int spa_bt_device_report_battery_level(struct spa_bt_device *device, uint8_t percentage)
{
	if (percentage == SPA_BT_NO_BATTERY) {
		battery_remove(device);
		return 0;
	}

	// BlueZ likely is running without battery provider support, don't try to report battery
	if (device->adapter->battery_provider_unavailable) return 0;

	// If everything is initialized and battery level has not changed we don't need to send anything to BlueZ
	if (device->adapter->has_battery_provider && device->has_battery && device->battery == percentage) return 1;

	device->battery = percentage;

	if (!device->adapter->has_battery_provider) {
		// No provider: register it, create battery when registered
		register_battery_provider(device);
	} else if (!device->has_battery) {
		// Have provider but no battery: create battery with correct percentage
		battery_create(device);
	} else {
		// Just update existing battery percentage
		battery_update(device);
	}

	return 1;
}
