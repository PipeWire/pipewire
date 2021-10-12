/* PipeWire
 *
 * Copyright © 2019 Wim Taymans
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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include "config.h"

#include <spa/node/node.h>
#include <spa/utils/hook.h>
#include <spa/utils/string.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/pod.h>
#include <spa/pod/filter.h>
#include <spa/utils/json.h>

#include "pipewire/pipewire.h"
#include "pipewire/extensions/metadata.h"

#include "media-session.h"

/** \page page_media_session_module_policy_node Media Session Module: Policy Node
 */

#define NAME		"policy-node"
#define SESSION_KEY	"policy-node"

#define DEFAULT_IDLE_SECONDS	3

#define DEFAULT_AUDIO_SINK_KEY		"default.audio.sink"
#define DEFAULT_AUDIO_SOURCE_KEY	"default.audio.source"
#define DEFAULT_VIDEO_SOURCE_KEY	"default.video.source"
#define DEFAULT_CONFIG_AUDIO_SINK_KEY	"default.configured.audio.sink"
#define DEFAULT_CONFIG_AUDIO_SOURCE_KEY	"default.configured.audio.source"
#define DEFAULT_CONFIG_VIDEO_SOURCE_KEY	"default.configured.video.source"

#define DEFAULT_AUDIO_SINK		0
#define DEFAULT_AUDIO_SOURCE		1
#define DEFAULT_VIDEO_SOURCE		2

#define MAX_LINK_RETRY			5

PW_LOG_TOPIC_STATIC(mod_topic, "ms.mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct default_node {
	char *key;
	char *key_config;
	char *value;
	char *config;
};

struct impl {
	struct timespec now;

	struct sm_media_session *session;
	struct spa_hook listener;

	struct spa_hook meta_listener;

	struct pw_context *context;

	uint32_t sample_rate;

	struct spa_list node_list;
	unsigned int node_list_changed:1;
	unsigned int linking_node_removed:1;
	int seq;

	struct default_node defaults[4];

	bool streams_follow_default;
	bool alsa_no_dsp;
};

struct node {
	struct sm_node *obj;

	uint32_t id;
	struct impl *impl;

	struct spa_list link;		/**< link in impl node_list */
	enum pw_direction direction;

	struct spa_hook listener;

	struct node *peer;
	struct node *failed_peer;

	uint32_t client_id;
	int32_t priority;

#define NODE_TYPE_UNKNOWN	0
#define NODE_TYPE_STREAM	1
#define NODE_TYPE_DEVICE	2
	uint32_t type;
	char *media;

	struct spa_audio_info format;

	int connect_count;
	int failed_count;
	uint64_t plugged;
	unsigned int active:1;
	unsigned int exclusive:1;
	unsigned int enabled:1;
	unsigned int configured:1;
	unsigned int dont_remix:1;
	unsigned int monitor:1;
	unsigned int capture_sink:1;
	unsigned int virtual:1;
	unsigned int linking:1;
	unsigned int have_passthrough:1;
	unsigned int passthrough_only:1;
	unsigned int passthrough:1;
	unsigned int want_passthrough:1;
	unsigned int unpositioned:1;
};

static bool is_unpositioned(struct spa_audio_info *info)
{
	uint32_t i;
	if (SPA_FLAG_IS_SET(info->info.raw.flags, SPA_AUDIO_FLAG_UNPOSITIONED))
		return true;
	for (i = 0; i < info->info.raw.channels; i++)
		if (info->info.raw.position[i] >= SPA_AUDIO_CHANNEL_START_Aux &&
		    info->info.raw.position[i] <= SPA_AUDIO_CHANNEL_LAST_Aux)
			return true;
	return false;
}

static bool find_format(struct node *node)
{
	struct impl *impl = node->impl;
	struct sm_param *p;
	bool have_format = false;

	node->have_passthrough = false;
	node->passthrough_only = false;

	spa_list_for_each(p, &node->obj->param_list, link) {
		struct spa_audio_info info = { 0, };
		struct spa_pod *position = NULL;
		uint32_t n_position = 0;

		if (p->id != SPA_PARAM_EnumFormat)
			continue;

		if (spa_format_parse(p->param, &info.media_type, &info.media_subtype) < 0)
			continue;

		if (info.media_type != SPA_MEDIA_TYPE_audio)
			continue;

		switch (info.media_subtype) {
		case SPA_MEDIA_SUBTYPE_raw:
			spa_pod_object_fixate((struct spa_pod_object*)p->param);
			if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
				spa_debug_pod(2, NULL, p->param);

			/* defaults */
			info.info.raw.format = SPA_AUDIO_FORMAT_F32;
			info.info.raw.rate = impl->sample_rate;
			info.info.raw.channels = 2;
			info.info.raw.position[0] = SPA_AUDIO_CHANNEL_FL;
			info.info.raw.position[1] = SPA_AUDIO_CHANNEL_FR;

			spa_pod_parse_object(p->param,
				SPA_TYPE_OBJECT_Format, NULL,
				SPA_FORMAT_AUDIO_format,	SPA_POD_Id(&info.info.raw.format),
				SPA_FORMAT_AUDIO_rate,		SPA_POD_OPT_Int(&info.info.raw.rate),
				SPA_FORMAT_AUDIO_channels,	SPA_POD_Int(&info.info.raw.channels),
				SPA_FORMAT_AUDIO_position,	SPA_POD_OPT_Pod(&position));

			if (position != NULL)
				n_position = spa_pod_copy_array(position, SPA_TYPE_Id,
						info.info.raw.position, SPA_AUDIO_MAX_CHANNELS);
			if (n_position == 0 || n_position != info.info.raw.channels)
				SPA_FLAG_SET(info.info.raw.flags, SPA_AUDIO_FLAG_UNPOSITIONED);

			if (node->format.info.raw.channels < info.info.raw.channels) {
				node->format = info;
				if (is_unpositioned(&info))
					node->unpositioned = true;
			}
			have_format = true;
			break;

		case SPA_MEDIA_SUBTYPE_iec958:
		case SPA_MEDIA_SUBTYPE_dsd:
			pw_log_info("passthrough node %d found", node->id);
			node->have_passthrough = true;
			break;
		}
	}
	if (!have_format && node->have_passthrough) {
		pw_log_info("passthrough only node %d found", node->id);
		node->passthrough_only = true;
		have_format = true;
	}
	return have_format;
}

