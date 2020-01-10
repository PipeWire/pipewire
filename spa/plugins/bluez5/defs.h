/* Spa Bluez5 Monitor
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

#ifndef SPA_BLUEZ5_DEFS_H
#define SPA_BLUEZ5_DEFS_H

#ifdef __cplusplus
extern "C" {
#endif

#include <spa/utils/hook.h>

#define BLUEZ_SERVICE "org.bluez"
#define BLUEZ_PROFILE_MANAGER_INTERFACE BLUEZ_SERVICE ".ProfileManager1"
#define BLUEZ_PROFILE_INTERFACE BLUEZ_SERVICE ".Profile1"
#define BLUEZ_ADAPTER_INTERFACE BLUEZ_SERVICE ".Adapter1"
#define BLUEZ_DEVICE_INTERFACE BLUEZ_SERVICE ".Device1"
#define BLUEZ_MEDIA_INTERFACE BLUEZ_SERVICE ".Media1"
#define BLUEZ_MEDIA_ENDPOINT_INTERFACE BLUEZ_SERVICE ".MediaEndpoint1"
#define BLUEZ_MEDIA_TRANSPORT_INTERFACE BLUEZ_SERVICE ".MediaTransport1"

#define MIN_LATENCY	128
#define MAX_LATENCY	1024

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

#define SPA_BT_UUID_A2DP_SOURCE "0000110A-0000-1000-8000-00805F9B34FB"
#define SPA_BT_UUID_A2DP_SINK   "0000110B-0000-1000-8000-00805F9B34FB"
#define SPA_BT_UUID_HSP_HS      "00001108-0000-1000-8000-00805F9B34FB"
#define SPA_BT_UUID_HSP_HS_ALT  "00001131-0000-1000-8000-00805F9B34FB"
#define SPA_BT_UUID_HSP_AG      "00001112-0000-1000-8000-00805F9B34FB"
#define SPA_BT_UUID_HFP_HF      "0000111E-0000-1000-8000-00805F9B34FB"
#define SPA_BT_UUID_HFP_AG      "0000111F-0000-1000-8000-00805F9B34FB"

#define PROFILE_HSP_AG	"/Profile/HSPAG"
#define PROFILE_HSP_HS	"/Profile/HSPHS"
#define PROFILE_HFP_AG	"/Profile/HFPAG"
#define PROFILE_HFP_HS	"/Profile/HFPHS"

#define HSP_HS_DEFAULT_CHANNEL  3

enum spa_bt_profile {
        SPA_BT_PROFILE_NULL =		0,
        SPA_BT_PROFILE_A2DP_SINK =	(1 << 0),
        SPA_BT_PROFILE_A2DP_SOURCE =	(1 << 1),
        SPA_BT_PROFILE_HSP_HS =		(1 << 2),
        SPA_BT_PROFILE_HSP_AG =		(1 << 3),
        SPA_BT_PROFILE_HFP_HF =		(1 << 4),
        SPA_BT_PROFILE_HFP_AG =		(1 << 5),

        SPA_BT_PROFILE_HEADSET_HEAD_UNIT = (SPA_BT_PROFILE_HSP_HS | SPA_BT_PROFILE_HFP_HF),
        SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY = (SPA_BT_PROFILE_HSP_AG | SPA_BT_PROFILE_HFP_AG),
};

static inline enum spa_bt_profile spa_bt_profile_from_uuid(const char *uuid)
{
	if (strcasecmp(uuid, SPA_BT_UUID_A2DP_SOURCE) == 0)
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
	else
		return 0;
}

static inline const char *spa_bt_profile_name (enum spa_bt_profile profile) {
      switch (profile) {
      case SPA_BT_PROFILE_A2DP_SOURCE:
        return "a2dp-source";
      case SPA_BT_PROFILE_A2DP_SINK:
        return "a2dp-sink";
      case SPA_BT_PROFILE_HSP_HS:
        return "hsp-hs";
      case SPA_BT_PROFILE_HSP_AG:
        return "hsp-ag";
      case SPA_BT_PROFILE_HFP_HF:
        return "hfp-hf";
      case SPA_BT_PROFILE_HFP_AG:
        return "hfp-ag";
      default:
        break;
      }
      return "unknown";
}

struct spa_bt_monitor;

struct spa_bt_adapter {
	struct spa_list link;
	struct spa_bt_monitor *monitor;
	char *path;
	char *alias;
	char *address;
	char *name;
	uint32_t bluetooth_class;
	uint32_t profiles;
	int powered;
};

struct spa_bt_device {
	struct spa_list link;
	struct spa_bt_monitor *monitor;
	struct spa_bt_adapter *adapter;
	uint32_t id;
	char *path;
	char *alias;
	char *address;
	char *adapter_path;
	char *name;
	char *icon;
	uint32_t bluetooth_class;
	uint16_t appearance;
	uint16_t RSSI;
	int paired;
	int trusted;
	int connected;
	int blocked;
	uint32_t profiles;
	uint32_t connected_profiles;
	struct spa_source timer;
	struct spa_list transport_list;
	bool added;
};

enum spa_bt_transport_state {
        SPA_BT_TRANSPORT_STATE_IDLE,
        SPA_BT_TRANSPORT_STATE_PENDING,
        SPA_BT_TRANSPORT_STATE_ACTIVE,
};

struct spa_bt_transport_events {
#define SPA_VERSION_BT_TRANSPORT_EVENTS	0
	uint32_t version;

	void (*destroy) (void *data);
	void (*state_changed) (void *data, enum spa_bt_transport_state old,
			enum spa_bt_transport_state state);
};

struct spa_bt_transport_implementation {
#define SPA_VERSION_BT_TRANSPORT_IMPLEMENTATION	0
	uint32_t version;

	int (*acquire) (void *data, bool optional);
	int (*release) (void *data);
	int (*destroy) (void *data);
};

struct spa_bt_transport {
	struct spa_list link;
	struct spa_bt_monitor *monitor;
	char *path;
	struct spa_bt_device *device;
	struct spa_list device_link;
	enum spa_bt_profile profile;
	enum spa_bt_transport_state state;
	int codec;
	void *configuration;
	int configuration_len;

	bool acquired;
	int fd;
	uint16_t read_mtu;
	uint16_t write_mtu;
	void *user_data;

	struct spa_hook_list listener_list;
	struct spa_callbacks impl;
};

#define spa_bt_transport_emit(t,m,v,...)		spa_hook_list_call(&(t)->listener_list, \
								struct spa_bt_transport_events,	\
								m, v, ##__VA_ARGS__)
#define spa_bt_transport_emit_destroy(t)		spa_bt_transport_emit(t, destroy, 0)
#define spa_bt_transport_emit_state_changed(t,...)	spa_bt_transport_emit(t, state_changed, 0, __VA_ARGS__)

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

#define spa_bt_transport_acquire(t,o)	spa_bt_transport_impl(t, acquire, 0, o)
#define spa_bt_transport_release(t)	spa_bt_transport_impl(t, release, 0)
#define spa_bt_transport_destroy(t)	spa_bt_transport_impl(t, destroy, 0)

static inline enum spa_bt_transport_state spa_bt_transport_state_from_string(const char *value)
{
	if (strcasecmp("idle", value) == 0)
		return SPA_BT_TRANSPORT_STATE_IDLE;
	else if (strcasecmp("pending", value) == 0)
		return SPA_BT_TRANSPORT_STATE_PENDING;
	else if (strcasecmp("active", value) == 0)
		return SPA_BT_TRANSPORT_STATE_ACTIVE;
	else
		return SPA_BT_TRANSPORT_STATE_IDLE;
}


#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif /* SPA_BLUEZ5_DEFS_H */
