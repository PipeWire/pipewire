/* Spa Bluez5 Monitor */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#ifndef SPA_BLUEZ5_DEFS_H
#define SPA_BLUEZ5_DEFS_H

#include "config.h"

#include <math.h>

#include <spa/support/dbus.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/monitor/device.h>
#include <spa/utils/hook.h>
#include <spa/param/audio/raw.h>

#include <dbus/dbus.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BLUEZ_SERVICE "org.bluez"
#define BLUEZ_PROFILE_MANAGER_INTERFACE BLUEZ_SERVICE ".ProfileManager1"
#define BLUEZ_PROFILE_INTERFACE BLUEZ_SERVICE ".Profile1"
#define BLUEZ_ADAPTER_INTERFACE BLUEZ_SERVICE ".Adapter1"
#define BLUEZ_DEVICE_INTERFACE BLUEZ_SERVICE ".Device1"
#define BLUEZ_DEVICE_SET_INTERFACE BLUEZ_SERVICE ".DeviceSet1"
#define BLUEZ_MEDIA_INTERFACE BLUEZ_SERVICE ".Media1"
#define BLUEZ_MEDIA_ENDPOINT_INTERFACE BLUEZ_SERVICE ".MediaEndpoint1"
#define BLUEZ_MEDIA_TRANSPORT_INTERFACE BLUEZ_SERVICE ".MediaTransport1"
#define BLUEZ_INTERFACE_BATTERY_PROVIDER BLUEZ_SERVICE ".BatteryProvider1"
#define BLUEZ_INTERFACE_BATTERY_PROVIDER_MANAGER BLUEZ_SERVICE ".BatteryProviderManager1"

#define DBUS_INTERFACE_OBJECT_MANAGER "org.freedesktop.DBus.ObjectManager"
#define DBUS_SIGNAL_INTERFACES_ADDED "InterfacesAdded"
#define DBUS_SIGNAL_INTERFACES_REMOVED "InterfacesRemoved"
#define DBUS_SIGNAL_PROPERTIES_CHANGED "PropertiesChanged"

#define PIPEWIRE_BATTERY_PROVIDER "/org/freedesktop/pipewire/battery"

#define OBJECT_MANAGER_INTROSPECT_XML                                          \
	DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                                  \
	"<node>\n"                                                                 \
	" <interface name=\"org.freedesktop.DBus.ObjectManager\">\n"               \
	"  <method name=\"GetManagedObjects\">\n"                                  \
	"   <arg name=\"objects\" direction=\"out\" type=\"a{oa{sa{sv}}}\"/>\n"    \
	"  </method>\n"                                                            \
	"  <signal name=\"InterfacesAdded\">\n"                                    \
	"   <arg name=\"object\" type=\"o\"/>\n"                                   \
	"   <arg name=\"interfaces\" type=\"a{sa{sv}}\"/>\n"                       \
	"  </signal>\n"                                                            \
	"  <signal name=\"InterfacesRemoved\">\n"                                  \
	"   <arg name=\"object\" type=\"o\"/>\n"                                   \
	"   <arg name=\"interfaces\" type=\"as\"/>\n"                              \
	"  </signal>\n"                                                            \
	" </interface>\n"                                                          \
	" <interface name=\"org.freedesktop.DBus.Introspectable\">\n"              \
	"  <method name=\"Introspect\">\n"                                         \
	"   <arg name=\"data\" direction=\"out\" type=\"s\"/>\n"                   \
	"  </method>\n"                                                            \
	" </interface>\n"                                                          \
	" <node name=\"A2DPSink\"/>\n"                                             \
	" <node name=\"A2DPSource\"/>\n"                                           \
	"</node>\n"

#define ENDPOINT_INTROSPECT_XML                                             \
	DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                           \
	"<node>"                                                            \
	" <interface name=\"" BLUEZ_MEDIA_ENDPOINT_INTERFACE "\">"          \
	"  <method name=\"SetConfiguration\">"                              \
	"   <arg name=\"transport\" direction=\"in\" type=\"o\"/>"          \
	"   <arg name=\"properties\" direction=\"in\" type=\"ay\"/>"        \
	"  </method>"                                                       \
	"  <method name=\"SelectConfiguration\">"                           \
	"   <arg name=\"capabilities\" direction=\"in\" type=\"ay\"/>"      \
	"   <arg name=\"configuration\" direction=\"out\" type=\"ay\"/>"    \
	"  </method>"                                                       \
	"  <method name=\"ClearConfiguration\">"                            \
	"   <arg name=\"transport\" direction=\"in\" type=\"o\"/>"          \
	"  </method>"                                                       \
	"  <method name=\"Release\">"                                       \
	"  </method>"                                                       \
	" </interface>"                                                     \
	" <interface name=\"org.freedesktop.DBus.Introspectable\">"         \
	"  <method name=\"Introspect\">"                                    \
	"   <arg name=\"data\" type=\"s\" direction=\"out\"/>"              \
	"  </method>"                                                       \
	" </interface>"                                                     \
	"</node>"

#define PROFILE_INTROSPECT_XML						    \
	DBUS_INTROSPECT_1_0_XML_DOCTYPE_DECL_NODE                           \
	"<node>"                                                            \
	" <interface name=\"" BLUEZ_PROFILE_INTERFACE "\">"                 \
	"  <method name=\"Release\">"                                       \
	"  </method>"                                                       \
	"  <method name=\"RequestDisconnection\">"                          \
	"   <arg name=\"device\" direction=\"in\" type=\"o\"/>"             \
	"  </method>"                                                       \
	"  <method name=\"NewConnection\">"                                 \
	"   <arg name=\"device\" direction=\"in\" type=\"o\"/>"             \
	"   <arg name=\"fd\" direction=\"in\" type=\"h\"/>"                 \
	"   <arg name=\"opts\" direction=\"in\" type=\"a{sv}\"/>"           \
	"  </method>"                                                       \
	" </interface>"                                                     \
	" <interface name=\"org.freedesktop.DBus.Introspectable\">"         \
	"  <method name=\"Introspect\">"                                    \
	"   <arg name=\"data\" type=\"s\" direction=\"out\"/>"              \
	"  </method>"                                                       \
	" </interface>"                                                     \
	"</node>"