static bool check_passthrough(struct node *node, struct node *peer)
{
	struct sm_param *p1, *p2;
	char buffer[1024];
	struct spa_pod_builder b;
	struct spa_pod *res;

	if (peer->obj->info == NULL)
		return false;

	if (peer->obj->info->state == PW_NODE_STATE_RUNNING)
		return false;

	if (!node->have_passthrough || !peer->have_passthrough)
		return false;

	spa_list_for_each(p1, &node->obj->param_list, link) {
		if (p1->id != SPA_PARAM_EnumFormat)
			continue;

		spa_list_for_each(p2, &peer->obj->param_list, link) {
			if (p2->id != SPA_PARAM_EnumFormat)
				continue;

			spa_pod_builder_init(&b, buffer, sizeof(buffer));
			if (spa_pod_filter(&b, &res, p1->param, p2->param) >= 0)
				return true;
		}
	}
	return false;
}

static void ensure_suspended(struct node *node)
{
	struct spa_command *cmd = &SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Suspend);

	if (node->obj->info->state < PW_NODE_STATE_IDLE)
		return;

	pw_node_send_command((struct pw_node*)node->obj->obj.proxy, cmd);
}

static int configure_passthrough(struct node *node)
{
	char buf[1024];
	struct spa_pod_builder b = { 0, };
	struct spa_pod *param;

	pw_log_info("node %d passthrough", node->id);

	ensure_suspended(node);

	spa_pod_builder_init(&b, buf, sizeof(buf));
	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(node->direction),
		SPA_PARAM_PORT_CONFIG_mode,	 SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_passthrough),
		SPA_PARAM_PORT_CONFIG_monitor,   SPA_POD_Bool(false));

	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_pod(2, NULL, param);

	pw_node_set_param((struct pw_node*)node->obj->obj.proxy,
			SPA_PARAM_PortConfig, 0, param);

	node->configured = true;
	node->passthrough = true;

	return 0;
}

static int configure_node(struct node *node, struct spa_audio_info *info, bool force)
{
	struct impl *impl = node->impl;
	char buf[1024];
	struct spa_pod_builder b = { 0, };
	struct spa_pod *param;
	struct spa_audio_info format;
	enum pw_direction direction;
	uint32_t mode;

	if (node->configured && !force) {
		pw_log_debug("node %d is configured passthrough:%d", node->id, node->passthrough);
		return 0;
	}

	if (!spa_streq(node->media, "Audio"))
		return 0;

	ensure_suspended(node);

	format = node->format;

	if (impl->alsa_no_dsp) {
		if ((info != NULL && memcmp(&node->format, info, sizeof(node->format)) == 0) ||
				node->type == NODE_TYPE_DEVICE)
			mode = SPA_PARAM_PORT_CONFIG_MODE_passthrough;
		else
			mode = SPA_PARAM_PORT_CONFIG_MODE_convert;
	} else {
		mode = SPA_PARAM_PORT_CONFIG_MODE_dsp;
	}

	if (mode != SPA_PARAM_PORT_CONFIG_MODE_passthrough &&
			info != NULL && info->info.raw.channels > 0) {
		pw_log_info("node %d monitor:%d channelmix %d->%d",
			node->id, node->monitor, format.info.raw.channels,
			info->info.raw.channels);
		format = *info;
	} else {
		pw_log_info("node %d monitor:%d channelmix %d",
			node->id, node->monitor, format.info.raw.channels);
	}
	format.info.raw.rate = impl->sample_rate;

	if (node->virtual)
		direction = pw_direction_reverse(node->direction);
	else
		direction = node->direction;

	spa_pod_builder_init(&b, buf, sizeof(buf));
	param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &format.info.raw);
	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(direction),
		SPA_PARAM_PORT_CONFIG_mode,	 SPA_POD_Id(mode),
		SPA_PARAM_PORT_CONFIG_monitor,   SPA_POD_Bool(true),
		SPA_PARAM_PORT_CONFIG_format,    SPA_POD_Pod(param));

	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_pod(2, NULL, param);

	pw_node_set_param((struct pw_node*)node->obj->obj.proxy,
			SPA_PARAM_PortConfig, 0, param);

	node->configured = true;
	node->passthrough = false;

	if (node->type == NODE_TYPE_DEVICE) {
		/* Schedule rescan in case we need to move streams */
		sm_media_session_schedule_rescan(impl->session);
	}

	return 0;
}

static void object_update(void *data)
{
	struct node *node = data;
	struct impl *impl = node->impl;
	const char *str;

	pw_log_debug("%p: node %d %08x", impl, node->id, node->obj->obj.changed);

	if (node->obj->obj.avail & SM_NODE_CHANGE_MASK_INFO &&
	    node->obj->info != NULL && node->obj->info->props != NULL) {
		str = spa_dict_lookup(node->obj->info->props, PW_KEY_NODE_EXCLUSIVE);
		node->exclusive = str ? pw_properties_parse_bool(str) : false;
	}

	if (!node->active) {
		if (node->obj->obj.avail & SM_NODE_CHANGE_MASK_PARAMS) {
			if (!find_format(node)) {
				pw_log_debug("%p: node %d can't find format", impl, node->id);
				return;
			}
			node->active = true;
		}
		if (node->active)
			sm_media_session_schedule_rescan(impl->session);
	}
}

