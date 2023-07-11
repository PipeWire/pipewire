/* Device/adapter/kernel quirk table */
/* SPDX-FileCopyrightText: Copyright © 2021 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#include <errno.h>
#include <stddef.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <regex.h>
#include <limits.h>
#include <sys/utsname.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <unistd.h>

#include <bluetooth/bluetooth.h>

#include <dbus/dbus.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/dbus.h>
#include <spa/support/plugin.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/utils/cleanup.h>
#include <spa/utils/hook.h>
#include <spa/utils/type.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/json.h>
#include <spa/utils/string.h>

#include "defs.h"

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.quirks");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

struct spa_bt_quirks {
	struct spa_log *log;

	int force_msbc;
	int force_hw_volume;
	int force_sbc_xq;
	int force_faststream;
	int force_a2dp_duplex;

	char *device_rules;
	char *adapter_rules;
	char *kernel_rules;
};

static enum spa_bt_feature parse_feature(const char *str)
{
	static const struct { const char *key; enum spa_bt_feature value; } feature_keys[] = {
		{ "msbc", SPA_BT_FEATURE_MSBC },
		{ "msbc-alt1", SPA_BT_FEATURE_MSBC_ALT1 },
		{ "msbc-alt1-rtl", SPA_BT_FEATURE_MSBC_ALT1_RTL },
		{ "hw-volume", SPA_BT_FEATURE_HW_VOLUME },
		{ "hw-volume-mic", SPA_BT_FEATURE_HW_VOLUME_MIC },
		{ "sbc-xq", SPA_BT_FEATURE_SBC_XQ },
		{ "faststream", SPA_BT_FEATURE_FASTSTREAM },
		{ "a2dp-duplex", SPA_BT_FEATURE_A2DP_DUPLEX },
	};
	SPA_FOR_EACH_ELEMENT_VAR(feature_keys, f) {
		if (spa_streq(str, f->key))
			return f->value;
	}
	return 0;
}

static int do_match(const char *rules, struct spa_dict *dict, uint32_t *no_features)
{
	struct spa_json rules_json = SPA_JSON_INIT(rules, strlen(rules));
	struct spa_json rules_arr, it[2];

	if (spa_json_enter_array(&rules_json, &rules_arr) <= 0)
		return 1;

	while (spa_json_enter_object(&rules_arr, &it[0]) > 0) {
		char key[256];
		int match = true;
		uint32_t no_features_cur = 0;

		while (spa_json_get_string(&it[0], key, sizeof(key)) > 0) {
			char val[4096];
			const char *str, *value;
			int len;
			bool success = false;

			if (spa_streq(key, "no-features")) {
				if (spa_json_enter_array(&it[0], &it[1]) > 0) {
					while (spa_json_get_string(&it[1], val, sizeof(val)) > 0)
						no_features_cur |= parse_feature(val);
				}
				continue;
			}

			if ((len = spa_json_next(&it[0], &value)) <= 0)
				break;

			if (spa_json_is_null(value, len)) {
				value = NULL;
			} else {
				if (spa_json_parse_stringn(value, len, val, sizeof(val)) < 0)
					continue;
				value = val;
			}

			str = spa_dict_lookup(dict, key);
			if (value == NULL) {
				success = str == NULL;
			} else if (str != NULL) {
				if (value[0] == '~') {
					regex_t r;
					if (regcomp(&r, value+1, REG_EXTENDED | REG_NOSUB) == 0) {
						if (regexec(&r, str, 0, NULL, 0) == 0)
							success = true;
						regfree(&r);
					}
				} else if (spa_streq(str, value)) {
					success = true;
				}
			}

			if (!success) {
				match = false;
				break;
			}
		}

		if (match) {
			*no_features = no_features_cur;
			return 0;
		}
	}
	return 0;
}

static int parse_force_flag(const struct spa_dict *info, const char *key)
{
	const char *str;
	str = spa_dict_lookup(info, key);
	if (str == NULL)
		return -1;
	else
		return (strcmp(str, "true") == 0 || atoi(str)) ? 1 : 0;
}

static void load_quirks(struct spa_bt_quirks *this, const char *str, size_t len)
{
	struct spa_json data = SPA_JSON_INIT(str, len);
	struct spa_json rules;
	char key[1024];

	if (spa_json_enter_object(&data, &rules) <= 0)
		spa_json_init(&rules, str, len);

	while (spa_json_get_string(&rules, key, sizeof(key)) > 0) {
		int sz;
		const char *value;

		if ((sz = spa_json_next(&rules, &value)) <= 0)
			break;

		if (!spa_json_is_container(value, sz))
			continue;

		sz = spa_json_container_len(&rules, value, sz);

		if (spa_streq(key, "bluez5.features.kernel") && !this->kernel_rules)
			this->kernel_rules = strndup(value, sz);
		else if (spa_streq(key, "bluez5.features.adapter") && !this->adapter_rules)
			this->adapter_rules = strndup(value, sz);
		else if (spa_streq(key, "bluez5.features.device") && !this->device_rules)
			this->device_rules = strndup(value, sz);
	}
}

static int load_conf(struct spa_bt_quirks *this, const char *path)
{
	char *data;
	struct stat sbuf;
	spa_autoclose int fd = -1;

	spa_log_debug(this->log, "loading %s", path);

	if ((fd = open(path, O_CLOEXEC | O_RDONLY)) < 0)
		return -errno;
	if (fstat(fd, &sbuf) < 0)
		return -errno;
	if ((data = mmap(NULL, sbuf.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED)
		return -errno;

	load_quirks(this, data, sbuf.st_size);
	munmap(data, sbuf.st_size);

	return 0;
}

struct spa_bt_quirks *spa_bt_quirks_create(const struct spa_dict *info, struct spa_log *log)
{
	struct spa_bt_quirks *this;
	const char *str;

	if (!info) {
		errno = -EINVAL;
		return NULL;
	}

	this = calloc(1, sizeof(struct spa_bt_quirks));
	if (this == NULL)
		return NULL;

	this->log = log;

	spa_log_topic_init(this->log, &log_topic);

	this->force_sbc_xq = parse_force_flag(info, "bluez5.enable-sbc-xq");
	this->force_msbc = parse_force_flag(info, "bluez5.enable-msbc");
	this->force_hw_volume = parse_force_flag(info, "bluez5.enable-hw-volume");
	this->force_faststream = parse_force_flag(info, "bluez5.enable-faststream");
	this->force_a2dp_duplex = parse_force_flag(info, "bluez5.enable-a2dp-duplex");

	if ((str = spa_dict_lookup(info, "bluez5.hardware-database")) != NULL) {
		spa_log_debug(this->log, "loading session manager provided data");
		load_quirks(this, str, strlen(str));
	} else {
		char path[PATH_MAX];
		const char *dir = getenv("SPA_DATA_DIR");
		int res;

		if (dir == NULL)
			dir = SPADATADIR;

		if (spa_scnprintf(path, sizeof(path), "%s/bluez5/bluez-hardware.conf", dir) >= 0)
			if ((res = load_conf(this, path)) < 0)
				spa_log_warn(this->log, "failed to load '%s': %s", path,
						spa_strerror(res));
	}
	if (!(this->kernel_rules && this->adapter_rules && this->device_rules))
		spa_log_warn(this->log, "failed to load bluez-hardware.conf");

	return this;
}

void spa_bt_quirks_destroy(struct spa_bt_quirks *this)
{
	free(this->kernel_rules);
	free(this->adapter_rules);
	free(this->device_rules);
	free(this);
}

static void log_props(struct spa_log *log, const struct spa_dict *dict)
{
	const struct spa_dict_item *item;
	spa_dict_for_each(item, dict)
		spa_log_debug(log, "quirk property %s=%s", item->key, item->value);
}

static void strtolower(char *src, char *dst, int maxsize)
{
	while (maxsize > 1 && *src != '\0') {
		*dst = (*src >= 'A' && *src <= 'Z') ? ('a' + (*src - 'A')) : *src;
		++src;
		++dst;
		--maxsize;
	}
	if (maxsize > 0)
		*dst = '\0';
}

int spa_bt_quirks_get_features(const struct spa_bt_quirks *this,
		const struct spa_bt_adapter *adapter,
		const struct spa_bt_device *device,
		uint32_t *features)
{
	struct spa_dict props;
	struct spa_dict_item items[5];
	int res;

	*features = ~(uint32_t)0;

	/* Kernel */
	if (this->kernel_rules) {
		uint32_t no_features = 0;
		int nitems = 0;
		struct utsname name;
		if ((res = uname(&name)) < 0)
			return res;
		items[nitems++] = SPA_DICT_ITEM_INIT("sysname", name.sysname);
		items[nitems++] = SPA_DICT_ITEM_INIT("release", name.release);
		items[nitems++] = SPA_DICT_ITEM_INIT("version", name.version);
		props = SPA_DICT_INIT(items, nitems);
		log_props(this->log, &props);
		do_match(this->kernel_rules, &props, &no_features);
		spa_log_debug(this->log, "kernel quirks:%08x", no_features);
		*features &= ~no_features;
	}

	/* Adapter */
	if (this->adapter_rules && adapter) {
		uint32_t no_features = 0;
		int nitems = 0;
		char vendor_id[64], product_id[64], address[64];

		if (spa_bt_format_vendor_product_id(
				adapter->source_id, adapter->vendor_id, adapter->product_id,
				vendor_id, sizeof(vendor_id), product_id, sizeof(product_id)) == 0) {
			items[nitems++] = SPA_DICT_ITEM_INIT("vendor-id", vendor_id);
			items[nitems++] = SPA_DICT_ITEM_INIT("product-id", product_id);
		}
		items[nitems++] = SPA_DICT_ITEM_INIT("bus-type",
				(adapter->bus_type == BUS_TYPE_USB) ? "usb" : "other");
		if (adapter->address) {
			strtolower(adapter->address, address, sizeof(address));
			items[nitems++] = SPA_DICT_ITEM_INIT("address", address);
		}
		props = SPA_DICT_INIT(items, nitems);
		log_props(this->log, &props);
		do_match(this->adapter_rules, &props, &no_features);
		spa_log_debug(this->log, "adapter quirks:%08x", no_features);
		*features &= ~no_features;
	}

	/* Device */
	if (this->device_rules && device) {
		uint32_t no_features = 0;
		int nitems = 0;
		char vendor_id[64], product_id[64], version_id[64], address[64];
		if (spa_bt_format_vendor_product_id(
				device->source_id, device->vendor_id, device->product_id,
				vendor_id, sizeof(vendor_id), product_id, sizeof(product_id)) == 0) {
			snprintf(version_id, sizeof(version_id), "%04x",
					(unsigned int)device->version_id);
			items[nitems++] = SPA_DICT_ITEM_INIT("vendor-id", vendor_id);
			items[nitems++] = SPA_DICT_ITEM_INIT("product-id", product_id);
			items[nitems++] = SPA_DICT_ITEM_INIT("version-id", version_id);
		}
		if (device->name)
			items[nitems++] = SPA_DICT_ITEM_INIT("name", device->name);
		if (device->address) {
			strtolower(device->address, address, sizeof(address));
			items[nitems++] = SPA_DICT_ITEM_INIT("address", address);
		}
		props = SPA_DICT_INIT(items, nitems);
		log_props(this->log, &props);
		do_match(this->device_rules, &props, &no_features);
		spa_log_debug(this->log, "device quirks:%08x", no_features);
		*features &= ~no_features;
	}

	/* Force flags */
	if (this->force_msbc != -1) {
		SPA_FLAG_UPDATE(*features, SPA_BT_FEATURE_MSBC, this->force_msbc);
		SPA_FLAG_UPDATE(*features, SPA_BT_FEATURE_MSBC_ALT1, this->force_msbc);
		SPA_FLAG_UPDATE(*features, SPA_BT_FEATURE_MSBC_ALT1_RTL, this->force_msbc);
	}

	if (this->force_hw_volume != -1)
		SPA_FLAG_UPDATE(*features, SPA_BT_FEATURE_HW_VOLUME, this->force_hw_volume);

	if (this->force_sbc_xq != -1)
		SPA_FLAG_UPDATE(*features, SPA_BT_FEATURE_SBC_XQ, this->force_sbc_xq);

	if (this->force_faststream != -1)
		SPA_FLAG_UPDATE(*features, SPA_BT_FEATURE_FASTSTREAM, this->force_faststream);

	if (this->force_a2dp_duplex != -1)
		SPA_FLAG_UPDATE(*features, SPA_BT_FEATURE_A2DP_DUPLEX, this->force_a2dp_duplex);

	return 0;
}
