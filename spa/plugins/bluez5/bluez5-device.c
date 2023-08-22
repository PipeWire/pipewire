/* Spa Bluez5 Device */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>

#include <spa/support/log.h>
#include <spa/utils/type.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/node/node.h>
#include <spa/support/loop.h>
#include <spa/support/plugin.h>
#include <spa/support/i18n.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/monitor/event.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>
#include <spa/param/param.h>
#include <spa/param/audio/raw.h>
#include <spa/param/bluetooth/audio.h>
#include <spa/param/bluetooth/type-info.h>
#include <spa/debug/pod.h>
#include <spa/debug/log.h>

#include "defs.h"
#include "media-codecs.h"

static struct spa_log_topic log_topic = SPA_LOG_TOPIC(0, "spa.bluez5.device");
#undef SPA_LOG_TOPIC_DEFAULT
#define SPA_LOG_TOPIC_DEFAULT &log_topic

#define MAX_DEVICES	64

#define DEVICE_ID_SOURCE	0
#define DEVICE_ID_SINK		1
#define DEVICE_ID_SOURCE_SET	2
#define DEVICE_ID_SINK_SET	3
#define DYNAMIC_NODE_ID_FLAG	0x1000

static struct spa_i18n *_i18n;

#define _(_str)	 spa_i18n_text(_i18n,(_str))
#define N_(_str) (_str)

enum {
	DEVICE_PROFILE_OFF = 0,
	DEVICE_PROFILE_AG = 1,
	DEVICE_PROFILE_A2DP = 2,
	DEVICE_PROFILE_HSP_HFP = 3,
	DEVICE_PROFILE_BAP = 4,
	DEVICE_PROFILE_LAST = DEVICE_PROFILE_BAP,
};

struct props {
	enum spa_bluetooth_audio_codec codec;
	bool offload_active;
};

static void reset_props(struct props *props)
{
	props->codec = 0;
	props->offload_active = false;
}

struct impl;

struct node {
	struct impl *impl;
	struct spa_bt_transport *transport;
	struct spa_hook transport_listener;
	uint32_t id;
	unsigned int active:1;
	unsigned int mute:1;
	unsigned int save:1;
	unsigned int a2dp_duplex:1;
	unsigned int offload_acquired:1;
	uint32_t n_channels;
	int64_t latency_offset;
	uint32_t channels[SPA_AUDIO_MAX_CHANNELS];
	float volumes[SPA_AUDIO_MAX_CHANNELS];
	float soft_volumes[SPA_AUDIO_MAX_CHANNELS];
};

struct dynamic_node
{
	struct impl *impl;
	struct spa_bt_transport *transport;
	struct spa_hook transport_listener;
	uint32_t id;
	const char *factory_name;
	bool a2dp_duplex;
};

struct device_set_member {
	struct impl *impl;
	struct spa_bt_transport *transport;
	struct spa_hook listener;
};

struct device_set {
	struct impl *impl;
	char *path;
	bool leader;
	uint32_t sinks;
	uint32_t sources;
	struct device_set_member sink[SPA_AUDIO_MAX_CHANNELS];
	struct device_set_member source[SPA_AUDIO_MAX_CHANNELS];
};

struct impl {
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;

	uint32_t info_all;
	struct spa_device_info info;
#define IDX_EnumProfile		0
#define IDX_Profile		1
#define IDX_EnumRoute		2
#define IDX_Route		3
#define IDX_PropInfo		4
#define IDX_Props		5
	struct spa_param_info params[6];

	struct spa_hook_list hooks;

	struct props props;

	struct spa_bt_device *bt_dev;
	struct spa_hook bt_dev_listener;

	uint32_t profile;
	unsigned int switching_codec:1;
	unsigned int save_profile:1;
	uint32_t prev_bt_connected_profiles;

	struct device_set device_set;

	const struct media_codec **supported_codecs;
	size_t supported_codec_count;

	struct dynamic_node dyn_media_source;
	struct dynamic_node dyn_media_sink;
	struct dynamic_node dyn_sco_source;
	struct dynamic_node dyn_sco_sink;

#define MAX_SETTINGS 32
	struct spa_dict_item setting_items[MAX_SETTINGS];
	struct spa_dict setting_dict;

	struct node nodes[4];
};

static void init_node(struct impl *this, struct node *node, uint32_t id)
{
	uint32_t i;

	spa_zero(*node);
	node->id = id;
	for (i = 0; i < SPA_AUDIO_MAX_CHANNELS; i++) {
		node->volumes[i] = 1.0f;
		node->soft_volumes[i] = 1.0f;
	}
}

static void get_media_codecs(struct impl *this, enum spa_bluetooth_audio_codec id, const struct media_codec **codecs, size_t size)
{
	const struct media_codec * const *c;

	spa_assert(size > 0);
	spa_assert(this->supported_codecs);

	for (c = this->supported_codecs; *c && size > 1; ++c) {
		if ((*c)->id == id || id == 0) {
			*codecs++ = *c;
			--size;
		}
	}

	*codecs = NULL;
}

static const struct media_codec *get_supported_media_codec(struct impl *this, enum spa_bluetooth_audio_codec id, size_t *idx)
{
	const struct media_codec *media_codec = NULL;
	size_t i;
	for (i = 0; i < this->supported_codec_count; ++i) {
		if (this->supported_codecs[i]->id == id) {
			media_codec = this->supported_codecs[i];
			if (idx)
				*idx = i;
		}
	}
	return media_codec;
}

static bool is_bap_client(struct impl *this)
{
	struct spa_bt_device *device = this->bt_dev;
	struct spa_bt_transport *t;

	spa_list_for_each(t, &device->transport_list, device_link) {
		if (t->bap_initiator)
			return true;
	}

	return false;
}

static bool can_bap_codec_switch(struct impl *this)
{
	if (!is_bap_client(this))
		return false;

	/* XXX: codec switching for source/duplex is not currently
	 * XXX: implemented properly. TODO: fix this
	 */
	if (this->bt_dev->connected_profiles & SPA_BT_PROFILE_BAP_SOURCE)
		return false;

	return true;
}

static unsigned int get_hfp_codec(enum spa_bluetooth_audio_codec id)
{
	switch (id) {
	case SPA_BLUETOOTH_AUDIO_CODEC_CVSD:
		return HFP_AUDIO_CODEC_CVSD;
	case SPA_BLUETOOTH_AUDIO_CODEC_MSBC:
		return HFP_AUDIO_CODEC_MSBC;
	default:
		return 0;
	}
}

static enum spa_bluetooth_audio_codec get_hfp_codec_id(unsigned int codec)
{
	switch (codec) {
	case HFP_AUDIO_CODEC_MSBC:
		return SPA_BLUETOOTH_AUDIO_CODEC_MSBC;
	case HFP_AUDIO_CODEC_CVSD:
		return SPA_BLUETOOTH_AUDIO_CODEC_CVSD;
	}
	return SPA_ID_INVALID;
}

static const char *get_hfp_codec_description(unsigned int codec)
{
	switch (codec) {
	case HFP_AUDIO_CODEC_MSBC:
		return "mSBC";
	case HFP_AUDIO_CODEC_CVSD:
		return "CVSD";
	}
	return "unknown";
}

static const char *get_hfp_codec_name(unsigned int codec)
{
	switch (codec) {
	case HFP_AUDIO_CODEC_MSBC:
		return "msbc";
	case HFP_AUDIO_CODEC_CVSD:
		return "cvsd";
	}
	return "unknown";
}

static const char *get_codec_name(struct spa_bt_transport *t, bool a2dp_duplex)
{
	if (t->media_codec != NULL) {
		if (a2dp_duplex && t->media_codec->duplex_codec)
			return t->media_codec->duplex_codec->name;
		return t->media_codec->name;
	}
	return get_hfp_codec_name(t->codec);
}

static void transport_destroy(void *userdata)
{
	struct node *node = userdata;
	node->transport = NULL;
}

static void emit_node_props(struct impl *this, struct node *node, bool full)
{
	struct spa_event *event;
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	struct spa_pod_frame f[1];

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_EVENT_Device, SPA_DEVICE_EVENT_ObjectConfig);
	spa_pod_builder_prop(&b, SPA_EVENT_DEVICE_Object, 0);
	spa_pod_builder_int(&b, node->id);
	spa_pod_builder_prop(&b, SPA_EVENT_DEVICE_Props, 0);
	spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Props, SPA_EVENT_DEVICE_Props,
			SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float),
				SPA_TYPE_Float, node->n_channels, node->volumes),
			SPA_PROP_softVolumes, SPA_POD_Array(sizeof(float),
				SPA_TYPE_Float, node->n_channels, node->soft_volumes),
			SPA_PROP_channelMap, SPA_POD_Array(sizeof(uint32_t),
				SPA_TYPE_Id, node->n_channels, node->channels));
	if (full) {
		spa_pod_builder_add(&b,
			SPA_PROP_mute, SPA_POD_Bool(node->mute),
			SPA_PROP_softMute, SPA_POD_Bool(node->mute),
			SPA_PROP_latencyOffsetNsec, SPA_POD_Long(node->latency_offset),
			0);
	}
	event = spa_pod_builder_pop(&b, &f[0]);

	spa_device_emit_event(&this->hooks, event);
}

static void emit_volume(struct impl *this, struct node *node)
{
	emit_node_props(this, node, false);
}

static void emit_info(struct impl *this, bool full);

static float get_soft_volume_boost(struct node *node)
{
	const struct media_codec *codec = node->transport ? node->transport->media_codec : NULL;

	/*
	 * For A2DP duplex, the duplex microphone channel sometimes does not appear
	 * to have hardware gain, and input volume is very low.
	 *
	 * Work around this by boosting the software volume level, i.e. adjust
	 * the scale on the user-visible volume control to something more sensible.
	 * If this causes clipping, the user can just reduce the mic volume to
	 * bring SW gain below 1.
	 */
	if (node->a2dp_duplex && node->transport && codec && codec->info &&
			spa_atob(spa_dict_lookup(codec->info, "duplex.boost")) &&
			node->id == DEVICE_ID_SOURCE &&
			!node->transport->volumes[SPA_BT_VOLUME_ID_RX].active)
		return 10.0f;	/* 20 dB boost */

	/* In all other cases, no boost */
	return 1.0f;
}

static float node_get_hw_volume(struct node *node)
{
	uint32_t i;
	float hw_volume = 0.0f;
	for (i = 0; i < node->n_channels; i++)
		hw_volume = SPA_MAX(node->volumes[i], hw_volume);
	return SPA_MIN(hw_volume, 1.0f);
}

static void node_update_soft_volumes(struct node *node, float hw_volume)
{
	for (uint32_t i = 0; i < node->n_channels; ++i) {
		node->soft_volumes[i] = hw_volume > 0.0f
			? node->volumes[i] / hw_volume
			: 0.0f;
	}
}

static bool node_update_volume_from_transport(struct node *node, bool reset)
{
	struct impl *impl = node->impl;
	struct spa_bt_transport_volume *t_volume;
	float prev_hw_volume;

	if (!node->active || !node->transport || !spa_bt_transport_volume_enabled(node->transport))
		return false;

	/* PW is the controller for remote device. */
	if (impl->profile != DEVICE_PROFILE_A2DP
	    && impl->profile != DEVICE_PROFILE_BAP
	    && impl->profile !=  DEVICE_PROFILE_HSP_HFP)
		return false;

	t_volume = &node->transport->volumes[node->id];

	if (!t_volume->active)
		return false;

	prev_hw_volume = node_get_hw_volume(node);

	if (!reset) {
		for (uint32_t i = 0; i < node->n_channels; ++i) {
			node->volumes[i] = prev_hw_volume > 0.0f
				? node->volumes[i] * t_volume->volume / prev_hw_volume
				: t_volume->volume;
		}
	} else {
		for (uint32_t i = 0; i < node->n_channels; ++i)
			node->volumes[i] = t_volume->volume;
	}

	node_update_soft_volumes(node, t_volume->volume);

	/*
	 * Consider volume changes from the headset as requested
	 * by the user, and to be saved by the SM.
	 */
	node->save = true;

	return true;
}