static const struct sm_object_events object_events = {
	SM_VERSION_OBJECT_EVENTS,
	.update = object_update
};

static int
handle_node(struct impl *impl, struct sm_object *object)
{
	const char *media_class = NULL, *role;
	enum pw_direction direction;
	struct node *node;
	uint32_t client_id = SPA_ID_INVALID;

	if (object->props) {
		pw_properties_fetch_uint32(object->props, PW_KEY_CLIENT_ID, &client_id);

		media_class = pw_properties_get(object->props, PW_KEY_MEDIA_CLASS);
		role = pw_properties_get(object->props, PW_KEY_MEDIA_ROLE);
	}

	pw_log_debug("%p: node "PW_KEY_MEDIA_CLASS" %s", impl, media_class);

	if (media_class == NULL)
		return 0;

	node = sm_object_add_data(object, SESSION_KEY, sizeof(struct node));
	node->obj = (struct sm_node*)object;
	node->id = object->id;
	node->impl = impl;
	node->client_id = client_id;
	node->type = NODE_TYPE_UNKNOWN;
	spa_list_append(&impl->node_list, &node->link);
	impl->node_list_changed = true;

	if (role && spa_streq(role, "DSP"))
		node->active = node->configured = true;

	if (spa_strstartswith(media_class, "Stream/")) {
		media_class += strlen("Stream/");

		if (spa_strstartswith(media_class, "Output/")) {
			direction = PW_DIRECTION_OUTPUT;
			media_class += strlen("Output/");
		}
		else if (spa_strstartswith(media_class, "Input/")) {
			direction = PW_DIRECTION_INPUT;
			media_class += strlen("Input/");
		}
		else
			return 0;

		if (spa_strstartswith(media_class, "Video")) {
			if (direction == PW_DIRECTION_OUTPUT) {
				node->plugged = pw_properties_get_uint64(object->props, PW_KEY_NODE_PLUGGED,
									 SPA_TIMESPEC_TO_NSEC(&impl->now));
			}
			node->active = node->configured = true;
		}
		else if (spa_strstartswith(media_class, "Unknown")) {
			node->active = node->configured = true;
		}

		node->direction = direction;
		node->type = NODE_TYPE_STREAM;
		node->media = strdup(media_class);
		pw_log_debug("%p: node %d is stream %s", impl, object->id, node->media);
	}
	else {
		const char *media;
		bool virtual = false;

		if (spa_strstartswith(media_class, "Audio/")) {
			media_class += strlen("Audio/");
			media = "Audio";
		}
		else if (spa_strstartswith(media_class, "Video/")) {
			media_class += strlen("Video/");
			media = "Video";
			node->active = node->configured = true;
		}
		else
			return 0;

		if (spa_streq(media_class, "Sink") ||
		    spa_streq(media_class, "Duplex"))
			direction = PW_DIRECTION_INPUT;
		else if (spa_streq(media_class, "Source"))
			direction = PW_DIRECTION_OUTPUT;
		else if (spa_streq(media_class, "Source/Virtual")) {
			virtual = true;
			direction = PW_DIRECTION_OUTPUT;
		} else
			return 0;

		node->plugged = pw_properties_get_uint64(object->props, PW_KEY_NODE_PLUGGED,
							 SPA_TIMESPEC_TO_NSEC(&impl->now));
		node->priority = pw_properties_get_uint32(object->props, PW_KEY_PRIORITY_SESSION, 0);

		node->direction = direction;
		node->virtual = virtual;
		node->type = NODE_TYPE_DEVICE;
		node->media = strdup(media);

		pw_log_debug("%p: node %d '%s' prio:%d", impl,
				object->id, node->media, node->priority);
	}

	node->enabled = true;
	node->obj->obj.mask |= SM_NODE_CHANGE_MASK_PARAMS;
	sm_object_add_listener(&node->obj->obj, &node->listener, &object_events, node);

	return 1;
}

static void unpeer_node(struct node *node)
{
	struct impl *impl = node->impl;
	pw_log_debug("unpeer id:%d exclusive:%d", node->id, node->exclusive);
	if (node->passthrough) {
		node->passthrough = false;
		node->configured = false;
		sm_media_session_schedule_rescan(impl->session);
	}
}

static void destroy_node(struct impl *impl, struct node *node)
{
	pw_log_debug("destroy %d %p", node->id, node->peer);
	spa_list_remove(&node->link);
	if (node->linking)
		impl->linking_node_removed = true;
	impl->node_list_changed = true;
	if (node->enabled)
		spa_hook_remove(&node->listener);
	free(node->media);
	if (node->peer)
		unpeer_node(node->peer);
	if (node->peer && node->peer->peer == node)
		node->peer->peer = NULL;
	sm_object_remove_data((struct sm_object*)node->obj, SESSION_KEY);
}

static int json_object_find(const char *obj, const char *key, char *value, size_t len)
{
	struct spa_json it[2];
	const char *v;
	char k[128];

	spa_json_init(&it[0], obj, strlen(obj));
	if (spa_json_enter_object(&it[0], &it[1]) <= 0)
		return -EINVAL;

	while (spa_json_get_string(&it[1], k, sizeof(k)-1) > 0) {
		if (spa_streq(k, key)) {
			if (spa_json_get_string(&it[1], value, len) <= 0)
				continue;
			return 0;
		} else {
			if (spa_json_next(&it[1], &v) <= 0)
				break;
		}
	}
	return -ENOENT;
}