#define BLUEZ_ERROR_NOT_SUPPORTED "org.bluez.Error.NotSupported"

#define SPA_BT_UUID_A2DP_SOURCE "0000110a-0000-1000-8000-00805f9b34fb"
#define SPA_BT_UUID_A2DP_SINK   "0000110b-0000-1000-8000-00805f9b34fb"
#define SPA_BT_UUID_HSP_HS      "00001108-0000-1000-8000-00805f9b34fb"
#define SPA_BT_UUID_HSP_HS_ALT  "00001131-0000-1000-8000-00805f9b34fb"
#define SPA_BT_UUID_HSP_AG      "00001112-0000-1000-8000-00805f9b34fb"
#define SPA_BT_UUID_HFP_HF      "0000111e-0000-1000-8000-00805f9b34fb"
#define SPA_BT_UUID_HFP_AG      "0000111f-0000-1000-8000-00805f9b34fb"
#define SPA_BT_UUID_PACS        "00001850-0000-1000-8000-00805f9b34fb"
#define SPA_BT_UUID_BAP_SINK    "00002bc9-0000-1000-8000-00805f9b34fb"
#define SPA_BT_UUID_BAP_SOURCE  "00002bcb-0000-1000-8000-00805f9b34fb"
#define SPA_BT_UUID_BAP_BROADCAST_SOURCE  "00001852-0000-1000-8000-00805f9b34fb"
#define SPA_BT_UUID_BAP_BROADCAST_SINK    "00001851-0000-1000-8000-00805f9b34fb"
#define SPA_BT_UUID_ASHA_SINK   "0000FDF0-0000-1000-8000-00805f9b34fb"

#define PROFILE_HSP_AG	"/Profile/HSPAG"
#define PROFILE_HSP_HS	"/Profile/HSPHS"
#define PROFILE_HFP_AG	"/Profile/HFPAG"
#define PROFILE_HFP_HF	"/Profile/HFPHF"

#define HSP_HS_DEFAULT_CHANNEL  3

#define SOURCE_ID_BLUETOOTH	0x1	/* Bluetooth SIG */
#define SOURCE_ID_USB		0x2	/* USB Implementer's Forum */

#define BUS_TYPE_USB		1
#define BUS_TYPE_OTHER		255

#define A2DP_OBJECT_MANAGER_PATH "/MediaEndpoint"
#define A2DP_SINK_ENDPOINT	A2DP_OBJECT_MANAGER_PATH "/A2DPSink"
#define A2DP_SOURCE_ENDPOINT	A2DP_OBJECT_MANAGER_PATH "/A2DPSource"

#define BAP_OBJECT_MANAGER_PATH "/MediaEndpointLE"
#define BAP_SINK_ENDPOINT	BAP_OBJECT_MANAGER_PATH "/BAPSink"
#define BAP_SOURCE_ENDPOINT	BAP_OBJECT_MANAGER_PATH "/BAPSource"
#define BAP_BROADCAST_SOURCE_ENDPOINT	BAP_OBJECT_MANAGER_PATH "/BAPBroadcastSource"
#define BAP_BROADCAST_SINK_ENDPOINT		BAP_OBJECT_MANAGER_PATH "/BAPBroadcastSink"

#define SPA_BT_UNKNOWN_DELAY			0

#define SPA_BT_NO_BATTERY			((uint8_t)255)

#define MAX_CHANNELS	(SPA_AUDIO_MAX_CHANNELS)

enum spa_bt_media_direction {
	SPA_BT_MEDIA_SOURCE,
	SPA_BT_MEDIA_SINK,
	SPA_BT_MEDIA_SOURCE_BROADCAST,
	SPA_BT_MEDIA_SINK_BROADCAST,
	SPA_BT_MEDIA_DIRECTION_LAST,
};

enum spa_bt_profile {
	SPA_BT_PROFILE_NULL =		0,
	SPA_BT_PROFILE_BAP_SINK =	(1 << 0),
	SPA_BT_PROFILE_BAP_SOURCE =	(1 << 1),
	SPA_BT_PROFILE_A2DP_SINK =	(1 << 2),
	SPA_BT_PROFILE_A2DP_SOURCE =	(1 << 3),
	SPA_BT_PROFILE_ASHA_SINK =      (1 << 4),
	SPA_BT_PROFILE_HSP_HS =		(1 << 5),
	SPA_BT_PROFILE_HSP_AG =		(1 << 6),
	SPA_BT_PROFILE_HFP_HF =		(1 << 7),
	SPA_BT_PROFILE_HFP_AG =		(1 << 8),
	SPA_BT_PROFILE_BAP_BROADCAST_SOURCE =	(1 << 9),
	SPA_BT_PROFILE_BAP_BROADCAST_SINK   =	(1 << 10),

	SPA_BT_PROFILE_A2DP_DUPLEX =	(SPA_BT_PROFILE_A2DP_SINK | SPA_BT_PROFILE_A2DP_SOURCE),
	SPA_BT_PROFILE_BAP_DUPLEX =     (SPA_BT_PROFILE_BAP_SINK | SPA_BT_PROFILE_BAP_SOURCE),
	SPA_BT_PROFILE_HEADSET_HEAD_UNIT = (SPA_BT_PROFILE_HSP_HS | SPA_BT_PROFILE_HFP_HF),
	SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY = (SPA_BT_PROFILE_HSP_AG | SPA_BT_PROFILE_HFP_AG),
	SPA_BT_PROFILE_HEADSET_AUDIO =  (SPA_BT_PROFILE_HEADSET_HEAD_UNIT | SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY),
	SPA_BT_PROFILE_BAP_AUDIO =  (SPA_BT_PROFILE_BAP_SINK | SPA_BT_PROFILE_BAP_BROADCAST_SINK |
			SPA_BT_PROFILE_BAP_SOURCE | SPA_BT_PROFILE_BAP_BROADCAST_SOURCE),