static void volume_changed(void *userdata)
{
	struct node *node = userdata;
	struct impl *impl = node->impl;

	if (!node_update_volume_from_transport(node, false))
		return;

	emit_volume(impl, node);

	impl->info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
	impl->params[IDX_Route].flags ^= SPA_PARAM_INFO_SERIAL;
	emit_info(impl, false);
}

static const struct spa_bt_transport_events transport_events = {
	SPA_VERSION_BT_DEVICE_EVENTS,
	.destroy = transport_destroy,
	.volume_changed = volume_changed,
};

static int node_offload_set_active(struct node *node, bool active)
{
	int res = 0;

	if (node->transport == NULL || !node->active)
		return -ENOTSUP;

	if (active && !node->offload_acquired)
		res = spa_bt_transport_acquire(node->transport, false);
	else if (!active && node->offload_acquired)
		res = spa_bt_transport_release(node->transport);

	if (res >= 0)
		node->offload_acquired = active;

	return res;
}

static void get_channels(struct spa_bt_transport *t, bool a2dp_duplex, uint32_t *n_channels, uint32_t *channels)
{
	const struct media_codec *codec;
	struct spa_audio_info info = { 0 };

	if (!a2dp_duplex || !t->media_codec || !t->media_codec->duplex_codec) {
		*n_channels = t->n_channels;
		memcpy(channels, t->channels, t->n_channels * sizeof(uint32_t));
		return;
	}

	codec = t->media_codec->duplex_codec;

	if (!codec->validate_config ||
			codec->validate_config(codec, 0,
					t->configuration, t->configuration_len,
					&info) < 0) {
		*n_channels = 1;
		channels[0] = SPA_AUDIO_CHANNEL_MONO;
		return;
	}

	*n_channels = info.info.raw.channels;
	memcpy(channels, info.info.raw.position,
			info.info.raw.channels * sizeof(uint32_t));
}

static const char *get_channel_name(uint32_t channel)
{
        int i;
        for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (spa_type_audio_channel[i].type == channel)
			return spa_debug_type_short_name(spa_type_audio_channel[i].name);
        }
        return NULL;
}

static int channel_position_cmp(const void *pa, const void *pb)
{
	uint32_t a = *(uint32_t *)pa, b = *(uint32_t *)pb;
	return (int)a - (int)b;
}

static void emit_device_set_node(struct impl *this, uint32_t id)
{
	struct spa_bt_device *device = this->bt_dev;
	struct node *node = &this->nodes[id];
	struct spa_device_object_info info;
	struct spa_dict_item items[7];
	char str_id[32], members_json[8192], channels_json[512];
	struct device_set_member *members;
	uint32_t n_members;
	uint32_t n_items = 0;
	struct spa_strbuf json;
	unsigned int i;

	items[n_items++] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_ADDRESS, device->address);
	items[n_items++] = SPA_DICT_ITEM_INIT("api.bluez5.set", this->device_set.path);
	items[n_items++] = SPA_DICT_ITEM_INIT("api.bluez5.set.leader", "true");
	snprintf(str_id, sizeof(str_id), "%d", id);
	items[n_items++] = SPA_DICT_ITEM_INIT("card.profile.device", str_id);

	if (id == DEVICE_ID_SOURCE_SET) {
		items[n_items++] = SPA_DICT_ITEM_INIT("media.class", "Audio/Source");
		members = this->device_set.source;
		n_members = this->device_set.sources;
	} else if (id == DEVICE_ID_SINK_SET) {
		items[n_items++] = SPA_DICT_ITEM_INIT("media.class", "Audio/Sink");
		members = this->device_set.sink;
		n_members = this->device_set.sinks;
	} else {
		spa_assert_not_reached();
	}

	node->impl = this;
	node->active = true;
	node->transport = NULL;
	node->a2dp_duplex = false;
	node->offload_acquired = false;
	node->mute = false;
	node->save = false;
	node->latency_offset = 0;

	/* Form channel map from members */
	node->n_channels = 0;
	for (i = 0; i < n_members; ++i) {
		struct spa_bt_transport *t = members[i].transport;
		unsigned int j;

		for (j = 0; j < t->n_channels; ++j) {
			unsigned int k;

			if (!get_channel_name(t->channels[j]))
				continue;

			for (k = 0; k < node->n_channels; ++k) {
				if (node->channels[k] == t->channels[j])
					break;
			}
			if (k == node->n_channels && node->n_channels < SPA_AUDIO_MAX_CHANNELS)
				node->channels[node->n_channels++] = t->channels[j];
		}
	}

	qsort(node->channels, node->n_channels, sizeof(uint32_t), channel_position_cmp);

	for (i = 0; i < node->n_channels; ++i) {
		/* Session manager will override this, so put in some safe number */
		node->volumes[i] = node->soft_volumes[i] = 0.064;
	}

	/* Produce member info json */
	spa_strbuf_init(&json, members_json, sizeof(members_json));
	spa_strbuf_append(&json, "[");
	for (i = 0; i < n_members; ++i) {
		struct spa_bt_transport *t = members[i].transport;
		char object_path[512];
		unsigned int j;
		int member_id = (id == DEVICE_ID_SINK_SET) ? DEVICE_ID_SINK : DEVICE_ID_SOURCE;

		if (i > 0)
			spa_strbuf_append(&json, ",");
		spa_scnprintf(object_path, sizeof(object_path), "%s/%s-%d",
				this->device_set.path, t->device->address, member_id);
		spa_strbuf_append(&json, "{\"object.path\":\"%s\",\"channels\":[", object_path);
		for (j = 0; j < t->n_channels; ++j) {
			if (j > 0)
				spa_strbuf_append(&json, ",");
			spa_strbuf_append(&json, "\"%s\"", get_channel_name(t->channels[j]));
		}
		spa_strbuf_append(&json, "]}");
	}
	spa_strbuf_append(&json, "]");
	json.buffer[SPA_MIN(json.pos, json.maxsize-1)] = 0;
	items[n_items++] = SPA_DICT_ITEM_INIT("api.bluez5.set.members", members_json);

	spa_strbuf_init(&json, channels_json, sizeof(channels_json));
	spa_strbuf_append(&json, "[");
	for (i = 0; i < node->n_channels; ++i) {
		if (i > 0)
			spa_strbuf_append(&json, ",");
		spa_strbuf_append(&json, "\"%s\"", get_channel_name(node->channels[i]));
	}
	spa_strbuf_append(&json, "]");
	json.buffer[SPA_MIN(json.pos, json.maxsize-1)] = 0;
	items[n_items++] = SPA_DICT_ITEM_INIT("api.bluez5.set.channels", channels_json);

	/* Emit */
	info = SPA_DEVICE_OBJECT_INFO_INIT();
	info.type = SPA_TYPE_INTERFACE_Node;
	info.factory_name = (id == DEVICE_ID_SOURCE_SET) ? "source" : "sink";
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.props = &SPA_DICT_INIT(items, n_items);

	spa_device_emit_object_info(&this->hooks, id, &info);

	emit_node_props(this, &this->nodes[id], true);
}

static void emit_node(struct impl *this, struct spa_bt_transport *t,
		uint32_t id, const char *factory_name, bool a2dp_duplex)
{
	struct spa_bt_device *device = this->bt_dev;
	struct spa_device_object_info info;
	struct spa_dict_item items[11];
	uint32_t n_items = 0;
	char transport[32], str_id[32], object_path[512];
	bool is_dyn_node = SPA_FLAG_IS_SET(id, DYNAMIC_NODE_ID_FLAG);

	snprintf(transport, sizeof(transport), "pointer:%p", t);
	items[0] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_TRANSPORT, transport);
	items[1] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_PROFILE, spa_bt_profile_name(t->profile));
	items[2] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_CODEC, get_codec_name(t, a2dp_duplex));
	items[3] = SPA_DICT_ITEM_INIT(SPA_KEY_API_BLUEZ5_ADDRESS, device->address);
	items[4] = SPA_DICT_ITEM_INIT("device.routes", "1");
	n_items = 5;
	if (!is_dyn_node && !this->device_set.path) {
		snprintf(str_id, sizeof(str_id), "%d", id);
		items[n_items] = SPA_DICT_ITEM_INIT("card.profile.device", str_id);
		n_items++;
	}
	if (spa_streq(spa_bt_profile_name(t->profile), "headset-head-unit")) {
		items[n_items] = SPA_DICT_ITEM_INIT("device.intended-roles", "Communication");
		n_items++;
	}
	if (a2dp_duplex) {
		items[n_items] = SPA_DICT_ITEM_INIT("api.bluez5.a2dp-duplex", "true");
		n_items++;
	}
	if (this->device_set.path) {
		items[n_items] = SPA_DICT_ITEM_INIT("api.bluez5.set", this->device_set.path);
		n_items++;
		items[n_items] = SPA_DICT_ITEM_INIT("api.bluez5.internal", "true");
		n_items++;

		/* object.path can be used in match rules with only basic node props */
		spa_scnprintf(object_path, sizeof(object_path), "%s/%s-%d",
				this->device_set.path, device->address, id);
		items[n_items] = SPA_DICT_ITEM_INIT("object.path", object_path);
		n_items++;
	}

	info = SPA_DEVICE_OBJECT_INFO_INIT();
	info.type = SPA_TYPE_INTERFACE_Node;
	info.factory_name = factory_name;
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.props = &SPA_DICT_INIT(items, n_items);

	SPA_FLAG_CLEAR(id, DYNAMIC_NODE_ID_FLAG);
	spa_device_emit_object_info(&this->hooks, id, &info);

	if (this->device_set.path) {
		/* Device set member nodes don't have their own routes */
		this->nodes[id].impl = this;
		this->nodes[id].active = false;
		if (this->nodes[id].transport)
			spa_hook_remove(&this->nodes[id].transport_listener);
		this->nodes[id].transport = NULL;
		return;
	}

	if (!is_dyn_node) {
		uint32_t prev_channels = this->nodes[id].n_channels;
		float boost;

		this->nodes[id].impl = this;
		this->nodes[id].active = true;
		this->nodes[id].offload_acquired = false;
		this->nodes[id].a2dp_duplex = a2dp_duplex;
		get_channels(t, a2dp_duplex, &this->nodes[id].n_channels, this->nodes[id].channels);
		if (this->nodes[id].transport)
			spa_hook_remove(&this->nodes[id].transport_listener);
		this->nodes[id].transport = t;
		spa_bt_transport_add_listener(t, &this->nodes[id].transport_listener, &transport_events, &this->nodes[id]);

		if (prev_channels > 0) {
			size_t i;
			/*
			 * Spread mono volume to all channels, if we had switched HFP -> A2DP.
			 * XXX: we should also use different route for hfp and a2dp
			 */
			for (i = prev_channels; i < this->nodes[id].n_channels; ++i)
				this->nodes[id].volumes[i] = this->nodes[id].volumes[i % prev_channels];
		}

		node_update_volume_from_transport(&this->nodes[id], true);

		boost = get_soft_volume_boost(&this->nodes[id]);
		if (boost != 1.0f) {
			size_t i;
			for (i = 0; i < this->nodes[id].n_channels; ++i)
				this->nodes[id].soft_volumes[i] = this->nodes[id].volumes[i] * boost;
		}

		emit_node_props(this, &this->nodes[id], true);
	}
}

static struct spa_bt_transport *find_device_transport(struct spa_bt_device *device, int profile,
		enum spa_bluetooth_audio_codec codec)
{
	struct spa_bt_transport *t;