static bool check_node_name(struct node *node, const char *name)
{
	const char *str;
	if ((str = pw_properties_get(node->obj->obj.props, PW_KEY_NODE_NAME)) != NULL &&
	    name != NULL && spa_streq(str, name))
		return true;
	return false;
}

static struct node *find_node_by_id_name(struct impl *impl, uint32_t id, const char *name)
{
	struct node *node;
	uint32_t name_id = name ? (uint32_t)atoi(name) : SPA_ID_INVALID;

	spa_list_for_each(node, &impl->node_list, link) {
		if (node->id == id || node->id == name_id)
			return node;
		if (check_node_name(node, name))
			return node;
	}
	return NULL;
}

static bool can_link_check(struct impl *impl, const char *link_group, struct node *target, int hops)
{
	const char *g, *ng;
	struct node *n;

	if (hops == 8)
		return false;

	pw_log_debug("link group %s", link_group);

	if (target->obj->info == NULL || target->obj->info->props == NULL)
		return true;
	g = spa_dict_lookup(target->obj->info->props, PW_KEY_NODE_LINK_GROUP);
	if (g == NULL)
		return true;
	if (spa_streq(g, link_group))
		return false;

	spa_list_for_each(n, &impl->node_list, link) {
		if (n == target || n->direction != target->direction)
			continue;
		if (n->obj->info == NULL || n->obj->info->props == NULL)
			return true;
		ng = spa_dict_lookup(n->obj->info->props, PW_KEY_NODE_LINK_GROUP);
		if (ng == NULL || !spa_streq(ng, g))
			continue;
		if (n->peer != NULL && !can_link_check(impl, link_group, n->peer, hops + 1))
			return false;
	}
	return true;
}

static bool can_link(struct impl *impl, struct node *node, struct node *target)
{
	const char *link_group;

	link_group = spa_dict_lookup(node->obj->info->props, PW_KEY_NODE_LINK_GROUP);
	if (link_group == NULL)
		return true;

	return can_link_check(impl, link_group, target, 0);
}

static const char *get_device_name(struct node *node)
{
	if (node->type != NODE_TYPE_DEVICE ||
	    node->obj->obj.props == NULL)
		return NULL;
	return pw_properties_get(node->obj->obj.props, PW_KEY_NODE_NAME);
}

static uint32_t find_device_for_name(struct impl *impl, const char *name)
{
	struct node *node;
	const char *str;
	uint32_t id = atoi(name);

	spa_list_for_each(node, &impl->node_list, link) {
		if (id == node->obj->obj.id)
			return id;
		if ((str = get_device_name(node)) == NULL)
			continue;
		if (spa_streq(str, name))
			return node->obj->obj.id;
	}
	return SPA_ID_INVALID;
}

static void session_create(void *data, struct sm_object *object)
{
	struct impl *impl = data;
	int res;

	clock_gettime(CLOCK_MONOTONIC, &impl->now);

	if (spa_streq(object->type, PW_TYPE_INTERFACE_Node))
		res = handle_node(impl, object);
	else
		res = 0;

	if (res < 0) {
		pw_log_warn("%p: can't handle global %d", impl, object->id);
	} else
		sm_media_session_schedule_rescan(impl->session);
}

static void session_remove(void *data, struct sm_object *object)
{
	struct impl *impl = data;
	pw_log_debug("%p: remove global '%d'", impl, object->id);

	if (spa_streq(object->type, PW_TYPE_INTERFACE_Node)) {
		struct node *n, *node;

		if ((node = sm_object_get_data(object, SESSION_KEY)) != NULL)
			destroy_node(impl, node);

		spa_list_for_each(n, &impl->node_list, link) {
			if (n->peer == node)
				n->peer = NULL;
			if (n->failed_peer == node)
				n->failed_peer = NULL;
		}
	}
	sm_media_session_schedule_rescan(impl->session);
}

static bool array_contains(const struct spa_pod *pod, uint32_t val)
{
	uint32_t *vals, n_vals;
	uint32_t n;

	if (pod == NULL)
		return false;
	vals = spa_pod_get_array(pod, &n_vals);
	if (vals == NULL || n_vals == 0)
		return false;
	for (n = 0; n < n_vals; n++)
		if (vals[n] == val)
			return true;
	return false;
}

static bool have_available_route(struct node *node, struct sm_device *dev)
{
	struct sm_param *p;
	const char *str;
	uint32_t card_profile_device;
	int found = 0, avail = 0;

	if (node->obj->info == NULL || node->obj->info->props == NULL ||
	    (str = spa_dict_lookup(node->obj->info->props, "card.profile.device")) == NULL)
		return 1;

	if (!spa_atou32(str, &card_profile_device, 0))
		return 1;

	spa_list_for_each(p, &dev->param_list, link) {
		uint32_t device_id;
		enum spa_param_availability available;

		if (p->id != SPA_PARAM_Route)
			continue;

		if (spa_pod_parse_object(p->param,
					SPA_TYPE_OBJECT_ParamRoute, NULL,
					SPA_PARAM_ROUTE_device, SPA_POD_Int(&device_id),
					SPA_PARAM_ROUTE_available,  SPA_POD_Id(&available)) < 0)
			continue;

		/* we found the route for the device */
		if (device_id != card_profile_device)
			continue;
		if (available == SPA_PARAM_AVAILABILITY_no)
			return 0;
		return 1;
	}
	/* Route is not found so no active profile. Check if there is a route that
	 * is available */
	spa_list_for_each(p, &dev->param_list, link) {
		struct spa_pod *devices = NULL;
		enum spa_param_availability available;

		if (p->id != SPA_PARAM_EnumRoute)
			continue;

		if (spa_pod_parse_object(p->param,
					SPA_TYPE_OBJECT_ParamRoute, NULL,
					SPA_PARAM_ROUTE_devices, SPA_POD_OPT_Pod(&devices),
					SPA_PARAM_ROUTE_available,  SPA_POD_Id(&available)) < 0)
			continue;

		if (!array_contains(devices, card_profile_device))
			continue;
		found++;
		if (available != SPA_PARAM_AVAILABILITY_no)
			avail++;
	}
	if (found == 0)
		return 1;
	if (avail > 0)
		return 1;
	return 0;
}

