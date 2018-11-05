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

#ifndef __SPA_BLUEZ5_DEFS_H__
#define __SPA_BLUEZ5_DEFS_H__

#ifdef __cplusplus
extern "C" {
#endif

#define BLUEZ_SERVICE "org.bluez"
#define BLUEZ_ADAPTER_INTERFACE BLUEZ_SERVICE ".Adapter1"
#define BLUEZ_DEVICE_INTERFACE BLUEZ_SERVICE ".Device1"
#define BLUEZ_MEDIA_INTERFACE BLUEZ_SERVICE ".Media1"
#define BLUEZ_MEDIA_ENDPOINT_INTERFACE BLUEZ_SERVICE ".MediaEndpoint1"
#define BLUEZ_MEDIA_TRANSPORT_INTERFACE BLUEZ_SERVICE ".MediaTransport1"

#define ENDPOINT_INTROSPECT_XML                                         \
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


#define BLUEZ_ERROR_NOT_SUPPORTED "org.bluez.Error.NotSupported"

#define SPA_BT_UUID_A2DP_SOURCE "0000110A-0000-1000-8000-00805F9B34FB"
#define SPA_BT_UUID_A2DP_SINK   "0000110B-0000-1000-8000-00805F9B34FB"
#define SPA_BT_UUID_HSP_HS      "00001108-0000-1000-8000-00805F9B34FB"
#define SPA_BT_UUID_HSP_AG      "00001112-0000-1000-8000-00805F9B34FB"
#define SPA_BT_UUID_HFP_HF      "0000111E-0000-1000-8000-00805F9B34FB"
#define SPA_BT_UUID_HFP_AG      "0000111F-0000-1000-8000-00805F9B34FB"

enum spa_bt_profile {
        SPA_BT_PROFILE_NULL =	0,
        SPA_BT_PROFILE_A2DP_SOURCE =	(1 << 0),
        SPA_BT_PROFILE_A2DP_SINK =	(1 << 1),
        SPA_BT_PROFILE_HSP_HS =	(1 << 2),
        SPA_BT_PROFILE_HSP_AG =	(1 << 3),
        SPA_BT_PROFILE_HFP_HF =	(1 << 4),
        SPA_BT_PROFILE_HFP_AG =	(1 << 5),
};

static inline enum spa_bt_profile spa_bt_profile_from_uuid(const char *uuid)
{
	if (strcasecmp(uuid, SPA_BT_UUID_A2DP_SOURCE) == 0)
		return SPA_BT_PROFILE_A2DP_SOURCE;
	else if (strcasecmp(uuid, SPA_BT_UUID_A2DP_SINK) == 0)
		return SPA_BT_PROFILE_A2DP_SINK;
	else if (strcasecmp(uuid, SPA_BT_UUID_HSP_HS) == 0)
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
};

enum spa_bt_transport_state {
        SPA_BT_TRANSPORT_STATE_IDLE,
        SPA_BT_TRANSPORT_STATE_PENDING,
        SPA_BT_TRANSPORT_STATE_ACTIVE,
};

struct spa_bt_transport {
	struct spa_list link;
	struct spa_bt_monitor *monitor;
	char *path;
	struct spa_bt_device *device;
	enum spa_bt_profile profile;
	enum spa_bt_transport_state state;
	int codec;
	void *configuration;
	int configuration_len;

	bool acquired;
	int fd;
	uint16_t read_mtu;
	uint16_t write_mtu;

	int (*acquire) (struct spa_bt_transport *trans, bool optional);

	int (*release) (struct spa_bt_transport *trans);
};

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

#endif /* __SPA_BLUEZ5_DEFS_H__ */