	spa_list_for_each(t, &device->transport_list, device_link) {
		bool codec_ok = codec == 0 ||
			(t->media_codec != NULL && t->media_codec->id == codec) ||
			get_hfp_codec_id(t->codec) == codec;

		if ((t->profile & device->connected_profiles) &&
				(t->profile & profile) == t->profile &&
				codec_ok)
			return t;
	}

	return NULL;
}

static struct spa_bt_transport *find_transport(struct impl *this, int profile, enum spa_bluetooth_audio_codec codec)
{
	return find_device_transport(this->bt_dev, profile, codec);
}

static void dynamic_node_transport_destroy(void *data)
{
	struct dynamic_node *this = data;
	spa_log_debug(this->impl->log, "transport %p destroy", this->transport);
	this->transport = NULL;
}

static void dynamic_node_transport_state_changed(void *data,
	enum spa_bt_transport_state old,
	enum spa_bt_transport_state state)
{
	struct dynamic_node *this = data;
	struct impl *impl = this->impl;
	struct spa_bt_transport *t = this->transport;

	spa_log_debug(impl->log, "transport %p state %d->%d", t, old, state);

	if (state >= SPA_BT_TRANSPORT_STATE_PENDING && old < SPA_BT_TRANSPORT_STATE_PENDING) {
		if (!SPA_FLAG_IS_SET(this->id, DYNAMIC_NODE_ID_FLAG)) {
			SPA_FLAG_SET(this->id, DYNAMIC_NODE_ID_FLAG);
			spa_bt_transport_keepalive(t, true);
			emit_node(impl, t, this->id, this->factory_name, this->a2dp_duplex);
		}
	} else if (state < SPA_BT_TRANSPORT_STATE_PENDING && old >= SPA_BT_TRANSPORT_STATE_PENDING) {
		if (SPA_FLAG_IS_SET(this->id, DYNAMIC_NODE_ID_FLAG)) {
			SPA_FLAG_CLEAR(this->id, DYNAMIC_NODE_ID_FLAG);
			spa_bt_transport_keepalive(t, false);
			spa_device_emit_object_info(&impl->hooks, this->id, NULL);
		}
	}
}

static void dynamic_node_volume_changed(void *data)
{
	struct dynamic_node *node = data;
	struct impl *impl = node->impl;
	struct spa_event *event;
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	struct spa_pod_frame f[1];
	struct spa_bt_transport_volume *t_volume;
	int id = node->id, volume_id;

	SPA_FLAG_CLEAR(id, DYNAMIC_NODE_ID_FLAG);

	/* Remote device is the controller */
	if (!node->transport || impl->profile != DEVICE_PROFILE_AG
	    || !spa_bt_transport_volume_enabled(node->transport))
		return;

	if (id == 0 || id == 2)
		volume_id = SPA_BT_VOLUME_ID_RX;
	else if (id == 1)
		volume_id = SPA_BT_VOLUME_ID_TX;
	else
		return;

	t_volume = &node->transport->volumes[volume_id];
	if (!t_volume->active)
		return;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_EVENT_Device, SPA_DEVICE_EVENT_ObjectConfig);
	spa_pod_builder_prop(&b, SPA_EVENT_DEVICE_Object, 0);
	spa_pod_builder_int(&b, id);
	spa_pod_builder_prop(&b, SPA_EVENT_DEVICE_Props, 0);
	spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Props, SPA_EVENT_DEVICE_Props,
			SPA_PROP_volume, SPA_POD_Float(t_volume->volume));
	event = spa_pod_builder_pop(&b, &f[0]);

	spa_log_debug(impl->log, "dynamic node %d: volume %d changed %f, profile %d",
			node->id, volume_id, t_volume->volume, node->transport->profile);

	/* Dynamic node doesn't has route, we can only set volume on adaptar node. */
	spa_device_emit_event(&impl->hooks, event);
}

static const struct spa_bt_transport_events dynamic_node_transport_events = {
	SPA_VERSION_BT_TRANSPORT_EVENTS,
	.destroy = dynamic_node_transport_destroy,
	.state_changed = dynamic_node_transport_state_changed,
	.volume_changed = dynamic_node_volume_changed,
};

static void emit_dynamic_node(struct dynamic_node *this, struct impl *impl,
	struct spa_bt_transport *t, uint32_t id, const char *factory_name, bool a2dp_duplex)
{
	spa_log_debug(impl->log, "dynamic node, transport: %p->%p id: %08x->%08x",
		this->transport, t, this->id, id);

	if (this->transport) {
		/* Session manager don't really handles transport ptr changing. */
		spa_assert(this->transport == t);
		spa_hook_remove(&this->transport_listener);
	}

	this->impl = impl;
	this->transport = t;
	this->id = id;
	this->factory_name = factory_name;
	this->a2dp_duplex = a2dp_duplex;

	spa_bt_transport_add_listener(this->transport,
		&this->transport_listener, &dynamic_node_transport_events, this);

	/* emits the node if the state is already pending */
	dynamic_node_transport_state_changed (this, SPA_BT_TRANSPORT_STATE_IDLE, t->state);
}

static void remove_dynamic_node(struct dynamic_node *this)
{
	if (this->transport == NULL)
		return;

	/* destroy the node, if it exists */
	dynamic_node_transport_state_changed (this, this->transport->state,
		SPA_BT_TRANSPORT_STATE_IDLE);

	spa_hook_remove(&this->transport_listener);
	this->impl = NULL;
	this->transport = NULL;
	this->id = 0;
	this->factory_name = NULL;
}

static void device_set_clear(struct impl *impl)
{
	struct device_set *set = &impl->device_set;
	unsigned int i;

	for (i = 0; i < SPA_N_ELEMENTS(set->sink); ++i) {
		if (set->sink[i].transport)
			spa_hook_remove(&set->sink[i].listener);
		if (set->source[i].transport)
			spa_hook_remove(&set->source[i].listener);
	}

	free(set->path);
	spa_zero(*set);
	set->impl = impl;
}

static void device_set_transport_destroy(void *data)
{
	struct device_set_member *member = data;

	member->transport = NULL;
	spa_hook_remove(&member->listener);
}

static const struct spa_bt_transport_events device_set_transport_events = {
	SPA_VERSION_BT_DEVICE_EVENTS,
	.destroy = device_set_transport_destroy,
};

static void device_set_update(struct impl *this)
{
	struct spa_bt_device *device = this->bt_dev;
	struct device_set *dset = &this->device_set;
	struct spa_bt_set_membership *set;

	device_set_clear(this);

	if (!is_bap_client(this))
		return;

	spa_list_for_each(set, &device->set_membership_list, link) {
		struct spa_bt_set_membership *s;
		int num_devices = 0;

		spa_bt_for_each_set_member(s, set) {
			struct spa_bt_transport *t;
			bool active = false;

			if (!(s->device->connected_profiles & SPA_BT_PROFILE_BAP_DUPLEX))
				continue;

			t = find_device_transport(s->device, SPA_BT_PROFILE_BAP_SOURCE, 0);
			if (t && t->bap_initiator) {
				active = true;
				dset->source[dset->sources].transport = t;
				spa_bt_transport_add_listener(t, &dset->source[dset->sources].listener,
						&device_set_transport_events, &dset->source[dset->sources]);
				++dset->sources;
			}

			t = find_device_transport(s->device, SPA_BT_PROFILE_BAP_SINK, this->props.codec);
			if (t && t->bap_initiator) {
				active = true;
				dset->sink[dset->sinks].transport = t;
				spa_bt_transport_add_listener(t, &dset->sink[dset->sinks].listener,
						&device_set_transport_events, &dset->sink[dset->sinks]);
				++dset->sinks;
			}

			if (active)
				++num_devices;
		}

		if (num_devices <= 1 || (dset->sinks <= 1 && dset->sources <= 1)) {
			device_set_clear(this);
			continue;
		}

		dset->path = strdup(set->path);
		dset->leader = set->leader;
		break;
	}
}