struct find_data {
	struct impl *impl;
	struct node *result;
	struct node *node;

	const char *media;
	const char *link_group;
	bool capture_sink;
	enum pw_direction direction;

	bool exclusive;
	bool have_passthrough;
	bool passthrough_only;
	bool can_passthrough;
	int priority;
	uint64_t plugged;
};

static int find_node(void *data, struct node *node)
{
	struct find_data *find = data;
	struct impl *impl = find->impl;
	int priority = 0;
	uint64_t plugged = 0;
	struct sm_device *device = node->obj->device;
	bool is_default = false, can_passthrough = false;

	if (node->obj->info == NULL) {
		pw_log_debug("%p: skipping node '%d' with no node info", impl, node->id);
		return 0;
	}

	pw_log_debug("%p: looking at node '%d' enabled:%d state:%d peer:%p exclusive:%d",
			impl, node->id, node->enabled, node->obj->info->state, node->peer, node->exclusive);

	if (!node->enabled || node->type == NODE_TYPE_UNKNOWN)
		return 0;

	if (device && device->locked) {
		pw_log_debug(".. device locked");
		return 0;
	}

	if (node->media && !spa_streq(node->media, find->media)) {
		pw_log_debug(".. incompatible media %s <-> %s", node->media, find->media);
		return 0;
	}
	if (find->link_group && !can_link_check(impl, find->link_group, node, 0)) {
		pw_log_debug(".. connecting link-group %s", find->link_group);
		return 0;
	}

	plugged = node->plugged;
	priority = node->priority;

	if (node->media) {
		if (spa_streq(node->media, "Audio")) {
			if (node->direction == PW_DIRECTION_INPUT) {
				if (find->direction == PW_DIRECTION_OUTPUT)
					is_default |= check_node_name(node,
						impl->defaults[DEFAULT_AUDIO_SINK].config);
				else if (find->direction == PW_DIRECTION_INPUT)
					is_default |= check_node_name(node,
						impl->defaults[DEFAULT_AUDIO_SOURCE].config);
			} else if (node->direction == PW_DIRECTION_OUTPUT &&
			    find->direction == PW_DIRECTION_INPUT)
				is_default |= check_node_name(node,
						impl->defaults[DEFAULT_AUDIO_SOURCE].config);
		} else if (spa_streq(node->media, "Video")) {
			if (node->direction == PW_DIRECTION_OUTPUT &&
			    find->direction == PW_DIRECTION_INPUT)
				is_default |= check_node_name(node,
						impl->defaults[DEFAULT_VIDEO_SOURCE].config);
		}
		if (is_default)
			priority += 10000;
	}
	if (device != NULL && !is_default && !have_available_route(node, device)) {
		pw_log_debug(".. no available routes");
		return 0;
	}

	if ((find->capture_sink && node->direction != PW_DIRECTION_INPUT) ||
	    (!find->capture_sink && !is_default && node->direction == find->direction)) {
		pw_log_debug(".. same direction");
		return 0;
	}
	if ((find->exclusive && node->obj->info->state == PW_NODE_STATE_RUNNING) ||
	    (node->peer && node->peer->exclusive)) {
		pw_log_debug("%p: node '%d' in use", impl, node->id);
		return 0;
	}
	if (find->node && find->have_passthrough && node->have_passthrough)
		can_passthrough = check_passthrough(find->node, node);

	if ((find->passthrough_only || node->passthrough_only) &&
	    !can_passthrough) {
		pw_log_debug("%p: node '%d' passthrough required", impl, node->id);
		return 0;
	}

	pw_log_debug("%p: found node '%d' %"PRIu64" prio:%d", impl,
			node->id, plugged, priority);

	if (find->result == NULL ||
	    priority > find->priority ||
	    (priority == find->priority && plugged > find->plugged)) {
		pw_log_debug("%p: new best %d %" PRIu64, impl, priority, plugged);
		find->result = node;
		find->priority = priority;
		find->plugged = plugged;
		find->can_passthrough = can_passthrough;
	}
	return 0;
}

static struct node *find_auto_default_node(struct impl *impl, const struct default_node *def)
{
	struct node *node;
	struct find_data find;

	spa_zero(find);
	find.impl = impl;
	find.capture_sink = false;
	find.exclusive = false;

	if (spa_streq(def->key, DEFAULT_AUDIO_SINK_KEY)) {
		find.media = "Audio";
		find.direction = PW_DIRECTION_OUTPUT;
	} else if (spa_streq(def->key, DEFAULT_AUDIO_SOURCE_KEY)) {
		find.media = "Audio";
		find.direction = PW_DIRECTION_INPUT;
	} else if (spa_streq(def->key, DEFAULT_VIDEO_SOURCE_KEY)) {
		find.media = "Video";
		find.direction = PW_DIRECTION_INPUT;
	} else {
		return NULL;
	}