	SPA_BT_PROFILE_MEDIA_SINK =		(SPA_BT_PROFILE_A2DP_SINK | SPA_BT_PROFILE_BAP_SINK |
										SPA_BT_PROFILE_BAP_BROADCAST_SINK),
	SPA_BT_PROFILE_MEDIA_SOURCE =	(SPA_BT_PROFILE_A2DP_SOURCE | SPA_BT_PROFILE_BAP_SOURCE |
										SPA_BT_PROFILE_BAP_BROADCAST_SOURCE),
};

static inline enum spa_bt_profile spa_bt_profile_from_uuid(const char *uuid)
{
	if (!uuid)
		return 0;
	else if (strcasecmp(uuid, SPA_BT_UUID_A2DP_SOURCE) == 0)
		return SPA_BT_PROFILE_A2DP_SOURCE;
	else if (strcasecmp(uuid, SPA_BT_UUID_A2DP_SINK) == 0)
		return SPA_BT_PROFILE_A2DP_SINK;
	else if (strcasecmp(uuid, SPA_BT_UUID_HSP_HS) == 0)
		return SPA_BT_PROFILE_HSP_HS;
	else if (strcasecmp(uuid, SPA_BT_UUID_HSP_HS_ALT) == 0)
		return SPA_BT_PROFILE_HSP_HS;
	else if (strcasecmp(uuid, SPA_BT_UUID_HSP_AG) == 0)
		return SPA_BT_PROFILE_HSP_AG;
	else if (strcasecmp(uuid, SPA_BT_UUID_HFP_HF) == 0)
		return SPA_BT_PROFILE_HFP_HF;
	else if (strcasecmp(uuid, SPA_BT_UUID_HFP_AG) == 0)
		return SPA_BT_PROFILE_HFP_AG;
	else if (strcasecmp(uuid, SPA_BT_UUID_BAP_SINK) == 0)
		return SPA_BT_PROFILE_BAP_SINK;
	else if (strcasecmp(uuid, SPA_BT_UUID_BAP_SOURCE) == 0)
		return SPA_BT_PROFILE_BAP_SOURCE;
	else if (strcasecmp(uuid, SPA_BT_UUID_BAP_BROADCAST_SOURCE) == 0)
		return SPA_BT_PROFILE_BAP_BROADCAST_SOURCE;
	else if (strcasecmp(uuid, SPA_BT_UUID_BAP_BROADCAST_SINK) == 0)
		return SPA_BT_PROFILE_BAP_BROADCAST_SINK;
	else if (strcasecmp(uuid, SPA_BT_UUID_ASHA_SINK) == 0)
		return SPA_BT_PROFILE_ASHA_SINK;
	else
		return 0;
}
int spa_bt_profiles_from_json_array(const char *str);

int spa_bt_format_vendor_product_id(uint16_t source_id, uint16_t vendor_id,
		uint16_t product_id, char *vendor_str, int vendor_str_size,
		char *product_str, int product_str_size);

enum spa_bt_hfp_ag_feature {
	SPA_BT_HFP_AG_FEATURE_NONE =			(0),
	SPA_BT_HFP_AG_FEATURE_3WAY =			(1 << 0),
	SPA_BT_HFP_AG_FEATURE_ECNR =			(1 << 1),
	SPA_BT_HFP_AG_FEATURE_VOICE_RECOG =		(1 << 2),
	SPA_BT_HFP_AG_FEATURE_IN_BAND_RING_TONE =	(1 << 3),
	SPA_BT_HFP_AG_FEATURE_ATTACH_VOICE_TAG =	(1 << 4),
	SPA_BT_HFP_AG_FEATURE_REJECT_CALL =		(1 << 5),
	SPA_BT_HFP_AG_FEATURE_ENHANCED_CALL_STATUS =	(1 << 6),
	SPA_BT_HFP_AG_FEATURE_ENHANCED_CALL_CONTROL =	(1 << 7),
	SPA_BT_HFP_AG_FEATURE_EXTENDED_RES_CODE =	(1 << 8),
	SPA_BT_HFP_AG_FEATURE_CODEC_NEGOTIATION =	(1 << 9),
	SPA_BT_HFP_AG_FEATURE_HF_INDICATORS =		(1 << 10),
	SPA_BT_HFP_AG_FEATURE_ESCO_S4 =			(1 << 11),
};

enum spa_bt_hfp_sdp_ag_features {
	SPA_BT_HFP_SDP_AG_FEATURE_NONE =			(0),
	SPA_BT_HFP_SDP_AG_FEATURE_3WAY =			(1 << 0),
	SPA_BT_HFP_SDP_AG_FEATURE_ECNR =			(1 << 1),
	SPA_BT_HFP_SDP_AG_FEATURE_VOICE_RECOG =			(1 << 2),
	SPA_BT_HFP_SDP_AG_FEATURE_IN_BAND_RING_TONE =		(1 << 3),
	SPA_BT_HFP_SDP_AG_FEATURE_ATTACH_VOICE_TAG =		(1 << 4),
	SPA_BT_HFP_SDP_AG_FEATURE_WIDEBAND_SPEECH =		(1 << 5),
	SPA_BT_HFP_SDP_AG_FEATURE_ENH_VOICE_RECOG_STATUS =	(1 << 6),
	SPA_BT_HFP_SDP_AG_FEATURE_VOICE_RECOG_TEXT =		(1 << 7),
	SPA_BT_HFP_SDP_AG_FEATURE_SUPER_WIDEBAND_SPEECH =	(1 << 8),
};