static int emit_nodes(struct impl *this)
{
	struct spa_bt_transport *t;

	device_set_update(this);

	switch (this->profile) {
	case DEVICE_PROFILE_OFF:
		break;
	case DEVICE_PROFILE_AG:
		if (this->bt_dev->connected_profiles & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY) {
			t = find_transport(this, SPA_BT_PROFILE_HFP_AG, 0);
			if (!t)
				t = find_transport(this, SPA_BT_PROFILE_HSP_AG, 0);
			if (t) {
				if (t->profile == SPA_BT_PROFILE_HSP_AG)
					this->props.codec = 0;
				else
					this->props.codec = get_hfp_codec_id(t->codec);
				emit_dynamic_node(&this->dyn_sco_source, this, t,
						0, SPA_NAME_API_BLUEZ5_SCO_SOURCE, false);
				emit_dynamic_node(&this->dyn_sco_sink, this, t,
						1, SPA_NAME_API_BLUEZ5_SCO_SINK, false);
			}
		}
		if (this->bt_dev->connected_profiles & (SPA_BT_PROFILE_A2DP_SOURCE)) {
			t = find_transport(this, SPA_BT_PROFILE_A2DP_SOURCE, 0);
			if (t) {
				this->props.codec = t->media_codec->id;
				emit_dynamic_node(&this->dyn_media_source, this, t,
						2, SPA_NAME_API_BLUEZ5_A2DP_SOURCE, false);

				if (t->media_codec->duplex_codec) {
					emit_dynamic_node(&this->dyn_media_sink, this, t,
						3, SPA_NAME_API_BLUEZ5_A2DP_SINK, true);
				}
			}
		}
		break;
	case DEVICE_PROFILE_A2DP:
		if (this->bt_dev->connected_profiles & SPA_BT_PROFILE_A2DP_SOURCE) {
			t = find_transport(this, SPA_BT_PROFILE_A2DP_SOURCE, 0);
			if (t) {
				this->props.codec = t->media_codec->id;
				emit_dynamic_node(&this->dyn_media_source, this, t,
					DEVICE_ID_SOURCE, SPA_NAME_API_BLUEZ5_A2DP_SOURCE, false);

				if (t->media_codec->duplex_codec) {
					emit_node(this, t,
						DEVICE_ID_SINK, SPA_NAME_API_BLUEZ5_A2DP_SINK, true);
				}
			}
		}

		if (this->bt_dev->connected_profiles & SPA_BT_PROFILE_A2DP_SINK) {
			t = find_transport(this, SPA_BT_PROFILE_A2DP_SINK, this->props.codec);
			if (t) {
				this->props.codec = t->media_codec->id;
				emit_node(this, t, DEVICE_ID_SINK, SPA_NAME_API_BLUEZ5_A2DP_SINK, false);

				if (t->media_codec->duplex_codec) {
					emit_node(this, t,
						DEVICE_ID_SOURCE, SPA_NAME_API_BLUEZ5_A2DP_SOURCE, true);
				}
			}
		}

		if (get_supported_media_codec(this, this->props.codec, NULL) == NULL)
			this->props.codec = 0;
		break;
	case DEVICE_PROFILE_BAP:
		if (this->bt_dev->connected_profiles & (SPA_BT_PROFILE_BAP_SOURCE)) {
			t = find_transport(this, SPA_BT_PROFILE_BAP_SOURCE, 0);
			if (t) {
				this->props.codec = t->media_codec->id;
				if (t->bap_initiator)
					emit_node(this, t, DEVICE_ID_SOURCE, SPA_NAME_API_BLUEZ5_MEDIA_SOURCE, false);
				else
					emit_dynamic_node(&this->dyn_media_source, this, t,
						DEVICE_ID_SOURCE, SPA_NAME_API_BLUEZ5_MEDIA_SOURCE, false);
			}

			if (this->device_set.leader && this->device_set.sources > 0)
				emit_device_set_node(this, DEVICE_ID_SOURCE_SET);
		}

		if (this->bt_dev->connected_profiles & (SPA_BT_PROFILE_BAP_SINK)) {
			t = find_transport(this, SPA_BT_PROFILE_BAP_SINK, this->props.codec);
			if (t) {
				this->props.codec = t->media_codec->id;
				if (t->bap_initiator)
					emit_node(this, t, DEVICE_ID_SINK, SPA_NAME_API_BLUEZ5_MEDIA_SINK, false);
				else
					emit_dynamic_node(&this->dyn_media_sink, this, t,
						DEVICE_ID_SINK, SPA_NAME_API_BLUEZ5_MEDIA_SINK, false);
			}

			if (this->device_set.leader && this->device_set.sinks > 0)
				emit_device_set_node(this, DEVICE_ID_SINK_SET);
		}

		if (this->bt_dev->connected_profiles & (SPA_BT_PROFILE_BAP_BROADCAST_SINK)) {
			t = find_transport(this, SPA_BT_PROFILE_BAP_BROADCAST_SINK, this->props.codec);
			if (t) {
				this->props.codec = t->media_codec->id;
				emit_node(this, t, DEVICE_ID_SINK, SPA_NAME_API_BLUEZ5_MEDIA_SINK, false);
			}

			if (this->device_set.leader && this->device_set.sinks > 0)
				emit_device_set_node(this, DEVICE_ID_SINK_SET);
		}

		if (this->bt_dev->connected_profiles & (SPA_BT_PROFILE_BAP_BROADCAST_SOURCE)) {
			t = find_transport(this, SPA_BT_PROFILE_BAP_BROADCAST_SOURCE, this->props.codec);
			if (t) {
				this->props.codec = t->media_codec->id;
				emit_dynamic_node(&this->dyn_media_source, this, t,
					DEVICE_ID_SOURCE, SPA_NAME_API_BLUEZ5_MEDIA_SOURCE, false);
			}
		}

		if (get_supported_media_codec(this, this->props.codec, NULL) == NULL)
			this->props.codec = 0;
		break;
	case DEVICE_PROFILE_HSP_HFP:
		if (this->bt_dev->connected_profiles & SPA_BT_PROFILE_HEADSET_HEAD_UNIT) {
			t = find_transport(this, SPA_BT_PROFILE_HFP_HF, this->props.codec);
			if (!t)
				t = find_transport(this, SPA_BT_PROFILE_HSP_HS, 0);
			if (t) {
				if (t->profile == SPA_BT_PROFILE_HSP_HS)
					this->props.codec = 0;
				else
					this->props.codec = get_hfp_codec_id(t->codec);
				emit_node(this, t, DEVICE_ID_SOURCE, SPA_NAME_API_BLUEZ5_SCO_SOURCE, false);
				emit_node(this, t, DEVICE_ID_SINK, SPA_NAME_API_BLUEZ5_SCO_SINK, false);
			}
		}

		if (spa_bt_device_supports_hfp_codec(this->bt_dev, get_hfp_codec(this->props.codec)) != 1)
			this->props.codec = 0;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static const struct spa_dict_item info_items[] = {
	{ SPA_KEY_DEVICE_API, "bluez5" },
	{ SPA_KEY_DEVICE_BUS, "bluetooth" },
	{ SPA_KEY_MEDIA_CLASS, "Audio/Device" },
};

static void emit_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;
	if (full)
		this->info.change_mask = this->info_all;
	if (this->info.change_mask) {
		this->info.props = &SPA_DICT_INIT_ARRAY(info_items);

		spa_device_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
}

static void emit_remove_nodes(struct impl *this)
{
	remove_dynamic_node (&this->dyn_media_source);
	remove_dynamic_node (&this->dyn_media_sink);
	remove_dynamic_node (&this->dyn_sco_source);
	remove_dynamic_node (&this->dyn_sco_sink);

	for (uint32_t i = 0; i < SPA_N_ELEMENTS(this->nodes); i++) {
		struct node * node = &this->nodes[i];
		node_offload_set_active(node, false);
		if (node->transport) {
			spa_hook_remove(&node->transport_listener);
			node->transport = NULL;
		}
		if (node->active) {
			spa_device_emit_object_info(&this->hooks, i, NULL);
			node->active = false;
		}
	}

	this->props.offload_active = false;
}

static bool validate_profile(struct impl *this, uint32_t profile,
		enum spa_bluetooth_audio_codec codec);

static int set_profile(struct impl *this, uint32_t profile, enum spa_bluetooth_audio_codec codec, bool save)
{
	if (!validate_profile(this, profile, codec)) {
		spa_log_warn(this->log, "trying to set invalid profile %d, codec %d, %08x %08x",
			    profile, codec,
			    this->bt_dev->profiles, this->bt_dev->connected_profiles);
		return -EINVAL;
	}

	this->save_profile = save;

	if (this->profile == profile &&
	    (this->profile != DEVICE_PROFILE_A2DP || codec == this->props.codec) &&
	    (this->profile != DEVICE_PROFILE_BAP || codec == this->props.codec || this->device_set.path) &&
	    (this->profile != DEVICE_PROFILE_HSP_HFP || codec == this->props.codec))
		return 0;

	emit_remove_nodes(this);

	spa_bt_device_release_transports(this->bt_dev);

	this->profile = profile;
	this->prev_bt_connected_profiles = this->bt_dev->connected_profiles;
	this->props.codec = codec;

	/*
	 * A2DP/BAP: ensure there's a transport with the selected codec (0 means any).
	 * Don't try to switch codecs when the device is in the A2DP source role, since
	 * devices do not appear to like that.
	 *
	 * For BAP, only BAP client can configure the codec.
	 *
	 * XXX: codec switching also currently does not work in the duplex or
	 * XXX: source-only case, as it will only switch the sink, and we only
	 * XXX: list the sink codecs here. TODO: fix this
	 */
	if ((profile == DEVICE_PROFILE_A2DP || (profile == DEVICE_PROFILE_BAP && can_bap_codec_switch(this)))
			&& !(this->bt_dev->connected_profiles & SPA_BT_PROFILE_A2DP_SOURCE)) {
		int ret;
		const struct media_codec *codecs[64];

		get_media_codecs(this, codec, codecs, SPA_N_ELEMENTS(codecs));

		this->switching_codec = true;

		ret = spa_bt_device_ensure_media_codec(this->bt_dev, codecs);
		if (ret < 0) {
			if (ret != -ENOTSUP)
				spa_log_error(this->log, "failed to switch codec (%d), setting basic profile", ret);
		} else {
			return 0;
		}
	} else if (profile == DEVICE_PROFILE_HSP_HFP && get_hfp_codec(codec) && !(this->bt_dev->connected_profiles & SPA_BT_PROFILE_HFP_AG)) {
		int ret;

		this->switching_codec = true;

		ret = spa_bt_device_ensure_hfp_codec(this->bt_dev, get_hfp_codec(codec));
		if (ret < 0) {
			if (ret != -ENOTSUP)
				spa_log_error(this->log, "failed to switch codec (%d), setting basic profile", ret);
		} else {
			return 0;
		}
	}

	this->switching_codec = false;
	this->props.codec = 0;
	emit_nodes(this);

	this->info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
	this->params[IDX_Profile].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_Route].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_EnumRoute].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_Props].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_PropInfo].flags ^= SPA_PARAM_INFO_SERIAL;
	emit_info(this, false);

	return 0;
}

static void codec_switched(void *userdata, int status)
{
	struct impl *this = userdata;

	spa_log_debug(this->log, "codec switched (status %d)", status);

	this->switching_codec = false;

	if (status < 0) {
		/* Failed to switch: return to a fallback profile */
		spa_log_error(this->log, "failed to switch codec (%d), setting fallback profile", status);
		if (this->profile == DEVICE_PROFILE_A2DP && this->props.codec != 0) {
			this->props.codec = 0;
		} else if (this->profile == DEVICE_PROFILE_BAP && this->props.codec != 0) {
			this->props.codec = 0;
		} else if (this->profile == DEVICE_PROFILE_HSP_HFP && this->props.codec != 0) {
			this->props.codec = 0;
		} else {
			this->profile = DEVICE_PROFILE_OFF;
		}
	}

	emit_remove_nodes(this);
	emit_nodes(this);

	this->info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
	if (this->prev_bt_connected_profiles != this->bt_dev->connected_profiles)
		this->params[IDX_EnumProfile].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_Profile].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_Route].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_EnumRoute].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_Props].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_PropInfo].flags ^= SPA_PARAM_INFO_SERIAL;
	emit_info(this, false);
}

static void profiles_changed(void *userdata, uint32_t prev_profiles, uint32_t prev_connected_profiles)
{
	struct impl *this = userdata;
	uint32_t connected_change;
	bool nodes_changed = false;

	connected_change = (this->bt_dev->connected_profiles ^ prev_connected_profiles);

	/* Profiles changed. We have to re-emit device information. */
	spa_log_info(this->log, "profiles changed to  %08x %08x (prev %08x %08x, change %08x)"
		     " switching_codec:%d",
		     this->bt_dev->profiles, this->bt_dev->connected_profiles,
		     prev_profiles, prev_connected_profiles, connected_change,
		     this->switching_codec);

	if (this->switching_codec)
		return;

	if (this->bt_dev->connected_profiles & SPA_BT_PROFILE_MEDIA_SINK) {
		free(this->supported_codecs);
		this->supported_codecs = spa_bt_device_get_supported_media_codecs(
			this->bt_dev, &this->supported_codec_count, true);
	}

	switch (this->profile) {
	case DEVICE_PROFILE_OFF:
		/* Noop */
		nodes_changed = false;
		break;
	case DEVICE_PROFILE_AG:
		nodes_changed = (connected_change & (SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY |
						     SPA_BT_PROFILE_MEDIA_SOURCE));
		spa_log_debug(this->log, "profiles changed: AG nodes changed: %d",
			      nodes_changed);
		break;
	case DEVICE_PROFILE_A2DP:
	case DEVICE_PROFILE_BAP:
		if (get_supported_media_codec(this, this->props.codec, NULL) == NULL)
			this->props.codec = 0;
		nodes_changed = (connected_change & (SPA_BT_PROFILE_MEDIA_SINK |
						     SPA_BT_PROFILE_MEDIA_SOURCE));
		spa_log_debug(this->log, "profiles changed: media nodes changed: %d",
			      nodes_changed);
		break;
	case DEVICE_PROFILE_HSP_HFP:
		if (spa_bt_device_supports_hfp_codec(this->bt_dev, get_hfp_codec(this->props.codec)) != 1)
			this->props.codec = 0;
		nodes_changed = (connected_change & SPA_BT_PROFILE_HEADSET_HEAD_UNIT);
		spa_log_debug(this->log, "profiles changed: HSP/HFP nodes changed: %d",
			      nodes_changed);
		break;
	}

	if (nodes_changed) {
		emit_remove_nodes(this);
		emit_nodes(this);
	}

	this->info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
	this->params[IDX_Profile].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_EnumProfile].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_Route].flags ^= SPA_PARAM_INFO_SERIAL;  /* Profile changes may affect routes */
	this->params[IDX_EnumRoute].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_Props].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_PropInfo].flags ^= SPA_PARAM_INFO_SERIAL;
	emit_info(this, false);
}

static void device_set_changed(void *userdata)
{
	struct impl *this = userdata;

	if (this->profile != DEVICE_PROFILE_BAP)
		return;
	if (!is_bap_client(this))
		return;

	spa_log_debug(this->log, "%p: device set changed", this);

	emit_remove_nodes(this);
	emit_nodes(this);

	this->info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
	this->params[IDX_Profile].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_EnumProfile].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_Route].flags ^= SPA_PARAM_INFO_SERIAL;
	this->params[IDX_EnumRoute].flags ^= SPA_PARAM_INFO_SERIAL;
	emit_info(this, false);
}

static void set_initial_profile(struct impl *this);

static void device_connected(void *userdata, bool connected)
{
	struct impl *this = userdata;

	spa_log_debug(this->log, "%p: connected: %d", this, connected);

	if (connected ^ (this->profile != DEVICE_PROFILE_OFF)) {
		emit_remove_nodes(this);
		set_initial_profile(this);
	}
}