	spa_list_for_each(node, &impl->node_list, link)
		find_node(&find, node);

	return find.result;
}

static int link_nodes(struct node *node, struct node *peer)
{
	struct impl *impl = node->impl;
	struct pw_properties *props;
	struct node *output, *input;
	int res;
	uint32_t node_id = node->id;

	pw_log_debug("%p: link nodes %d %d remix:%d", impl,
			node->id, peer->id, !node->dont_remix);

	if (node->want_passthrough) {
		configure_passthrough(node);
		configure_passthrough(peer);
	} else {
		if (node->dont_remix || peer->unpositioned)
			configure_node(node, NULL, peer->unpositioned);
		else
			configure_node(node, &peer->format, true);
	}

	if (node->direction == PW_DIRECTION_INPUT) {
		output = peer;
		input = node;
	} else {
		output = node;
		input = peer;
	}
	props = pw_properties_new(NULL, NULL);
	pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%d", output->id);
	pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%d", input->id);
	pw_log_info("linking node %d to node %d", output->id, input->id);

	node->linking = true;
	res = sm_media_session_create_links(impl->session, &props->dict);
	pw_properties_free(props);

	if (impl->linking_node_removed) {
		impl->linking_node_removed = false;
		pw_log_info("linking node %d was removed", node_id);
		return -ENOENT;
	}
	node->linking = false;

	pw_log_info("linked %d to %p", res, peer);
	if (res > 0) {
		node->peer = peer;
		node->connect_count++;
	}
	return res;
}

static int unlink_nodes(struct node *node, struct node *peer)
{
	struct impl *impl = node->impl;
	struct pw_properties *props;

	pw_log_debug("%p: unlink nodes %d %d", impl, node->id, peer->id);

	if (peer->peer == node)
		peer->peer = NULL;
	node->peer = NULL;

	if (node->direction == PW_DIRECTION_INPUT) {
		struct node *t = node;
		node = peer;
		peer = t;
	}
	props = pw_properties_new(NULL, NULL);
	pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%d", node->id);
	pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%d", peer->id);
	pw_log_info("unlinking node %d from peer node %d", node->id, peer->id);

	sm_media_session_remove_links(impl->session, &props->dict);

	pw_properties_free(props);

	return 0;
}

static int relink_node(struct impl *impl, struct node *n, struct node *peer)
{
	int res;

	if (peer == n->failed_peer && n->failed_count > MAX_LINK_RETRY) {
		/* Break rescan -> failed link -> rescan loop. */
		pw_log_debug("%p: tried to link '%d' on last rescan, not retrying",
				impl, peer->id);
		return -EBUSY;
	}

	if (n->failed_peer != peer)
		n->failed_count = 0;
	n->failed_peer = peer;
	n->failed_count++;

	if (!can_link(impl, n, peer)) {
		pw_log_debug("can't link node %d to %d: same link-group", n->id,
				peer->id);
		return -EPERM;
	}

	if (n->peer != NULL)
		if ((res = unlink_nodes(n, n->peer)) < 0)
			return res;

	pw_log_debug("%p: linking node %d to node %d", impl, n->id, peer->id);

	/* NB. if link_nodes returns error, n may have been invalidated */
	if ((res = link_nodes(n, peer)) > 0) {
		n->failed_peer = NULL;
		n->failed_count = 0;
	}
	return res;
}