enum spa_bt_hfp_hf_feature {
	SPA_BT_HFP_HF_FEATURE_NONE =			(0),
	SPA_BT_HFP_HF_FEATURE_ECNR =			(1 << 0),
	SPA_BT_HFP_HF_FEATURE_3WAY =			(1 << 1),
	SPA_BT_HFP_HF_FEATURE_CLIP =			(1 << 2),
	SPA_BT_HFP_HF_FEATURE_VOICE_RECOGNITION =	(1 << 3),
	SPA_BT_HFP_HF_FEATURE_REMOTE_VOLUME_CONTROL =	(1 << 4),
	SPA_BT_HFP_HF_FEATURE_ENHANCED_CALL_STATUS =	(1 << 5),
	SPA_BT_HFP_HF_FEATURE_ENHANCED_CALL_CONTROL =	(1 << 6),
	SPA_BT_HFP_HF_FEATURE_CODEC_NEGOTIATION =	(1 << 7),
	SPA_BT_HFP_HF_FEATURE_HF_INDICATORS =		(1 << 8),
	SPA_BT_HFP_HF_FEATURE_ESCO_S4 =			(1 << 9),
};

/* https://btprodspecificationrefs.blob.core.windows.net/assigned-numbers/Assigned%20Number%20Types/Hands-Free%20Profile.pdf */
enum spa_bt_hfp_hf_indicator {
	SPA_BT_HFP_HF_INDICATOR_ENHANCED_SAFETY =        1,
	SPA_BT_HFP_HF_INDICATOR_BATTERY_LEVEL =          2,
};

/* HFP Command AT+IPHONEACCEV
 *  https://developer.apple.com/accessories/Accessory-Design-Guidelines.pdf */
enum spa_bt_hfp_hf_iphoneaccev_keys {
	SPA_BT_HFP_HF_IPHONEACCEV_KEY_BATTERY_LEVEL =    1,
	SPA_BT_HFP_HF_IPHONEACCEV_KEY_DOCK_STATE =       2,
};

/* HFP Command AT+XAPL
 *  https://developer.apple.com/accessories/Accessory-Design-Guidelines.pdf
 * Bits 0, 5 and above are reserved. */
enum spa_bt_hfp_hf_xapl_features {
	SPA_BT_HFP_HF_XAPL_FEATURE_BATTERY_REPORTING =         (1 << 1),
	SPA_BT_HFP_HF_XAPL_FEATURE_DOCKED_OR_POWERED =         (1 << 2),
	SPA_BT_HFP_HF_XAPL_FEATURE_SIRI_STATUS_REPORTING =     (1 << 3),
	SPA_BT_HFP_HF_XAPL_FEATURE_NOISE_REDUCTION_REPORTING = (1 << 4),
};

enum spa_bt_hfp_sdp_hf_features {
	SPA_BT_HFP_SDP_HF_FEATURE_NONE =			(0),
	SPA_BT_HFP_SDP_HF_FEATURE_ECNR =			(1 << 0),
	SPA_BT_HFP_SDP_HF_FEATURE_3WAY =			(1 << 1),
	SPA_BT_HFP_SDP_HF_FEATURE_CLIP =			(1 << 2),
	SPA_BT_HFP_SDP_HF_FEATURE_VOICE_RECOGNITION =		(1 << 3),
	SPA_BT_HFP_SDP_HF_FEATURE_REMOTE_VOLUME_CONTROL =	(1 << 4),
	SPA_BT_HFP_SDP_HF_FEATURE_WIDEBAND_SPEECH =		(1 << 5),
	SPA_BT_HFP_SDP_HF_FEATURE_ENH_VOICE_RECOG_STATUS =	(1 << 6),
	SPA_BT_HFP_SDP_HF_FEATURE_VOICE_RECOG_TEXT =		(1 << 7),
	SPA_BT_HFP_SDP_HF_FEATURE_SUPER_WIDEBAND_SPEECH =	(1 << 8),
};

static inline const char *spa_bt_profile_name (enum spa_bt_profile profile) {
      switch (profile) {
      case SPA_BT_PROFILE_ASHA_SINK:
        return "asha-sink";
      case SPA_BT_PROFILE_A2DP_SOURCE:
        return "a2dp-source";
      case SPA_BT_PROFILE_A2DP_SINK:
        return "a2dp-sink";
      case SPA_BT_PROFILE_A2DP_DUPLEX:
        return "a2dp-duplex";
      case SPA_BT_PROFILE_HSP_HS:
      case SPA_BT_PROFILE_HFP_HF:
      case SPA_BT_PROFILE_HEADSET_HEAD_UNIT:
	return "headset-head-unit";
      case SPA_BT_PROFILE_HSP_AG:
      case SPA_BT_PROFILE_HFP_AG:
      case SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY:
	return "headset-audio-gateway";
      case SPA_BT_PROFILE_HEADSET_AUDIO:
	return "headset-audio";
      case SPA_BT_PROFILE_BAP_SOURCE:
      case SPA_BT_PROFILE_BAP_BROADCAST_SOURCE:
        return "bap-source";
      case SPA_BT_PROFILE_BAP_SINK:
      case SPA_BT_PROFILE_BAP_BROADCAST_SINK:
        return "bap-sink";
      case SPA_BT_PROFILE_BAP_DUPLEX:
        return "bap-duplex";
      default:
        break;
      }
      return "unknown";
}

struct spa_bt_monitor;
struct spa_bt_backend;
struct spa_bt_player;