static const struct spa_bt_device_events bt_dev_events = {
	SPA_VERSION_BT_DEVICE_EVENTS,
	.connected = device_connected,
	.codec_switched = codec_switched,
	.profiles_changed = profiles_changed,
	.device_set_changed = device_set_changed,
};

static int impl_add_listener(void *object,
			struct spa_hook *listener,
			const struct spa_device_events *events,
			void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	if (events->info)
		emit_info(this, true);

	if (events->object_info)
		emit_nodes(this);

	spa_hook_list_join(&this->hooks, &save);

	return 0;
}

static int impl_sync(void *object, int seq)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	spa_device_emit_result(&this->hooks, seq, 0, 0, NULL);

	return 0;
}

static uint32_t profile_direction_mask(struct impl *this, uint32_t index, enum spa_bluetooth_audio_codec codec)
{
	struct spa_bt_device *device = this->bt_dev;
	uint32_t mask;
	bool have_output = false, have_input = false;
	const struct media_codec *media_codec;

	switch (index) {
	case DEVICE_PROFILE_A2DP:
		if (device->connected_profiles & SPA_BT_PROFILE_A2DP_SINK)
			have_output = true;

		media_codec = get_supported_media_codec(this, codec, NULL);
		if (media_codec && media_codec->duplex_codec)
			have_input = true;
		break;
	case DEVICE_PROFILE_BAP:
		if (device->connected_profiles & SPA_BT_PROFILE_BAP_SINK)
			have_output = true;
		if (device->connected_profiles & SPA_BT_PROFILE_BAP_SOURCE)
			have_input = true;
		break;
	case DEVICE_PROFILE_HSP_HFP:
		if (device->connected_profiles & SPA_BT_PROFILE_HEADSET_HEAD_UNIT)
			have_output = have_input = true;
		break;
	default:
		break;
	}

	mask = 0;
	if (have_output)
		mask |= 1 << SPA_DIRECTION_OUTPUT;
	if (have_input)
		mask |= 1 << SPA_DIRECTION_INPUT;
	return mask;
}

static uint32_t get_profile_from_index(struct impl *this, uint32_t index, uint32_t *next, enum spa_bluetooth_audio_codec *codec)
{
	/*
	 * XXX: The codecs should probably become a separate param, and not have
	 * XXX: separate profiles for each one.
	 */

	*codec = 0;
	*next = index + 1;

	if (index <= DEVICE_PROFILE_LAST) {
		return index;
	} else if (index != SPA_ID_INVALID) {
		const struct spa_type_info *info;
		uint32_t profile;

		*codec = index - DEVICE_PROFILE_LAST;
		*next = SPA_ID_INVALID;

		for (info = spa_type_bluetooth_audio_codec; info->type; ++info)
			if (info->type > *codec)
				*next = SPA_MIN(info->type + DEVICE_PROFILE_LAST, *next);

		if (get_hfp_codec(*codec))
			profile = DEVICE_PROFILE_HSP_HFP;
		else if (*codec == SPA_BLUETOOTH_AUDIO_CODEC_LC3)
			profile = DEVICE_PROFILE_BAP;
		else
			profile = DEVICE_PROFILE_A2DP;

		return profile;
	}

	*next = SPA_ID_INVALID;
	return SPA_ID_INVALID;
}

static uint32_t get_index_from_profile(struct impl *this, uint32_t profile, enum spa_bluetooth_audio_codec codec)
{
	if (profile == DEVICE_PROFILE_OFF || profile == DEVICE_PROFILE_AG)
		return profile;

	if ((profile == DEVICE_PROFILE_A2DP) || (profile == DEVICE_PROFILE_BAP))
		return codec + DEVICE_PROFILE_LAST;

	if (profile == DEVICE_PROFILE_HSP_HFP) {
		if (codec == 0 || (this->bt_dev->connected_profiles & SPA_BT_PROFILE_HFP_AG))
			return profile;

		return codec + DEVICE_PROFILE_LAST;
	}

	return SPA_ID_INVALID;
}

static bool set_initial_hsp_hfp_profile(struct impl *this)
{
	struct spa_bt_transport *t;
	int i;

	for (i = SPA_BT_PROFILE_HSP_HS; i <= SPA_BT_PROFILE_HFP_AG; i <<= 1) {
		if (!(this->bt_dev->connected_profiles & i))
			continue;

		t = find_transport(this, i, 0);
		if (t) {
			this->profile = (i & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY) ?
				DEVICE_PROFILE_AG : DEVICE_PROFILE_HSP_HFP;
			this->props.codec = get_hfp_codec_id(t->codec);

			spa_log_debug(this->log, "initial profile HSP/HFP profile:%d codec:%d",
					this->profile, this->props.codec);
			return true;
		}
	}
	return false;
}

static void set_initial_profile(struct impl *this)
{
	struct spa_bt_transport *t;
	int i;

	this->switching_codec = false;

	if (this->supported_codecs)
		free(this->supported_codecs);
	this->supported_codecs = spa_bt_device_get_supported_media_codecs(
		this->bt_dev, &this->supported_codec_count, true);

	/* Prefer BAP, then A2DP, then HFP, then null, but select AG if the device
	   appears not to have BAP_SINK, A2DP_SINK or any HEAD_UNIT profile */

	/* If default profile is set to HSP/HFP, first try those and exit if found. */
	if (this->bt_dev->settings != NULL) {
		const char *str = spa_dict_lookup(this->bt_dev->settings, "bluez5.profile");
		if (spa_streq(str, "off"))
			goto off;
		if (spa_streq(str, "headset-head-unit") && set_initial_hsp_hfp_profile(this))
			return;
	}

	for (i = SPA_BT_PROFILE_BAP_SINK; i <= SPA_BT_PROFILE_A2DP_SOURCE; i <<= 1) {
		if (!(this->bt_dev->connected_profiles & i))
			continue;

		t = find_transport(this, i, 0);
		if (t) {
			if (i == SPA_BT_PROFILE_A2DP_SOURCE || i == SPA_BT_PROFILE_BAP_SOURCE)
				this->profile =  DEVICE_PROFILE_AG;
			else if (i == SPA_BT_PROFILE_BAP_SINK)
				this->profile =  DEVICE_PROFILE_BAP;
			else
				this->profile =  DEVICE_PROFILE_A2DP;
			this->props.codec = t->media_codec->id;
			spa_log_debug(this->log, "initial profile media profile:%d codec:%d",
					this->profile, this->props.codec);
			return;
		}
	}

	if (set_initial_hsp_hfp_profile(this))
		return;

off:
	spa_log_debug(this->log, "initial profile off");

	this->profile = DEVICE_PROFILE_OFF;
	this->props.codec = 0;
}

static struct spa_pod *build_profile(struct impl *this, struct spa_pod_builder *b,
		uint32_t id, uint32_t index, uint32_t profile_index, enum spa_bluetooth_audio_codec codec,
		bool current)
{
	struct spa_bt_device *device = this->bt_dev;
	struct spa_pod_frame f[2];
	const char *name, *desc;
	char *name_and_codec = NULL;
	char *desc_and_codec = NULL;
	uint32_t n_source = 0, n_sink = 0;
	uint32_t capture[1] = { DEVICE_ID_SOURCE }, playback[1] = { DEVICE_ID_SINK };
	int priority;

	switch (profile_index) {
	case DEVICE_PROFILE_OFF:
		name = "off";
		desc = _("Off");
		priority = 0;
		break;
	case DEVICE_PROFILE_AG:
	{
		uint32_t profile = device->connected_profiles &
		      (SPA_BT_PROFILE_A2DP_SOURCE | SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY);
		if (profile == 0) {
			return NULL;
		} else {
			name = "audio-gateway";
			desc = _("Audio Gateway (A2DP Source & HSP/HFP AG)");
		}
		priority = 256;
		break;
	}
	case DEVICE_PROFILE_A2DP:
	{
		/* make this device profile visible only if there is an A2DP sink */
		uint32_t profile = device->connected_profiles &
		      (SPA_BT_PROFILE_A2DP_SINK | SPA_BT_PROFILE_A2DP_SOURCE);
		if (!(profile & SPA_BT_PROFILE_A2DP_SINK)) {
			return NULL;
		}

		/* A2DP will only enlist codec profiles */
		if (!codec)
			return NULL;

		name = spa_bt_profile_name(profile);
		n_sink++;
		if (codec) {
			size_t idx;
			const struct media_codec *media_codec = get_supported_media_codec(this, codec, &idx);
			if (media_codec == NULL) {
				errno = EINVAL;
				return NULL;
			}
			name_and_codec = spa_aprintf("%s-%s", name, media_codec->name);

			/*
			 * Give base name to highest priority profile, so that best codec can be
			 * selected at command line with out knowing which codecs are actually
			 * supported
			 */
			if (idx != 0)
				name = name_and_codec;

			if (profile == SPA_BT_PROFILE_A2DP_SINK && !media_codec->duplex_codec) {
				desc_and_codec = spa_aprintf(_("High Fidelity Playback (A2DP Sink, codec %s)"),
							     media_codec->description);
			} else {
				desc_and_codec = spa_aprintf(_("High Fidelity Duplex (A2DP Source/Sink, codec %s)"),
							     media_codec->description);

			}
			desc = desc_and_codec;
			priority = 16 + this->supported_codec_count - idx;  /* order as in codec list */
		} else {
			if (profile == SPA_BT_PROFILE_A2DP_SINK) {
				desc = _("High Fidelity Playback (A2DP Sink)");
			} else {
				desc = _("High Fidelity Duplex (A2DP Source/Sink)");
			}
			priority = 16;
		}
		break;
	}
	case DEVICE_PROFILE_BAP:
	{
		uint32_t profile = device->connected_profiles &
		      (SPA_BT_PROFILE_BAP_SINK | SPA_BT_PROFILE_BAP_SOURCE 
			  	| SPA_BT_PROFILE_BAP_BROADCAST_SOURCE 
				| SPA_BT_PROFILE_BAP_BROADCAST_SINK);
		size_t idx;
		const struct media_codec *media_codec;

		/* BAP will only enlist codec profiles */
		if (codec == 0)
			return NULL;

		if (profile == 0)
			return NULL;

		if ((profile & (SPA_BT_PROFILE_BAP_SINK)) || 
			(profile & (SPA_BT_PROFILE_BAP_BROADCAST_SINK)))
			n_sink++;
		if ((profile & (SPA_BT_PROFILE_BAP_SOURCE)) || 
			(profile & (SPA_BT_PROFILE_BAP_BROADCAST_SOURCE)))
			n_source++;

		name = spa_bt_profile_name(profile);

		if (codec) {
			media_codec = get_supported_media_codec(this, codec, &idx);
			if (media_codec == NULL) {
				errno = EINVAL;
				return NULL;
			}
			name_and_codec = spa_aprintf("%s-%s", name, media_codec->name);

			/*
			 * Give base name to highest priority profile, so that best codec can be
			 * selected at command line with out knowing which codecs are actually
			 * supported
			 */
			if (idx != 0)
				name = name_and_codec;

			switch (profile) {
			case SPA_BT_PROFILE_BAP_SINK:
			case SPA_BT_PROFILE_BAP_BROADCAST_SINK:
				desc_and_codec = spa_aprintf(_("High Fidelity Playback (BAP Sink, codec %s)"),
						media_codec->description);
				break;
			case SPA_BT_PROFILE_BAP_SOURCE:
			case SPA_BT_PROFILE_BAP_BROADCAST_SOURCE:
				desc_and_codec = spa_aprintf(_("High Fidelity Input (BAP Source, codec %s)"),
						media_codec->description);
				break;
			default:
				desc_and_codec = spa_aprintf(_("High Fidelity Duplex (BAP Source/Sink, codec %s)"),
						media_codec->description);
			}
			desc = desc_and_codec;
			priority = 128 + this->supported_codec_count - idx;  /* order as in codec list */
		} else {
			switch (profile) {
			case SPA_BT_PROFILE_BAP_SINK:
			case SPA_BT_PROFILE_BAP_BROADCAST_SINK:
				desc = _("High Fidelity Playback (BAP Sink)");
				break;
			case SPA_BT_PROFILE_BAP_SOURCE:
			case SPA_BT_PROFILE_BAP_BROADCAST_SOURCE:
				desc = _("High Fidelity Input (BAP Source)");
				break;
			default:
				desc = _("High Fidelity Duplex (BAP Source/Sink)");
			}
			priority = 128;
		}

		if (this->device_set.leader) {
			n_sink = this->device_set.sinks ? 1 : 0;
			n_source = this->device_set.sinks ? 1 : 0;
		} else if (this->device_set.path) {
			n_sink = 0;
			n_source = 0;
		}
		break;
	}
	case DEVICE_PROFILE_HSP_HFP:
	{
		/* make this device profile visible only if there is a head unit */
		uint32_t profile = device->connected_profiles &
		      SPA_BT_PROFILE_HEADSET_HEAD_UNIT;
		if (profile == 0) {
			return NULL;
		}
		name = spa_bt_profile_name(profile);
		n_source++;
		n_sink++;
		if (codec) {
			bool codec_ok = !(profile & SPA_BT_PROFILE_HEADSET_AUDIO_GATEWAY);
			unsigned int hfp_codec = get_hfp_codec(codec);
			if (spa_bt_device_supports_hfp_codec(this->bt_dev, hfp_codec) != 1)
				codec_ok = false;
			if (!codec_ok) {
				errno = EINVAL;
				return NULL;
			}
			name_and_codec = spa_aprintf("%s-%s", name, get_hfp_codec_name(hfp_codec));
			name = name_and_codec;
			desc_and_codec = spa_aprintf(_("Headset Head Unit (HSP/HFP, codec %s)"),
						get_hfp_codec_description(hfp_codec));
			desc = desc_and_codec;
			priority = 1 + hfp_codec;  /* prefer msbc over cvsd */
		} else {
			desc = _("Headset Head Unit (HSP/HFP)");
			priority = 1;
		}
		break;
	}
	default:
		errno = EINVAL;
		return NULL;
	}

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_ParamProfile, id);
	spa_pod_builder_add(b,
		SPA_PARAM_PROFILE_index,   SPA_POD_Int(index),
		SPA_PARAM_PROFILE_name, SPA_POD_String(name),
		SPA_PARAM_PROFILE_description, SPA_POD_String(desc),
		SPA_PARAM_PROFILE_available, SPA_POD_Id(SPA_PARAM_AVAILABILITY_yes),
		SPA_PARAM_PROFILE_priority, SPA_POD_Int(priority),
		0);
	if (n_source > 0 || n_sink > 0) {
		spa_pod_builder_prop(b, SPA_PARAM_PROFILE_classes, 0);
		spa_pod_builder_push_struct(b, &f[1]);
		if (n_source > 0) {
			spa_pod_builder_add_struct(b,
				SPA_POD_String("Audio/Source"),
				SPA_POD_Int(n_source),
				SPA_POD_String("card.profile.devices"),
				SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Int, 1, capture));
		}
		if (n_sink > 0) {
			spa_pod_builder_add_struct(b,
				SPA_POD_String("Audio/Sink"),
				SPA_POD_Int(n_sink),
				SPA_POD_String("card.profile.devices"),
				SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Int, 1, playback));
		}
		spa_pod_builder_pop(b, &f[1]);
	}
	if (current) {
                spa_pod_builder_prop(b, SPA_PARAM_PROFILE_save, 0);
                spa_pod_builder_bool(b, this->save_profile);
	}

	if (name_and_codec)
		free(name_and_codec);
	if (desc_and_codec)
		free(desc_and_codec);

	return spa_pod_builder_pop(b, &f[0]);
}