static int rescan_node(struct impl *impl, struct node *n)
{
	struct spa_dict *props;
	const char *str;
	bool reconnect, autoconnect, can_passthrough = false;
	struct pw_node_info *info;
	struct node *peer;
	struct sm_object *obj;
	uint32_t path_id;

	if (!n->active) {
		pw_log_debug("%p: node %d is not active", impl, n->id);
		return 0;
	}

	if (n->type == NODE_TYPE_DEVICE) {
		configure_node(n, NULL, false);
		return 0;
	}

	if (n->obj->info == NULL || n->obj->info->props == NULL) {
		pw_log_debug("%p: node %d has no properties", impl, n->id);
		return 0;
	}

	info = n->obj->info;
	props = info->props;

	str = spa_dict_lookup(props, PW_KEY_NODE_DONT_RECONNECT);
	reconnect = str ? !pw_properties_parse_bool(str) : true;

	if ((str = spa_dict_lookup(props, PW_KEY_STREAM_DONT_REMIX)) != NULL)
		n->dont_remix = pw_properties_parse_bool(str);

	if ((str = spa_dict_lookup(props, PW_KEY_STREAM_MONITOR)) != NULL)
		n->monitor = pw_properties_parse_bool(str);

	if (n->direction == PW_DIRECTION_INPUT &&
	    (str = spa_dict_lookup(props, PW_KEY_STREAM_CAPTURE_SINK)) != NULL)
		n->capture_sink = pw_properties_parse_bool(str);

	autoconnect = false;
	if ((str = spa_dict_lookup(props, PW_KEY_NODE_AUTOCONNECT)) != NULL)
		autoconnect = pw_properties_parse_bool(str);

	if ((str = spa_dict_lookup(props, PW_KEY_DEVICE_API)) != NULL &&
	    spa_streq(str, "bluez5"))
		autoconnect = true;

	if (!autoconnect) {
		pw_log_debug("%p: node %d does not need autoconnect", impl, n->id);
		configure_node(n, NULL, false);
		return 0;
	}

	if (n->media == NULL) {
		pw_log_debug("%p: node %d has unknown media", impl, n->id);
		return 0;
	}

	pw_log_debug("%p: exclusive:%d", impl, n->exclusive);

	/* honor target node set by user or asked for by the client */
	path_id = SPA_ID_INVALID;
	if (n->obj->target_node != NULL)
		path_id = find_device_for_name(impl, n->obj->target_node);
	if (!n->obj->fixed_target &&
			(str = spa_dict_lookup(props, PW_KEY_NODE_TARGET)) != NULL) {
		bool has_target = ((uint32_t)atoi(str) != SPA_ID_INVALID);
		path_id = find_device_for_name(impl, str);
		if (!reconnect && has_target && path_id == SPA_ID_INVALID) {
			/* don't use fallbacks for non-reconnecting nodes */
			peer = NULL;
			goto do_link;
		}
	}

	if (n->peer != NULL) {
		/* Do we need to check again where to link to? */
		bool target_found = (path_id != SPA_ID_INVALID);
		bool peer_is_target = (target_found && n->peer->obj->obj.id == path_id);
		bool follows_default = (impl->streams_follow_default &&
				n->type == NODE_TYPE_STREAM);
		bool recheck = !peer_is_target && (follows_default || target_found) &&
			reconnect && !n->passthrough;
		if (!recheck) {
			pw_log_debug("%p: node %d is already linked, peer-is-target:%d "
					"follows-default:%d", impl, n->id, peer_is_target,
					follows_default);
			return 0;
		}
	}

	pw_log_info("trying to link node %d exclusive:%d reconnect:%d target:%d, peer %p",
			n->id, n->exclusive, reconnect, path_id, n->peer);

	if (path_id != SPA_ID_INVALID) {
		pw_log_debug("%p: target:%d", impl, path_id);

		if (!reconnect)
			n->obj->target_node = NULL;

		if ((obj = sm_media_session_find_object(impl->session, path_id)) == NULL) {
			pw_log_warn("node %d target:%d not found, find fallback:%d", n->id,
					path_id, reconnect);
			path_id = SPA_ID_INVALID;
			goto fallback;
		}
		path_id = SPA_ID_INVALID;

		if (!spa_streq(obj->type, PW_TYPE_INTERFACE_Node))
			goto fallback;

		peer = sm_object_get_data(obj, SESSION_KEY);
		pw_log_debug("%p: found target:%d type:%s %d:%d", impl,
				peer->id, obj->type, n->passthrough_only, peer->have_passthrough);

		can_passthrough = check_passthrough(n, peer);
		if (n->passthrough_only && !can_passthrough) {
			pw_log_info("%p: peer has no passthrough", impl);
			goto fallback;
		}

		goto do_link;
	}
fallback:
	if (path_id == SPA_ID_INVALID && (reconnect || n->connect_count == 0)) {
		/* find fallback */
		struct find_data find;

		spa_zero(find);
		find.impl = impl;
		find.node = n;
		find.media = n->media;
		find.capture_sink = n->capture_sink;
		find.direction = n->direction;
		find.exclusive = n->exclusive;
		find.have_passthrough = n->have_passthrough;
		find.passthrough_only = n->passthrough_only;
		find.link_group = n->peer == NULL ? spa_dict_lookup(props, PW_KEY_NODE_LINK_GROUP) : NULL;

		spa_list_for_each(peer, &impl->node_list, link)
			find_node(&find, peer);

		peer = find.result;
		if (peer)
			can_passthrough = find.can_passthrough;

		if (n->passthrough_only && !can_passthrough)
			peer = NULL;
	} else {
		peer = NULL;
	}

do_link:
	if (peer == NULL) {
		if (!reconnect) {
			pw_log_info("don-reconnect target node destroyed: destroy %d", n->id);
			sm_media_session_destroy_object(impl->session, n->id);
		} else if (reconnect && n->connect_count > 0) {
			/* Don't error the stream on reconnects */
			pw_log_info("%p: no node found for %d, waiting reconnect", impl, n->id);
			if (n->peer != NULL)
				unlink_nodes(n, n->peer);
			return 0;
		} else {
			pw_log_warn("%p: no node found for %d, stream error", impl, n->id);
		}

		obj = sm_media_session_find_object(impl->session, n->client_id);
		pw_log_debug("%p: client_id:%d object:%p type:%s", impl,
				n->client_id, obj, obj ? obj->type : "None");

		if (obj && spa_streq(obj->type, PW_TYPE_INTERFACE_Client)) {
			pw_client_error((struct pw_client*)obj->proxy,
				n->id, -ENOENT, "no node available");
		}
		return -ENOENT;
	} else if (peer == n->peer) {
		pw_log_debug("%p: node %d already linked to %d (not changing)",
				impl, n->id, peer->id);
		return 0;
	} else {
		n->want_passthrough = can_passthrough;
	}

	if ((n->exclusive || n->want_passthrough) &&
			peer->obj->info->state == PW_NODE_STATE_RUNNING) {
		pw_log_warn("node %d busy, can't get exclusive/passthrough access", peer->id);
		return -EBUSY;
	}

	return relink_node(impl, n, peer);
}

static void session_info(void *data, const struct pw_core_info *info)
{
	struct impl *impl = data;

	if (info && (info->change_mask & PW_CORE_CHANGE_MASK_PROPS)) {
		const char *str;

		if ((str = spa_dict_lookup(info->props, "default.clock.rate")) != NULL)
			impl->sample_rate = atoi(str);

		pw_log_debug("%p: props changed sample_rate:%d", impl, impl->sample_rate);
	}
}