struct spa_bt_adapter {
	struct spa_list link;
	struct spa_bt_monitor *monitor;
	struct spa_bt_player *dummy_player;
	char *path;
	char *alias;
	char *address;
	char *name;
	int bus_type;
	uint16_t source_id;
	uint16_t vendor_id;
	uint16_t product_id;
	uint16_t version_id;
	uint32_t bluetooth_class;
	uint32_t profiles;
	int powered;
	unsigned int has_msbc:1;
	unsigned int msbc_probed:1;
	unsigned int legacy_endpoints_registered:1;
	unsigned int a2dp_application_registered:1;
	unsigned int bap_application_registered:1;
	unsigned int player_registered:1;
	unsigned int has_battery_provider:1;
	unsigned int battery_provider_unavailable:1;
	unsigned int le_audio_supported:1;
	unsigned int has_adapter1_interface:1;
	unsigned int has_media1_interface:1;
	unsigned int le_audio_bcast_supported:1;
	unsigned int tx_timestamping_supported:1;
};

enum spa_bt_form_factor {
	SPA_BT_FORM_FACTOR_UNKNOWN,
	SPA_BT_FORM_FACTOR_HEADSET,
	SPA_BT_FORM_FACTOR_HANDSFREE,
	SPA_BT_FORM_FACTOR_MICROPHONE,
	SPA_BT_FORM_FACTOR_SPEAKER,
	SPA_BT_FORM_FACTOR_HEADPHONE,
	SPA_BT_FORM_FACTOR_PORTABLE,
	SPA_BT_FORM_FACTOR_CAR,
	SPA_BT_FORM_FACTOR_HIFI,
	SPA_BT_FORM_FACTOR_PHONE,
};

static inline const char *spa_bt_form_factor_name(enum spa_bt_form_factor ff)
{
	switch (ff) {
	case SPA_BT_FORM_FACTOR_HEADSET:
		return "headset";
	case SPA_BT_FORM_FACTOR_HANDSFREE:
		return "hands-free";
	case SPA_BT_FORM_FACTOR_MICROPHONE:
		return "microphone";
	case SPA_BT_FORM_FACTOR_SPEAKER:
		return "speaker";
	case SPA_BT_FORM_FACTOR_HEADPHONE:
		return "headphone";
	case SPA_BT_FORM_FACTOR_PORTABLE:
		return "portable";
	case SPA_BT_FORM_FACTOR_CAR:
		return "car";
	case SPA_BT_FORM_FACTOR_HIFI:
		return "hifi";
	case SPA_BT_FORM_FACTOR_PHONE:
		return "phone";
	case SPA_BT_FORM_FACTOR_UNKNOWN:
	default:
		return "unknown";
	}
}

static inline const char *spa_bt_form_factor_icon_name(enum spa_bt_form_factor ff)
{
	switch (ff) {
	case SPA_BT_FORM_FACTOR_HEADSET:
		return "audio-headset-bluetooth";
	case SPA_BT_FORM_FACTOR_HANDSFREE:
		return "audio-handsfree-bluetooth";
	case SPA_BT_FORM_FACTOR_MICROPHONE:
		return "audio-input-microphone-bluetooth";
	case SPA_BT_FORM_FACTOR_SPEAKER:
		return "audio-speakers-bluetooth";
	case SPA_BT_FORM_FACTOR_HEADPHONE:
		return "audio-headphones-bluetooth";
	case SPA_BT_FORM_FACTOR_PORTABLE:
		return "multimedia-player-bluetooth";
	case SPA_BT_FORM_FACTOR_PHONE:
		return "phone-bluetooth";
	case SPA_BT_FORM_FACTOR_CAR:
	case SPA_BT_FORM_FACTOR_HIFI:
	case SPA_BT_FORM_FACTOR_UNKNOWN:
	default:
		return "audio-card-bluetooth";
	}
}

static inline enum spa_bt_form_factor spa_bt_form_factor_from_class(uint32_t bluetooth_class)
{
	uint32_t major, minor;
	/* See Bluetooth Assigned Numbers:
	 * https://www.bluetooth.org/Technical/AssignedNumbers/baseband.htm */
	major = (bluetooth_class >> 8) & 0x1F;
	minor = (bluetooth_class >> 2) & 0x3F;

	switch (major) {
	case 2:
		return SPA_BT_FORM_FACTOR_PHONE;
	case 4:
		switch (minor) {
		case 1:
			return SPA_BT_FORM_FACTOR_HEADSET;
		case 2:
			return SPA_BT_FORM_FACTOR_HANDSFREE;
		case 4:
			return SPA_BT_FORM_FACTOR_MICROPHONE;
		case 5:
			return SPA_BT_FORM_FACTOR_SPEAKER;
		case 6:
			return SPA_BT_FORM_FACTOR_HEADPHONE;
		case 7:
			return SPA_BT_FORM_FACTOR_PORTABLE;
		case 8:
			return SPA_BT_FORM_FACTOR_CAR;
		case 10:
			return SPA_BT_FORM_FACTOR_HIFI;
		}
	}
	return SPA_BT_FORM_FACTOR_UNKNOWN;
}

struct spa_bt_transport;

struct spa_bt_device_events {
#define SPA_VERSION_BT_DEVICE_EVENTS	0
	uint32_t version;

	/** Device connection status */
	void (*connected) (void *data, bool connected);

	/** Codec switching completed */
	void (*codec_switched) (void *data, int status);

	/** Codec switching initiated or completed by another device */
	void (*codec_switch_other) (void *data, bool switching);

	/** Profile configuration changed */
	void (*profiles_changed) (void *data, uint32_t connected_change);

	/** Device set configuration changed */
	void (*device_set_changed) (void *data);

	/** Switch profile between OFF and HSP_HFP */
	void (*switch_profile) (void *data);

	/** Device freed */
	void (*destroy) (void *data);
};

struct media_codec;

struct spa_bt_set_membership {
	struct spa_list link;
	struct spa_list others;
	struct spa_bt_device *device;
	char *path;
	uint8_t rank;
	bool leader;
};