static bool validate_profile(struct impl *this, uint32_t profile,
		enum spa_bluetooth_audio_codec codec)
{
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	return (build_profile(this, &b, 0, 0, profile, codec, false) != NULL);
}

static struct spa_pod *build_route(struct impl *this, struct spa_pod_builder *b,
		uint32_t id, uint32_t port, uint32_t profile)
{
	struct spa_bt_device *device = this->bt_dev;
	struct spa_pod_frame f[2];
	enum spa_direction direction;
	const char *name_prefix, *description, *hfp_description, *port_type;
	enum spa_bt_form_factor ff;
	enum spa_bluetooth_audio_codec codec;
	enum spa_param_availability available;
	char name[128];
	uint32_t i, j, mask, next;
	uint32_t dev = SPA_ID_INVALID, enum_dev;

	ff = spa_bt_form_factor_from_class(device->bluetooth_class);

	switch (ff) {
	case SPA_BT_FORM_FACTOR_HEADSET:
		name_prefix = "headset";
		description = _("Headset");
		hfp_description = _("Handsfree");
		port_type = "headset";
		break;
	case SPA_BT_FORM_FACTOR_HANDSFREE:
		name_prefix = "handsfree";
		description = _("Handsfree");
		hfp_description = _("Handsfree (HFP)");
		port_type = "handsfree";
		break;
	case SPA_BT_FORM_FACTOR_MICROPHONE:
		name_prefix = "microphone";
		description = _("Microphone");
		hfp_description = _("Handsfree");
		port_type = "mic";
		break;
	case SPA_BT_FORM_FACTOR_SPEAKER:
		name_prefix = "speaker";
		description = _("Speaker");
		hfp_description = _("Handsfree");
		port_type = "speaker";
		break;
	case SPA_BT_FORM_FACTOR_HEADPHONE:
		name_prefix = "headphone";
		description = _("Headphone");
		hfp_description = _("Handsfree");
		port_type = "headphones";
		break;
	case SPA_BT_FORM_FACTOR_PORTABLE:
		name_prefix = "portable";
		description = _("Portable");
		hfp_description = _("Handsfree");
		port_type = "portable";
		break;
	case SPA_BT_FORM_FACTOR_CAR:
		name_prefix = "car";
		description = _("Car");
		hfp_description = _("Handsfree");
		port_type = "car";
		break;
	case SPA_BT_FORM_FACTOR_HIFI:
		name_prefix = "hifi";
		description = _("HiFi");
		hfp_description = _("Handsfree");
		port_type = "hifi";
		break;
	case SPA_BT_FORM_FACTOR_PHONE:
		name_prefix = "phone";
		description = _("Phone");
		hfp_description = _("Handsfree");
		port_type = "phone";
		break;
	case SPA_BT_FORM_FACTOR_UNKNOWN:
	default:
		name_prefix = "bluetooth";
		description = _("Bluetooth");
		hfp_description = _("Bluetooth (HFP)");
		port_type = "bluetooth";
		break;
	}

	switch (port) {
	case 0:
		direction = SPA_DIRECTION_INPUT;
		snprintf(name, sizeof(name), "%s-input", name_prefix);
		enum_dev = DEVICE_ID_SOURCE;
		if (profile == DEVICE_PROFILE_A2DP || profile == DEVICE_PROFILE_BAP)
			dev = enum_dev;
		else if (profile != SPA_ID_INVALID)
			enum_dev = SPA_ID_INVALID;
		break;
	case 1:
		direction = SPA_DIRECTION_OUTPUT;
		snprintf(name, sizeof(name), "%s-output", name_prefix);
		enum_dev = DEVICE_ID_SINK;
		if (profile == DEVICE_PROFILE_A2DP || profile == DEVICE_PROFILE_BAP)
			dev = enum_dev;
		else if (profile != SPA_ID_INVALID)
			enum_dev = SPA_ID_INVALID;
		break;
	case 2:
		direction = SPA_DIRECTION_INPUT;
		snprintf(name, sizeof(name), "%s-hf-input", name_prefix);
		description = hfp_description;
		enum_dev = DEVICE_ID_SOURCE;
		if (profile == DEVICE_PROFILE_HSP_HFP)
			dev = enum_dev;
		else if (profile != SPA_ID_INVALID)
			enum_dev = SPA_ID_INVALID;
		break;
	case 3:
		direction = SPA_DIRECTION_OUTPUT;
		snprintf(name, sizeof(name), "%s-hf-output", name_prefix);
		description = hfp_description;
		enum_dev = DEVICE_ID_SINK;
		if (profile == DEVICE_PROFILE_HSP_HFP)
			dev = enum_dev;
		else if (profile != SPA_ID_INVALID)
			enum_dev = SPA_ID_INVALID;
		break;
	case 4:
		if (!this->device_set.leader) {
			errno = EINVAL;
			return NULL;
		}
		direction = SPA_DIRECTION_INPUT;
		snprintf(name, sizeof(name), "%s-set-input", name_prefix);
		enum_dev = DEVICE_ID_SOURCE_SET;
		if (profile == DEVICE_PROFILE_BAP)
			dev = enum_dev;
		else if (profile != SPA_ID_INVALID)
			enum_dev = SPA_ID_INVALID;
		break;
	case 5:
		if (!this->device_set.leader) {
			errno = EINVAL;
			return NULL;
		}
		direction = SPA_DIRECTION_OUTPUT;
		snprintf(name, sizeof(name), "%s-set-output", name_prefix);
		enum_dev = DEVICE_ID_SINK_SET;
		if (profile == DEVICE_PROFILE_BAP)
			dev = enum_dev;
		else if (profile != SPA_ID_INVALID)
			enum_dev = SPA_ID_INVALID;
		break;
	default:
		errno = EINVAL;
		return NULL;
	}

	if (enum_dev == SPA_ID_INVALID) {
		errno = EINVAL;
		return NULL;
	}

	available = SPA_PARAM_AVAILABILITY_yes;
	if (this->device_set.path && !(port == 4 || port == 5))
		available = SPA_PARAM_AVAILABILITY_no;

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_ParamRoute, id);
	spa_pod_builder_add(b,
		SPA_PARAM_ROUTE_index, SPA_POD_Int(port),
		SPA_PARAM_ROUTE_direction,  SPA_POD_Id(direction),
		SPA_PARAM_ROUTE_name,  SPA_POD_String(name),
		SPA_PARAM_ROUTE_description,  SPA_POD_String(description),
		SPA_PARAM_ROUTE_priority,  SPA_POD_Int(0),
		SPA_PARAM_ROUTE_available,  SPA_POD_Id(available),
		0);
	spa_pod_builder_prop(b, SPA_PARAM_ROUTE_info, 0);
	spa_pod_builder_push_struct(b, &f[1]);
	spa_pod_builder_int(b, 1);
	spa_pod_builder_add(b,
			SPA_POD_String("port.type"),
			SPA_POD_String(port_type),
			NULL);
	spa_pod_builder_pop(b, &f[1]);
	spa_pod_builder_prop(b, SPA_PARAM_ROUTE_profiles, 0);
	spa_pod_builder_push_array(b, &f[1]);

	mask = 0;
	for (i = 1; (j = get_profile_from_index(this, i, &next, &codec)) != SPA_ID_INVALID; i = next) {
		uint32_t profile_mask;

		if (j == DEVICE_PROFILE_A2DP && !(port == 0 || port == 1))
			continue;
		if (j == DEVICE_PROFILE_BAP && !(port == 0 || port == 1 || port == 4 || port == 5))
			continue;
		if (j == DEVICE_PROFILE_HSP_HFP && !(port == 2 || port == 3))
			continue;

		profile_mask = profile_direction_mask(this, j, codec);
		if (!(profile_mask & (1 << direction)))
			continue;

		/* Check the profile actually exists */
		if (!validate_profile(this, j, codec))
			continue;

		mask |= profile_mask;
		spa_pod_builder_int(b, i);
	}
	spa_pod_builder_pop(b, &f[1]);

	if (!(mask & (1 << direction))) {
		/* No profile has route direction */
		return NULL;
	}

	if (dev != SPA_ID_INVALID) {
		struct node *node = &this->nodes[dev];
		struct spa_bt_transport_volume *t_volume;

		mask = profile_direction_mask(this, this->profile, this->props.codec);
		if (!(mask & (1 << direction)))
			return NULL;

		t_volume = node->transport
			? &node->transport->volumes[node->id]
			: NULL;

		spa_pod_builder_prop(b, SPA_PARAM_ROUTE_device, 0);
		spa_pod_builder_int(b, dev);

		spa_pod_builder_prop(b, SPA_PARAM_ROUTE_props, 0);
		spa_pod_builder_push_object(b, &f[1], SPA_TYPE_OBJECT_Props, id);

		spa_pod_builder_prop(b, SPA_PROP_mute, 0);
		spa_pod_builder_bool(b, node->mute);

		spa_pod_builder_prop(b, SPA_PROP_channelVolumes,
			(t_volume && t_volume->active) ? SPA_POD_PROP_FLAG_HARDWARE : 0);
		spa_pod_builder_array(b, sizeof(float), SPA_TYPE_Float,
				node->n_channels, node->volumes);

		if (t_volume && t_volume->active) {
			spa_pod_builder_prop(b, SPA_PROP_volumeStep, SPA_POD_PROP_FLAG_READONLY);
			spa_pod_builder_float(b, 1.0f / (t_volume->hw_volume_max + 1));
		}

		spa_pod_builder_prop(b, SPA_PROP_channelMap, 0);
		spa_pod_builder_array(b, sizeof(uint32_t), SPA_TYPE_Id,
				node->n_channels, node->channels);

		if ((this->profile == DEVICE_PROFILE_A2DP || this->profile == DEVICE_PROFILE_BAP) &&
		    dev == DEVICE_ID_SINK) {
			spa_pod_builder_prop(b, SPA_PROP_latencyOffsetNsec, 0);
			spa_pod_builder_long(b, node->latency_offset);
		}

		spa_pod_builder_pop(b, &f[1]);

		spa_pod_builder_prop(b, SPA_PARAM_ROUTE_save, 0);
		spa_pod_builder_bool(b, node->save);
	}

	spa_pod_builder_prop(b, SPA_PARAM_ROUTE_devices, 0);
	spa_pod_builder_push_array(b, &f[1]);
	spa_pod_builder_int(b, enum_dev);
	spa_pod_builder_pop(b, &f[1]);

	if (profile != SPA_ID_INVALID) {
		spa_pod_builder_prop(b, SPA_PARAM_ROUTE_profile, 0);
		spa_pod_builder_int(b, profile);
	}
	return spa_pod_builder_pop(b, &f[0]);
}