static void refresh_auto_default_nodes(struct impl *impl)
{
	struct default_node *def;

	if (impl->session->metadata == NULL)
		return;

	pw_log_debug("%p: refresh", impl);

	/* Auto set default nodes */
	for (def = impl->defaults; def->key != NULL; ++def) {
		struct node *node;
		node = find_auto_default_node(impl, def);
		if (node == NULL && def->value != NULL) {
			def->value = NULL;
			pw_metadata_set_property(impl->session->metadata,
					PW_ID_CORE, def->key, NULL, NULL);
		} else if (node != NULL) {
			const char *name = pw_properties_get(node->obj->obj.props, PW_KEY_NODE_NAME);
			char buf[1024];

			if (name == NULL || spa_streq(name, def->value))
				continue;

			free(def->value);
			def->value = strdup(name);

			snprintf(buf, sizeof(buf), "{ \"name\": \"%s\" }", name);
			pw_metadata_set_property(impl->session->metadata,
					PW_ID_CORE, def->key,
					"Spa:String:JSON", buf);
		}
	}
}

static void session_rescan(void *data, int seq)
{
	struct impl *impl = data;
	struct node *node;

	pw_log_debug("%p: rescan", impl);

again:
	impl->node_list_changed = false;
	spa_list_for_each(node, &impl->node_list, link) {
		rescan_node(impl, node);
		if (impl->node_list_changed)
			goto again;
	}

	refresh_auto_default_nodes(impl);
}

static void session_destroy(void *data)
{
	struct impl *impl = data;
	struct default_node *def;
	for (def = impl->defaults; def->key != NULL; ++def) {
		free(def->config);
		free(def->value);
	}
	spa_hook_remove(&impl->listener);
	if (impl->session->metadata)
		spa_hook_remove(&impl->meta_listener);
	free(impl);
}

static const struct sm_media_session_events session_events = {
	SM_VERSION_MEDIA_SESSION_EVENTS,
	.info = session_info,
	.create = session_create,
	.remove = session_remove,
	.rescan = session_rescan,
	.destroy = session_destroy,
};

static int metadata_property(void *object, uint32_t subject,
		const char *key, const char *type, const char *value)
{
	struct impl *impl = object;

	if (subject == PW_ID_CORE) {
		struct default_node *def;
		bool changed = false;
		char *val = NULL;
		char name[1024];

		if (key != NULL && value != NULL) {
			pw_log_info("meta %s: %s", key, value);
			if (json_object_find(value, "name", name, sizeof(name)) < 0)
				return 0;
			pw_log_info("meta name: %s", name);
			val = name;
		}
		for (def = impl->defaults; def->key != NULL; ++def) {
			if (key == NULL || spa_streq(key, def->key_config)) {
				if (!spa_streq(def->config, val))
					changed = true;
				free(def->config);
				def->config = val ? strdup(val) : NULL;
			}
			if (key == NULL || spa_streq(key, def->key)) {
				bool eff_changed = !spa_streq(def->value, val);
				free(def->value);
				def->value = val ? strdup(val) : NULL;

				/* The effective value was changed. In case it was changed by
				 * someone else than us, reset the value to avoid confusion. */
				if (eff_changed)
					refresh_auto_default_nodes(impl);
			}
		}
		if (changed)
			sm_media_session_schedule_rescan(impl->session);
	} else if (key == NULL || spa_streq(key, "target.node")) {
		struct node *src_node;

		src_node = find_node_by_id_name(impl, subject, NULL);
		if (!src_node)
			return 0;

		/* Set target and schedule rescan */
		if (key == NULL || value == NULL) {
			free(src_node->obj->target_node);
			src_node->obj->target_node = NULL;
			src_node->obj->fixed_target = false;
		} else {
			const char *str;
			struct node *dst_node;

			dst_node = find_node_by_id_name(impl, SPA_ID_INVALID, value);
			if (dst_node) {
				str = get_device_name(dst_node);
				if (!str)
					return 0;
			} else if ((uint32_t)atoi(value) == SPA_ID_INVALID) {
				str = NULL;
			} else {
				return 0;
			}

			free(src_node->obj->target_node);
			src_node->obj->target_node = str ? strdup(str) : NULL;
			src_node->obj->fixed_target = true;
		}

		sm_media_session_schedule_rescan(impl->session);
	}
	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_property,
};

int sm_policy_node_start(struct sm_media_session *session)
{
	struct impl *impl;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->session = session;
	impl->context = session->context;

	impl->sample_rate = 48000;

	impl->defaults[DEFAULT_AUDIO_SINK] = (struct default_node){
		DEFAULT_AUDIO_SINK_KEY, DEFAULT_CONFIG_AUDIO_SINK_KEY, NULL, NULL
	};
	impl->defaults[DEFAULT_AUDIO_SOURCE] = (struct default_node){
		DEFAULT_AUDIO_SOURCE_KEY, DEFAULT_CONFIG_AUDIO_SOURCE_KEY, NULL, NULL
	};
	impl->defaults[DEFAULT_VIDEO_SOURCE] = (struct default_node){
		DEFAULT_VIDEO_SOURCE_KEY, DEFAULT_CONFIG_VIDEO_SOURCE_KEY, NULL, NULL
	};
	impl->defaults[3] = (struct default_node){ NULL, NULL, NULL, NULL };


	impl->streams_follow_default = pw_properties_get_bool(session->props, NAME ".streams-follow-default", false);
	impl->alsa_no_dsp = pw_properties_get_bool(session->props, NAME ".alsa-no-dsp", false);

	spa_list_init(&impl->node_list);

	sm_media_session_add_listener(impl->session,
			&impl->listener,
			&session_events, impl);

	if (session->metadata) {
		pw_metadata_add_listener(session->metadata,
				&impl->meta_listener,
				&metadata_events, impl);
	}
	return 0;
}