#define spa_bt_for_each_set_member(s, set) \
	for ((s) = (set); (s); (s) = spa_list_next((s), others), (s) = (s) != (set) ? (s) : NULL)

struct spa_bt_device {
	struct spa_list link;
	struct spa_bt_monitor *monitor;
	struct spa_bt_adapter *adapter;
	uint32_t id;
	char *path;
	char *alias;
	char *address;
	char *adapter_path;
	char *battery_path;
	char *name;
	char *icon;
	uint16_t source_id;
	uint16_t vendor_id;
	uint16_t product_id;
	uint16_t version_id;
	uint32_t bluetooth_class;
	uint16_t appearance;
	uint16_t RSSI;
	int paired;
	int trusted;
	int connected;
	int blocked;
	uint32_t profiles;
	uint32_t connected_profiles;
	uint32_t reconnect_profiles;
	int reconnect_state;
	struct spa_source timer;
	struct spa_list remote_endpoint_list;
	struct spa_list transport_list;
	struct spa_list codec_switch_list;
	struct spa_list set_membership_list;
	uint8_t battery;
	int has_battery;

	uint32_t hw_volume_profiles;
	/* Even though A2DP volume is exposed on transport interface, the
	 * volume activation info would not be variate between transports
	 * under same device. So it's safe to cache activation info here. */
	bool a2dp_volume_active[2];

	uint64_t last_bluez_action_time;

	struct spa_hook_list listener_list;
	bool added;

	const struct spa_dict *settings;

	DBusPendingCall *battery_pending_call;

	const struct media_codec *preferred_codec;
	uint32_t preferred_profiles;
};

struct spa_bt_device *spa_bt_device_find(struct spa_bt_monitor *monitor, const char *path);
struct spa_bt_device *spa_bt_device_find_by_address(struct spa_bt_monitor *monitor, const char *remote_address, const char *local_address);
int spa_bt_device_add_profile(struct spa_bt_device *device, enum spa_bt_profile profile);
int spa_bt_device_connect_profile(struct spa_bt_device *device, enum spa_bt_profile profile);
int spa_bt_device_check_profiles(struct spa_bt_device *device, bool force);
int spa_bt_device_ensure_media_codec(struct spa_bt_device *device, const struct media_codec * const *codecs, uint32_t profiles);
int spa_bt_device_ensure_hfp_codec(struct spa_bt_device *device, const struct media_codec *codec);
bool spa_bt_device_supports_media_codec(struct spa_bt_device *device, const struct media_codec *codec, enum spa_bt_profile profile);
const struct media_codec **spa_bt_device_get_supported_media_codecs(struct spa_bt_device *device, size_t *count);
int spa_bt_device_release_transports(struct spa_bt_device *device);
int spa_bt_device_report_battery_level(struct spa_bt_device *device, uint8_t percentage);
void spa_bt_device_update_last_bluez_action_time(struct spa_bt_device *device);
const struct media_codec * const * spa_bt_get_media_codecs(struct spa_bt_monitor *monitor);
const struct media_codec *spa_bt_get_hfp_codec(struct spa_bt_monitor *monitor, unsigned int hfp_codec_id);

#define spa_bt_device_emit(d,m,v,...)			spa_hook_list_call(&(d)->listener_list, \
								struct spa_bt_device_events,	\
								m, v, ##__VA_ARGS__)
#define spa_bt_device_emit_connected(d,...)	        spa_bt_device_emit(d, connected, 0, __VA_ARGS__)
#define spa_bt_device_emit_codec_switched(d,...)	spa_bt_device_emit(d, codec_switched, 0, __VA_ARGS__)
#define spa_bt_device_emit_codec_switch_other(d,...)	spa_bt_device_emit(d, codec_switch_other, 0, __VA_ARGS__)
#define spa_bt_device_emit_profiles_changed(d,...)	spa_bt_device_emit(d, profiles_changed, 0, __VA_ARGS__)
#define spa_bt_device_emit_device_set_changed(d)	spa_bt_device_emit(d, device_set_changed, 0)
#define spa_bt_device_emit_switch_profile(d)		spa_bt_device_emit(d, switch_profile, 0)
#define spa_bt_device_emit_destroy(d)			spa_bt_device_emit(d, destroy, 0)
#define spa_bt_device_add_listener(d,listener,events,data)           \
	spa_hook_list_append(&(d)->listener_list, listener, events, data)

struct spa_bt_iso_io;

struct spa_bt_sco_io;

struct spa_bt_sco_io *spa_bt_sco_io_create(struct spa_bt_transport *transport, struct spa_loop *data_loop,
		struct spa_system *data_system, struct spa_log *log);
void spa_bt_sco_io_destroy(struct spa_bt_sco_io *io);
void spa_bt_sco_io_set_source_cb(struct spa_bt_sco_io *io, int (*source_cb)(void *userdata, uint8_t *data, int size, uint64_t rx_time), void *userdata);
int spa_bt_sco_io_write(struct spa_bt_sco_io *io, const uint8_t *buf, size_t size);
void spa_bt_sco_io_write_start(struct spa_bt_sco_io *io);

#define SPA_BT_VOLUME_ID_RX	0
#define SPA_BT_VOLUME_ID_TX	1
#define SPA_BT_VOLUME_ID_TERM	2

#define SPA_BT_VOLUME_INVALID	-1
#define SPA_BT_VOLUME_HS_MAX	15
#define SPA_BT_VOLUME_A2DP_MAX	127
#define SPA_BT_VOLUME_BAP_MAX	255

enum spa_bt_transport_state {
        SPA_BT_TRANSPORT_STATE_ERROR = -1,
        SPA_BT_TRANSPORT_STATE_IDLE = 0,
        SPA_BT_TRANSPORT_STATE_PENDING = 1,
        SPA_BT_TRANSPORT_STATE_ACTIVE = 2,
};