static bool iterate_supported_media_codecs(struct impl *this, int *j, const struct media_codec **codec)
{
	int i;

next:
	*j = *j + 1;
	spa_assert(*j >= 0);
	if ((size_t)*j >= this->supported_codec_count)
		return false;

	for (i = 0; i < *j; ++i)
		if (this->supported_codecs[i]->id == this->supported_codecs[*j]->id)
			goto next;

	*codec = this->supported_codecs[*j];
	return true;
}

static struct spa_pod *build_prop_info_codec(struct impl *this, struct spa_pod_builder *b, uint32_t id)
{
	struct spa_pod_frame f[2];
	struct spa_pod_choice *choice;
	const struct media_codec *codec;
	size_t n;
	int j;

#define FOR_EACH_MEDIA_CODEC(j, codec) \
		for (j = -1; iterate_supported_media_codecs(this, &j, &codec);)
#define FOR_EACH_HFP_CODEC(j) \
		for (j = HFP_AUDIO_CODEC_MSBC; j >= HFP_AUDIO_CODEC_CVSD; --j) \
			if (spa_bt_device_supports_hfp_codec(this->bt_dev, j) == 1)

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_PropInfo, id);

	/*
	 * XXX: the ids in principle should use builder_id, not builder_int,
	 * XXX: but the type info for _type and _labels doesn't work quite right now.
	 */

	/* Transport codec */
	spa_pod_builder_prop(b, SPA_PROP_INFO_id, 0);
	spa_pod_builder_id(b, SPA_PROP_bluetoothAudioCodec);
	spa_pod_builder_prop(b, SPA_PROP_INFO_description, 0);
	spa_pod_builder_string(b, "Air codec");
	spa_pod_builder_prop(b, SPA_PROP_INFO_type, 0);
	spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
	choice = (struct spa_pod_choice *)spa_pod_builder_frame(b, &f[1]);
	n = 0;
	if (this->profile == DEVICE_PROFILE_A2DP || this->profile == DEVICE_PROFILE_BAP) {
		FOR_EACH_MEDIA_CODEC(j, codec) {
			if (n == 0)
				spa_pod_builder_int(b, codec->id);
			spa_pod_builder_int(b, codec->id);
			++n;
		}
	} else if (this->profile == DEVICE_PROFILE_HSP_HFP) {
		FOR_EACH_HFP_CODEC(j) {
			if (n == 0)
				spa_pod_builder_int(b, get_hfp_codec_id(j));
			spa_pod_builder_int(b, get_hfp_codec_id(j));
			++n;
		}
	}
	if (n == 0)
		choice->body.type = SPA_CHOICE_None;
	spa_pod_builder_pop(b, &f[1]);
	spa_pod_builder_prop(b, SPA_PROP_INFO_labels, 0);
	spa_pod_builder_push_struct(b, &f[1]);
	if (this->profile == DEVICE_PROFILE_A2DP || this->profile == DEVICE_PROFILE_BAP) {
		FOR_EACH_MEDIA_CODEC(j, codec) {
			spa_pod_builder_int(b, codec->id);
			spa_pod_builder_string(b, codec->description);
		}
	} else if (this->profile == DEVICE_PROFILE_HSP_HFP) {
		FOR_EACH_HFP_CODEC(j) {
			spa_pod_builder_int(b, get_hfp_codec_id(j));
			spa_pod_builder_string(b, get_hfp_codec_description(j));
		}
	}
	spa_pod_builder_pop(b, &f[1]);
	return spa_pod_builder_pop(b, &f[0]);

#undef FOR_EACH_MEDIA_CODEC
#undef FOR_EACH_HFP_CODEC
}

static struct spa_pod *build_props(struct impl *this, struct spa_pod_builder *b, uint32_t id)
{
	struct props *p = &this->props;

	return spa_pod_builder_add_object(b,
			SPA_TYPE_OBJECT_Props, id,
			SPA_PROP_bluetoothAudioCodec, SPA_POD_Id(p->codec),
			SPA_PROP_bluetoothOffloadActive, SPA_POD_Bool(p->offload_active));
}

static int impl_enum_params(void *object, int seq,
			    uint32_t id, uint32_t start, uint32_t num,
			    const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod *param;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[2048];
	struct spa_result_device_params result;
	uint32_t count = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(num != 0, -EINVAL);

	result.id = id;
	result.next = start;
      next:
	result.index = result.next++;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumProfile:
	{
		uint32_t profile;
		enum spa_bluetooth_audio_codec codec;

		profile = get_profile_from_index(this, result.index, &result.next, &codec);

		switch (profile) {
		case DEVICE_PROFILE_OFF:
		case DEVICE_PROFILE_AG:
		case DEVICE_PROFILE_A2DP:
		case DEVICE_PROFILE_BAP:
		case DEVICE_PROFILE_HSP_HFP:
			param = build_profile(this, &b, id, result.index, profile, codec, false);
			if (param == NULL)
				goto next;
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_Profile:
	{
		uint32_t index;

		switch (result.index) {
		case 0:
			index = get_index_from_profile(this, this->profile, this->props.codec);
			param = build_profile(this, &b, id, index, this->profile, this->props.codec, true);
			if (param == NULL)
				return 0;
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_EnumRoute:
	{
		switch (result.index) {
		case 0: case 1: case 2: case 3: case 4: case 5:
			param = build_route(this, &b, id, result.index, SPA_ID_INVALID);
			if (param == NULL)
				goto next;
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_Route:
	{
		switch (result.index) {
		case 0: case 1: case 2: case 3: case 4: case 5:
			param = build_route(this, &b, id, result.index, this->profile);
			if (param == NULL)
				goto next;
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_PropInfo:
	{
		switch (result.index) {
		case 0:
			param = build_prop_info_codec(this, &b, id);
			break;
		case 1:
			param = spa_pod_builder_add_object(&b,
					SPA_TYPE_OBJECT_PropInfo, id,
					SPA_PROP_INFO_id, SPA_POD_Id(SPA_PROP_bluetoothOffloadActive),
					SPA_PROP_INFO_description, SPA_POD_String("Bluetooth audio offload active"),
					SPA_PROP_INFO_type, SPA_POD_CHOICE_Bool(false));
			break;
		default:
			return 0;
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		switch (result.index) {
		case 0:
			param = build_props(this, &b, id);
			break;
		default:
			return 0;
		}
		break;
	}
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, &result.param, param, filter) < 0)
		goto next;

	spa_device_emit_result(&this->hooks, seq, 0,
			SPA_RESULT_TYPE_DEVICE_PARAMS, &result);

	if (++count != num)
		goto next;

	return 0;
}

static int node_set_volume(struct impl *this, struct node *node, float volumes[], uint32_t n_volumes)
{
	uint32_t i;
	int changed = 0;
	struct spa_bt_transport_volume *t_volume;

	if (n_volumes == 0)
		return -EINVAL;

	spa_log_info(this->log, "node %d volume %f", node->id, volumes[0]);

	for (i = 0; i < node->n_channels; i++) {
		if (node->volumes[i] == volumes[i % n_volumes])
			continue;
		++changed;
		node->volumes[i] = volumes[i % n_volumes];
	}

	t_volume = node->transport ? &node->transport->volumes[node->id]: NULL;

	if (t_volume && t_volume->active
	    && spa_bt_transport_volume_enabled(node->transport)) {
		float hw_volume = node_get_hw_volume(node);
		spa_log_debug(this->log, "node %d hardware volume %f", node->id, hw_volume);

		node_update_soft_volumes(node, hw_volume);
		spa_bt_transport_set_volume(node->transport, node->id, hw_volume);
	} else {
		float boost = get_soft_volume_boost(node);
		for (uint32_t i = 0; i < node->n_channels; ++i)
			node->soft_volumes[i] = node->volumes[i] * boost;
	}

	emit_volume(this, node);

	return changed;
}

static int node_set_mute(struct impl *this, struct node *node, bool mute)
{
	struct spa_event *event;
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	struct spa_pod_frame f[1];
	int changed = 0;

	spa_log_info(this->log, "node %d mute %d", node->id, mute);

	changed = (node->mute != mute);
	node->mute = mute;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_EVENT_Device, SPA_DEVICE_EVENT_ObjectConfig);
	spa_pod_builder_prop(&b, SPA_EVENT_DEVICE_Object, 0);
	spa_pod_builder_int(&b, node->id);
	spa_pod_builder_prop(&b, SPA_EVENT_DEVICE_Props, 0);

	spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Props, SPA_EVENT_DEVICE_Props,
			SPA_PROP_mute, SPA_POD_Bool(mute),
			SPA_PROP_softMute, SPA_POD_Bool(mute));
	event = spa_pod_builder_pop(&b, &f[0]);

	spa_device_emit_event(&this->hooks, event);

	return changed;
}

static int node_set_latency_offset(struct impl *this, struct node *node, int64_t latency_offset)
{
	struct spa_event *event;
	uint8_t buffer[4096];
	struct spa_pod_builder b = { 0 };
	struct spa_pod_frame f[1];
	int changed = 0;

	spa_log_info(this->log, "node %d latency offset %"PRIi64" nsec", node->id, latency_offset);

	changed = (node->latency_offset != latency_offset);
	node->latency_offset = latency_offset;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_EVENT_Device, SPA_DEVICE_EVENT_ObjectConfig);
	spa_pod_builder_prop(&b, SPA_EVENT_DEVICE_Object, 0);
	spa_pod_builder_int(&b, node->id);
	spa_pod_builder_prop(&b, SPA_EVENT_DEVICE_Props, 0);

	spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Props, SPA_EVENT_DEVICE_Props,
			SPA_PROP_latencyOffsetNsec, SPA_POD_Long(latency_offset));
	event = spa_pod_builder_pop(&b, &f[0]);

	spa_device_emit_event(&this->hooks, event);

	return changed;
}

static int apply_device_props(struct impl *this, struct node *node, struct spa_pod *props)
{
	float volume = 0;
	bool mute = 0;
	struct spa_pod_prop *prop;
	struct spa_pod_object *obj = (struct spa_pod_object *) props;
	int changed = 0;
	float volumes[SPA_AUDIO_MAX_CHANNELS];
	uint32_t channels[SPA_AUDIO_MAX_CHANNELS];
	uint32_t n_volumes = 0, SPA_UNUSED n_channels = 0;
	int64_t latency_offset = 0;

	if (!spa_pod_is_object_type(props, SPA_TYPE_OBJECT_Props))
		return -EINVAL;

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_volume:
			if (spa_pod_get_float(&prop->value, &volume) == 0) {
				int res = node_set_volume(this, node, &volume, 1);
				if (res > 0)
					++changed;
			}
			break;
		case SPA_PROP_mute:
			if (spa_pod_get_bool(&prop->value, &mute) == 0) {
				int res = node_set_mute(this, node, mute);
				if (res > 0)
					++changed;
			}
			break;
		case SPA_PROP_channelVolumes:
			n_volumes = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					volumes, SPA_AUDIO_MAX_CHANNELS);
			break;
		case SPA_PROP_channelMap:
			n_channels = spa_pod_copy_array(&prop->value, SPA_TYPE_Id,
					channels, SPA_AUDIO_MAX_CHANNELS);
			break;
		case SPA_PROP_latencyOffsetNsec:
			if (spa_pod_get_long(&prop->value, &latency_offset) == 0) {
				int res = node_set_latency_offset(this, node, latency_offset);
				if (res > 0)
					++changed;
			}
		}
	}
	if (n_volumes > 0) {
		int res = node_set_volume(this, node, volumes, n_volumes);
		if (res > 0)
			++changed;
	}

	return changed;
}