struct spa_bt_transport_events {
#define SPA_VERSION_BT_TRANSPORT_EVENTS	0
	uint32_t version;

	void (*destroy) (void *data);
	void (*delay_changed) (void *data);
	void (*state_changed) (void *data, enum spa_bt_transport_state old,
			enum spa_bt_transport_state state);
	void (*volume_changed) (void *data);
};

struct spa_bt_transport_implementation {
#define SPA_VERSION_BT_TRANSPORT_IMPLEMENTATION	0
	uint32_t version;

	int (*acquire) (void *data, bool optional);
	int (*release) (void *data);
	int (*set_volume) (void *data, int id, float volume);
	int (*set_delay) (void *data, int64_t delay_nsec);
	int (*destroy) (void *data);
};

struct spa_bt_transport_volume {
	bool active;
	float volume;
	int hw_volume_max;

	/* XXX: items below should be put to user_data */
	int hw_volume;
	int new_hw_volume;
};

struct spa_bt_transport {
	struct spa_list link;
	struct spa_bt_monitor *monitor;
	struct spa_bt_backend *backend;
	char *path;
	struct spa_bt_device *device;
	struct spa_list device_link;
	enum spa_bt_profile profile;
	enum spa_bt_transport_state state;
	const struct media_codec *media_codec;
	void *configuration;
	int configuration_len;
	char *endpoint_path;
	char *remote_endpoint_path;
	bool bap_initiator;
	struct spa_list bap_transport_linked;

	uint32_t n_channels;
	uint32_t channels[MAX_CHANNELS];

	struct spa_bt_transport_volume volumes[SPA_BT_VOLUME_ID_TERM];

	int acquire_refcount;
	bool acquired;
	bool keepalive;
	int error_count;
	uint64_t last_error_time;
	int fd;
	uint16_t read_mtu;
	uint16_t write_mtu;
	unsigned int delay_us;
	unsigned int latency_us;
	uint8_t bap_cig;
	uint8_t bap_cis;
	uint8_t bap_big;
	uint8_t bap_bis;

	struct spa_bt_iso_io *iso_io;
	struct spa_bt_sco_io *sco_io;

	struct spa_source volume_timer;
	struct spa_source release_timer;
	DBusPendingCall *acquire_call;
	DBusPendingCall *volume_call;

	struct spa_hook_list listener_list;
	struct spa_callbacks impl;

	/* For ASHA */
	bool asha_right_side;
	uint64_t hisyncid;

	/* user_data must be the last item in the struct */
	void *user_data;
};

struct spa_bt_transport *spa_bt_transport_create(struct spa_bt_monitor *monitor, char *path, size_t extra);
void spa_bt_transport_free(struct spa_bt_transport *transport);
void spa_bt_transport_set_state(struct spa_bt_transport *transport, enum spa_bt_transport_state state);
struct spa_bt_transport *spa_bt_transport_find(struct spa_bt_monitor *monitor, const char *path);
struct spa_bt_transport *spa_bt_transport_find_full(struct spa_bt_monitor *monitor,
                                                    bool (*callback) (struct spa_bt_transport *t, const void *data),
                                                    const void *data);
int64_t spa_bt_transport_get_delay_nsec(struct spa_bt_transport *transport);
bool spa_bt_transport_volume_enabled(struct spa_bt_transport *transport);

int spa_bt_transport_acquire(struct spa_bt_transport *t, bool optional);
int spa_bt_transport_release(struct spa_bt_transport *t);
int spa_bt_transport_keepalive(struct spa_bt_transport *t, bool keepalive);
int spa_bt_transport_ensure_sco_io(struct spa_bt_transport *t, struct spa_loop *data_loop, struct spa_system *data_system);

#define spa_bt_transport_emit(t,m,v,...)		spa_hook_list_call(&(t)->listener_list, \
								struct spa_bt_transport_events,	\
								m, v, ##__VA_ARGS__)
#define spa_bt_transport_emit_destroy(t)		spa_bt_transport_emit(t, destroy, 0)
#define spa_bt_transport_emit_delay_changed(t)		spa_bt_transport_emit(t, delay_changed, 0)
#define spa_bt_transport_emit_state_changed(t,...)	spa_bt_transport_emit(t, state_changed, 0, __VA_ARGS__)
#define spa_bt_transport_emit_volume_changed(t)		spa_bt_transport_emit(t, volume_changed, 0)

#define spa_bt_transport_add_listener(t,listener,events,data) \
        spa_hook_list_append(&(t)->listener_list, listener, events, data)

#define spa_bt_transport_set_implementation(t,_impl,_data) \
			(t)->impl = SPA_CALLBACKS_INIT(_impl, _data)