static void apply_prop_offload_active(struct impl *this, bool active)
{
	bool old_value = this->props.offload_active;
	unsigned int i;

	this->props.offload_active = active;

	for (i = 0; i < SPA_N_ELEMENTS(this->nodes); i++) {
		node_offload_set_active(&this->nodes[i], active);
		if (!this->nodes[i].offload_acquired)
			this->props.offload_active = false;
	}

	if (this->props.offload_active != old_value) {
		this->info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
		this->params[IDX_Props].flags ^= SPA_PARAM_INFO_SERIAL;
		emit_info(this, false);
	}
}

static int impl_set_param(void *object,
			  uint32_t id, uint32_t flags,
			  const struct spa_pod *param)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_PARAM_Profile:
	{
		uint32_t idx, next;
		uint32_t profile;
		enum spa_bluetooth_audio_codec codec;
		bool save = false;

		if (param == NULL)
			return -EINVAL;

		if ((res = spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&idx),
				SPA_PARAM_PROFILE_save, SPA_POD_OPT_Bool(&save))) < 0) {
			spa_log_warn(this->log, "can't parse profile");
			spa_debug_log_pod(this->log, SPA_LOG_LEVEL_DEBUG, 0, NULL, param);
			return res;
		}

		profile = get_profile_from_index(this, idx, &next, &codec);
		if (profile == SPA_ID_INVALID)
			return -EINVAL;

		spa_log_debug(this->log, "%p: setting profile %d codec:%d save:%d", this,
				profile, codec, (int)save);
		return set_profile(this, profile, codec, save);
	}
	case SPA_PARAM_Route:
	{
		uint32_t idx, device;
		struct spa_pod *props = NULL;
		struct node *node;
		bool save = false;

		if (param == NULL)
			return -EINVAL;

		if ((res = spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamRoute, NULL,
				SPA_PARAM_ROUTE_index, SPA_POD_Int(&idx),
				SPA_PARAM_ROUTE_device, SPA_POD_Int(&device),
				SPA_PARAM_ROUTE_props, SPA_POD_OPT_Pod(&props),
				SPA_PARAM_ROUTE_save, SPA_POD_OPT_Bool(&save))) < 0) {
			spa_log_warn(this->log, "can't parse route");
			spa_debug_log_pod(this->log, SPA_LOG_LEVEL_DEBUG, 0, NULL, param);
			return res;
		}
		if (device >= SPA_N_ELEMENTS(this->nodes) || !this->nodes[device].active)
			return -EINVAL;

		node = &this->nodes[device];
		node->save = save;
		if (props) {
			int changed = apply_device_props(this, node, props);
			if (changed > 0) {
				this->info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
				this->params[IDX_Route].flags ^= SPA_PARAM_INFO_SERIAL;
			}
			emit_info(this, false);
		}
		break;
	}
	case SPA_PARAM_Props:
	{
		uint32_t codec_id = SPA_ID_INVALID;
		bool offload_active = this->props.offload_active;

		if (param == NULL)
			return 0;

		if ((res = spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_Props, NULL,
				SPA_PROP_bluetoothAudioCodec, SPA_POD_OPT_Id(&codec_id),
				SPA_PROP_bluetoothOffloadActive, SPA_POD_OPT_Bool(&offload_active))) < 0) {
			spa_log_warn(this->log, "can't parse props");
			spa_debug_log_pod(this->log, SPA_LOG_LEVEL_DEBUG, 0, NULL, param);
			return res;
		}

		spa_log_debug(this->log, "setting props codec:%d offload:%d", (int)codec_id, (int)offload_active);

		apply_prop_offload_active(this, offload_active);

		if (codec_id == SPA_ID_INVALID)
			return 0;

		if (this->profile == DEVICE_PROFILE_A2DP || this->profile == DEVICE_PROFILE_BAP) {
			size_t j;
			for (j = 0; j < this->supported_codec_count; ++j) {
				if (this->supported_codecs[j]->id == codec_id) {
					return set_profile(this, this->profile, codec_id, true);
				}
			}
		} else if (this->profile == DEVICE_PROFILE_HSP_HFP) {
			if (codec_id == SPA_BLUETOOTH_AUDIO_CODEC_CVSD &&
					spa_bt_device_supports_hfp_codec(this->bt_dev, HFP_AUDIO_CODEC_CVSD) == 1) {
				return set_profile(this, this->profile, codec_id, true);
			} else if (codec_id == SPA_BLUETOOTH_AUDIO_CODEC_MSBC &&
					spa_bt_device_supports_hfp_codec(this->bt_dev, HFP_AUDIO_CODEC_MSBC) == 1) {
				return set_profile(this, this->profile, codec_id, true);
			}
		}
		return -EINVAL;
	}
	default:
		return -ENOENT;
	}
	return 0;
}

static const struct spa_device_methods impl_device = {
	SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_add_listener,
	.sync = impl_sync,
	.enum_params = impl_enum_params,
	.set_param = impl_set_param,
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
	struct impl *this = (struct impl *) handle;
	const struct spa_dict_item *it;

	emit_remove_nodes(this);

	free(this->supported_codecs);
	if (this->bt_dev) {
		this->bt_dev->settings = NULL;
		spa_hook_remove(&this->bt_dev_listener);
	}

	spa_dict_for_each(it, &this->setting_dict) {
		if(it->key)
			free((void *)it->key);
		if(it->value)
			free((void *)it->value);
	}

	device_set_clear(this);
	return 0;
}

static size_t
impl_get_size(const struct spa_handle_factory *factory,
	      const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static const struct spa_dict*
filter_bluez_device_setting(struct impl *this, const struct spa_dict *dict)
{
	uint32_t n_items = 0;
	for (uint32_t i = 0
		; i < dict->n_items && n_items < SPA_N_ELEMENTS(this->setting_items)
		; i++)
	{
		const struct spa_dict_item *it = &dict->items[i];
		if (it->key != NULL && strncmp(it->key, "bluez", 5) == 0 && it->value != NULL) {
			this->setting_items[n_items++] =
				SPA_DICT_ITEM_INIT(strdup(it->key), strdup(it->value));
		}
	}
	this->setting_dict = SPA_DICT_INIT(this->setting_items, n_items);
	return &this->setting_dict;
}

static int
impl_init(const struct spa_handle_factory *factory,
	  struct spa_handle *handle,
	  const struct spa_dict *info,
	  const struct spa_support *support,
	  uint32_t n_support)
{
	struct impl *this;
	const char *str;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;

	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	_i18n = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_I18N);

	spa_log_topic_init(this->log, &log_topic);

	if (info && (str = spa_dict_lookup(info, SPA_KEY_API_BLUEZ5_DEVICE)))
		sscanf(str, "pointer:%p", &this->bt_dev);

	if (this->bt_dev == NULL) {
		spa_log_error(this->log, "a device is needed");
		return -EINVAL;
	}

	if (info) {
		int profiles;
		this->bt_dev->settings = filter_bluez_device_setting(this, info);

		if ((str = spa_dict_lookup(info, "bluez5.auto-connect")) != NULL) {
			if ((profiles = spa_bt_profiles_from_json_array(str)) >= 0)
				this->bt_dev->reconnect_profiles = profiles;
		}

		if ((str = spa_dict_lookup(info, "bluez5.hw-volume")) != NULL) {
			if ((profiles = spa_bt_profiles_from_json_array(str)) >= 0)
				this->bt_dev->hw_volume_profiles = profiles;
		}
	}

	this->device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, this);

	spa_hook_list_init(&this->hooks);

	reset_props(&this->props);

	init_node(this, &this->nodes[0], 0);
	init_node(this, &this->nodes[1], 1);
	init_node(this, &this->nodes[2], 2);
	init_node(this, &this->nodes[3], 3);

	this->info = SPA_DEVICE_INFO_INIT();
	this->info_all = SPA_DEVICE_CHANGE_MASK_PROPS |
		SPA_DEVICE_CHANGE_MASK_PARAMS;

	this->params[IDX_EnumProfile] = SPA_PARAM_INFO(SPA_PARAM_EnumProfile, SPA_PARAM_INFO_READ);
	this->params[IDX_Profile] = SPA_PARAM_INFO(SPA_PARAM_Profile, SPA_PARAM_INFO_READWRITE);
	this->params[IDX_EnumRoute] = SPA_PARAM_INFO(SPA_PARAM_EnumRoute, SPA_PARAM_INFO_READ);
	this->params[IDX_Route] = SPA_PARAM_INFO(SPA_PARAM_Route, SPA_PARAM_INFO_READWRITE);
	this->params[IDX_PropInfo] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	this->params[IDX_Props] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = 6;

	spa_bt_device_add_listener(this->bt_dev, &this->bt_dev_listener, &bt_dev_events, this);

	this->device_set.impl = this;

	set_initial_profile(this);

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

static const struct spa_dict_item handle_info_items[] = {
	{ SPA_KEY_FACTORY_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ SPA_KEY_FACTORY_DESCRIPTION, "A bluetooth device" },
	{ SPA_KEY_FACTORY_USAGE, SPA_KEY_API_BLUEZ5_DEVICE"=<device>" },
};

static const struct spa_dict handle_info = SPA_DICT_INIT_ARRAY(handle_info_items);

const struct spa_handle_factory spa_bluez5_device_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	SPA_NAME_API_BLUEZ5_DEVICE,
	&handle_info,
	impl_get_size,
	impl_init,
	impl_enum_interface_info,
};