#define spa_bt_transport_impl(t,m,v,...)		\
({							\
	int res = 0;					\
	spa_callbacks_call_res(&(t)->impl,		\
		struct spa_bt_transport_implementation,	\
		res, m, v, ##__VA_ARGS__);		\
	res;						\
})

#define spa_bt_transport_destroy(t)		spa_bt_transport_impl(t, destroy, 0)
#define spa_bt_transport_set_volume(t,...)	spa_bt_transport_impl(t, set_volume, 0, __VA_ARGS__)
#define spa_bt_transport_set_delay(t,...)	spa_bt_transport_impl(t, set_delay, 0, __VA_ARGS__)

static inline enum spa_bt_transport_state spa_bt_transport_state_from_string(const char *value)
{
	if (strcasecmp("idle", value) == 0)
		return SPA_BT_TRANSPORT_STATE_IDLE;
	else if ((strcasecmp("pending", value) == 0) || (strcasecmp("broadcasting", value) == 0))
		return SPA_BT_TRANSPORT_STATE_PENDING;
	else if (strcasecmp("active", value) == 0)
		return SPA_BT_TRANSPORT_STATE_ACTIVE;
	else
		return SPA_BT_TRANSPORT_STATE_IDLE;
}

#define DEFAULT_AG_VOLUME	1.0f
#define DEFAULT_RX_VOLUME	1.0f
#define DEFAULT_TX_VOLUME	0.064f /* spa_bt_volume_hw_to_linear(40, 100) */

/* AVRCP/HSP volume is considered as percentage, so map it to pulseaudio (cubic) volume. */
static inline uint32_t spa_bt_volume_linear_to_hw(double v, uint32_t hw_volume_max)
{
	if (v <= 0.0)
		return 0;
	if (v >= 1.0)
		return hw_volume_max;
	return SPA_CLAMP((uint64_t) lround(cbrt(v) * hw_volume_max),
			 0u, hw_volume_max);
}

static inline double spa_bt_volume_hw_to_linear(uint32_t v, uint32_t hw_volume_max)
{
	double f;
	if (v <= 0)
		return 0.0;
	if (v >= hw_volume_max)
		return 1.0;
	f = ((double) v / hw_volume_max);
	return f * f * f;
}

enum spa_bt_feature {
	SPA_BT_FEATURE_MSBC		= (1 << 0),
	SPA_BT_FEATURE_MSBC_ALT1	= (1 << 1),
	SPA_BT_FEATURE_MSBC_ALT1_RTL	= (1 << 2),
	SPA_BT_FEATURE_HW_VOLUME	= (1 << 3),
	SPA_BT_FEATURE_HW_VOLUME_MIC	= (1 << 4),
	SPA_BT_FEATURE_SBC_XQ		= (1 << 5),
	SPA_BT_FEATURE_FASTSTREAM	= (1 << 6),
	SPA_BT_FEATURE_A2DP_DUPLEX	= (1 << 7),
};

struct spa_bt_quirks;

struct spa_bt_quirks *spa_bt_quirks_create(const struct spa_dict *info, struct spa_log *log);
int spa_bt_quirks_get_features(const struct spa_bt_quirks *quirks,
		const struct spa_bt_adapter *adapter,
		const struct spa_bt_device *device,
		uint32_t *features);
void spa_bt_quirks_log_features(const struct spa_bt_quirks *this,
		const struct spa_bt_adapter *adapter,
		const struct spa_bt_device *device);
void spa_bt_quirks_destroy(struct spa_bt_quirks *quirks);

int spa_bt_adapter_has_msbc(struct spa_bt_adapter *adapter);

struct spa_bt_backend_implementation {
#define SPA_VERSION_BT_BACKEND_IMPLEMENTATION	0
	uint32_t version;

	int (*free) (void *data);
	int (*register_profiles) (void *data);
	int (*unregister_profiles) (void *data);
	int (*ensure_codec) (void *data, struct spa_bt_device *device, unsigned int codec);
	int (*supports_codec) (void *data, struct spa_bt_device *device, unsigned int codec);
};

struct spa_bt_backend {
	struct spa_callbacks impl;
	const char *name;
	bool available;
	bool exclusive;
};

#define spa_bt_backend_set_implementation(b,_impl,_data) \
			(b)->impl = SPA_CALLBACKS_INIT(_impl, _data)

#define spa_bt_backend_impl(b,m,v,...)				\
({								\
	int res = -ENOTSUP;					\
	if (b)							\
		spa_callbacks_call_res(&(b)->impl,		\
			struct spa_bt_backend_implementation,	\
			res, m, v, ##__VA_ARGS__);		\
	res;							\
})

#define spa_bt_backend_free(b)			spa_bt_backend_impl(b, free, 0)
#define spa_bt_backend_register_profiles(b)	spa_bt_backend_impl(b, register_profiles, 0)
#define spa_bt_backend_unregister_profiles(b)	spa_bt_backend_impl(b, unregister_profiles, 0)
#define spa_bt_backend_ensure_codec(b,...)	spa_bt_backend_impl(b, ensure_codec, 0, __VA_ARGS__)
#define spa_bt_backend_supports_codec(b,...)	spa_bt_backend_impl(b, supports_codec, 0, __VA_ARGS__)

static inline struct spa_bt_backend *dummy_backend_new(struct spa_bt_monitor *monitor,
		void *dbus_connection,
		const struct spa_dict *info,
		const struct spa_bt_quirks *quirks,
		const struct spa_support *support,
		uint32_t n_support)
{
	return NULL;
}

#ifdef HAVE_BLUEZ_5_BACKEND_NATIVE
struct spa_bt_backend *backend_native_new(struct spa_bt_monitor *monitor,
		void *dbus_connection,
		const struct spa_dict *info,
		const struct spa_bt_quirks *quirks,
		const struct spa_support *support,
		uint32_t n_support);
#else
#define backend_native_new	dummy_backend_new
#endif

#define OFONO_SERVICE "org.ofono"
#ifdef HAVE_BLUEZ_5_BACKEND_OFONO
struct spa_bt_backend *backend_ofono_new(struct spa_bt_monitor *monitor,
		void *dbus_connection,
		const struct spa_dict *info,
		const struct spa_bt_quirks *quirks,
		const struct spa_support *support,
		uint32_t n_support);
#else
#define backend_ofono_new	dummy_backend_new
#endif

#define HSPHFPD_SERVICE "org.hsphfpd"
#ifdef HAVE_BLUEZ_5_BACKEND_HSPHFPD
struct spa_bt_backend *backend_hsphfpd_new(struct spa_bt_monitor *monitor,
		void *dbus_connection,
		const struct spa_dict *info,
		const struct spa_bt_quirks *quirks,
		const struct spa_support *support,
		uint32_t n_support);
#else
#define backend_hsphfpd_new	dummy_backend_new
#endif

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_BLUEZ5_DEFS_H */
