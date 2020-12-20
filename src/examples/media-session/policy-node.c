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
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/pod.h>

#include "pipewire/pipewire.h"
#include "extensions/metadata.h"

#include "media-session.h"

#define NAME		"policy-node"
#define SESSION_KEY	"policy-node"

#define DEFAULT_IDLE_SECONDS	3

struct impl {
	struct timespec now;

	struct sm_media_session *session;
	struct spa_hook listener;

	struct spa_hook meta_listener;

	struct pw_context *context;

	uint32_t sample_rate;

	struct spa_list node_list;
	int seq;

	uint32_t default_audio_sink;
	uint32_t default_audio_source;
	uint32_t default_video_source;
};

struct node {
	struct sm_node *obj;

	uint32_t id;
	struct impl *impl;

	struct spa_list link;		/**< link in impl node_list */
	enum pw_direction direction;

	struct spa_hook listener;

	struct node *peer;

	uint32_t client_id;
	int32_t priority;

#define NODE_TYPE_UNKNOWN	0
#define NODE_TYPE_STREAM	1
#define NODE_TYPE_DEVICE	2
	uint32_t type;
	char *media;

	struct spa_audio_info format;

	int connect_count;
	uint64_t plugged;
	unsigned int active:1;
	unsigned int exclusive:1;
	unsigned int enabled:1;
	unsigned int configured:1;
	unsigned int dont_remix:1;
	unsigned int monitor:1;
	unsigned int moving:1;
	unsigned int capture_sink:1;
};

static bool find_format(struct node *node)
{
	struct impl *impl = node->impl;
	struct sm_param *p;
	bool have_format = false;

	spa_list_for_each(p, &node->obj->param_list, link) {
		struct spa_audio_info info = { 0, };

		if (p->id != SPA_PARAM_EnumFormat)
			continue;

		if (spa_format_parse(p->param, &info.media_type, &info.media_subtype) < 0)
			continue;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			continue;

		spa_pod_object_fixate((struct spa_pod_object*)p->param);
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_pod(2, NULL, p->param);

		/* defaults */
		info.info.raw.format = SPA_AUDIO_FORMAT_F32;
		info.info.raw.rate = impl->sample_rate;
		info.info.raw.channels = 2;
		info.info.raw.position[0] = SPA_AUDIO_CHANNEL_FL;
		info.info.raw.position[1] = SPA_AUDIO_CHANNEL_FR;

		spa_format_audio_raw_parse(p->param, &info.info.raw);

		if (node->format.info.raw.channels < info.info.raw.channels)
			node->format = info;

		have_format = true;
	}
	return have_format;
}

static int configure_node(struct node *node, struct spa_audio_info *info, bool force)
{
	struct impl *impl = node->impl;
	char buf[1024];
	struct spa_pod_builder b = { 0, };
	struct spa_pod *param;
	struct spa_audio_info format;

	if (node->configured && !force)
		return 0;

	if (strcmp(node->media, "Audio") != 0)
		return 0;

	format = node->format;

	if (info != NULL && info->info.raw.channels > 0) {
		pw_log_info("node %d monitor:%d channelmix %d->%d",
			node->id, node->monitor, format.info.raw.channels,
			info->info.raw.channels);
		format = *info;
	}
	format.info.raw.rate = impl->sample_rate;

	spa_pod_builder_init(&b, buf, sizeof(buf));
	param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &format.info.raw);
	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(node->direction),
		SPA_PARAM_PORT_CONFIG_mode,	 SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp),
		SPA_PARAM_PORT_CONFIG_monitor,   SPA_POD_Bool(true),
		SPA_PARAM_PORT_CONFIG_format,    SPA_POD_Pod(param));

	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_pod(2, NULL, param);

	pw_node_set_param((struct pw_node*)node->obj->obj.proxy,
			SPA_PARAM_PortConfig, 0, param);

	node->configured = true;

	return 0;
}

static void object_update(void *data)
{
	struct node *node = data;
	struct impl *impl = node->impl;

	pw_log_debug(NAME" %p: node %p %08x", impl, node, node->obj->obj.changed);

	if (node->obj->obj.avail & SM_NODE_CHANGE_MASK_PARAMS &&
	    !node->active) {
		if (!find_format(node)) {
			pw_log_debug(NAME" %p: can't find format %p", impl, node);
			return;
		}
		node->active = true;
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
	const char *str, *media_class = NULL, *role;
	enum pw_direction direction;
	struct node *node;
	uint32_t client_id = SPA_ID_INVALID;

	if (object->props) {
		if ((str = pw_properties_get(object->props, PW_KEY_CLIENT_ID)) != NULL)
			client_id = atoi(str);

		media_class = pw_properties_get(object->props, PW_KEY_MEDIA_CLASS);
		role = pw_properties_get(object->props, PW_KEY_MEDIA_ROLE);
	}

	pw_log_debug(NAME" %p: node "PW_KEY_MEDIA_CLASS" %s", impl, media_class);

	if (media_class == NULL)
		return 0;

	node = sm_object_add_data(object, SESSION_KEY, sizeof(struct node));
	node->obj = (struct sm_node*)object;
	node->id = object->id;
	node->impl = impl;
	node->client_id = client_id;
	node->type = NODE_TYPE_UNKNOWN;
	spa_list_append(&impl->node_list, &node->link);

	if (role && !strcmp(role, "DSP"))
		node->active = node->configured = true;

	if (strstr(media_class, "Stream/") == media_class) {
		media_class += strlen("Stream/");

		if (strstr(media_class, "Output/") == media_class) {
			direction = PW_DIRECTION_OUTPUT;
			media_class += strlen("Output/");
		}
		else if (strstr(media_class, "Input/") == media_class) {
			direction = PW_DIRECTION_INPUT;
			media_class += strlen("Input/");
		}
		else
			return 0;

		if (strstr(media_class, "Video") == media_class) {
			if (direction == PW_DIRECTION_OUTPUT) {
				if ((str = pw_properties_get(object->props, PW_KEY_NODE_PLUGGED)) != NULL)
					node->plugged = pw_properties_parse_uint64(str);
				else
					node->plugged = SPA_TIMESPEC_TO_NSEC(&impl->now);
			}
			node->active = node->configured = true;
		}
		else if (strstr(media_class, "Unknown") == media_class) {
			node->active = node->configured = true;
		}

		node->direction = direction;
		node->type = NODE_TYPE_STREAM;
		node->media = strdup(media_class);
		pw_log_debug(NAME" %p: node %d is stream %s", impl, object->id, node->media);
	}
	else {
		const char *media;
		if (strstr(media_class, "Audio/") == media_class) {
			media_class += strlen("Audio/");
			media = "Audio";
		}
		else if (strstr(media_class, "Video/") == media_class) {
			media_class += strlen("Video/");
			media = "Video";
			node->active = node->configured = true;
		}
		else
			return 0;

		if (strcmp(media_class, "Sink") == 0 ||
		    strcmp(media_class, "Duplex") == 0 ||
		    strcmp(media_class, "Source/Virtual") == 0)
			direction = PW_DIRECTION_INPUT;
		else if (strcmp(media_class, "Source") == 0)
			direction = PW_DIRECTION_OUTPUT;
		else
			return 0;

		if ((str = pw_properties_get(object->props, PW_KEY_NODE_PLUGGED)) != NULL)
			node->plugged = pw_properties_parse_uint64(str);
		else
			node->plugged = SPA_TIMESPEC_TO_NSEC(&impl->now);

		if ((str = pw_properties_get(object->props, PW_KEY_PRIORITY_SESSION)) != NULL)
			node->priority = pw_properties_parse_int(str);
		else
			node->priority = 0;

		node->direction = direction;
		node->type = NODE_TYPE_DEVICE;
		node->media = strdup(media);

		pw_log_debug(NAME" %p: node %d '%s' prio:%d", impl,
				object->id, node->media, node->priority);
	}

	node->enabled = true;
	node->obj->obj.mask |= SM_NODE_CHANGE_MASK_PARAMS;
	sm_object_add_listener(&node->obj->obj, &node->listener, &object_events, node);

	return 1;
}

static void destroy_node(struct impl *impl, struct node *node)
{
	spa_list_remove(&node->link);
	if (node->enabled)
		spa_hook_remove(&node->listener);
	free(node->media);
	if (node->peer && node->peer->peer == node)
		node->peer->peer = NULL;
	sm_object_remove_data((struct sm_object*)node->obj, SESSION_KEY);
}

static struct node *find_node_by_id(struct impl *impl, uint32_t id)
{
	struct node *node;
	spa_list_for_each(node, &impl->node_list, link) {
		if (node->id == id)
			return node;
	}
	return NULL;
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
		if (strcmp(str, name) == 0)
			return node->obj->obj.id;
	}
	return SPA_ID_INVALID;
}

static void session_create(void *data, struct sm_object *object)
{
	struct impl *impl = data;
	int res;

	clock_gettime(CLOCK_MONOTONIC, &impl->now);

	if (strcmp(object->type, PW_TYPE_INTERFACE_Node) == 0)
		res = handle_node(impl, object);
	else
		res = 0;

	if (res < 0) {
		pw_log_warn(NAME" %p: can't handle global %d", impl, object->id);
	} else
		sm_media_session_schedule_rescan(impl->session);
}

static void session_remove(void *data, struct sm_object *object)
{
	struct impl *impl = data;
	pw_log_debug(NAME " %p: remove global '%d'", impl, object->id);

	if (strcmp(object->type, PW_TYPE_INTERFACE_Node) == 0) {
		struct node *n, *node;

		if ((node = sm_object_get_data(object, SESSION_KEY)) != NULL)
			destroy_node(impl, node);

		spa_list_for_each(n, &impl->node_list, link) {
			if (n->peer == node)
				n->peer = NULL;
		}
		if (impl->default_audio_sink == object->id)
			impl->default_audio_sink = SPA_ID_INVALID;
		if (impl->default_audio_source == object->id)
			impl->default_audio_source = SPA_ID_INVALID;
		if (impl->default_video_source == object->id)
			impl->default_video_source = SPA_ID_INVALID;
	}

	sm_media_session_schedule_rescan(impl->session);
}

struct find_data {
	struct impl *impl;
	struct node *target;
	struct node *node;
	bool exclusive;
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

	pw_log_debug(NAME " %p: looking at node '%d' enabled:%d state:%d peer:%p exclusive:%d",
			impl, node->id, node->enabled, node->obj->info->state, node->peer, node->exclusive);

	if (!node->enabled || node->type == NODE_TYPE_UNKNOWN)
		return 0;

	if (device && device->locked) {
		pw_log_debug(".. device locked");
		return 0;
	}

	if ((find->target->capture_sink && node->direction != PW_DIRECTION_INPUT) ||
	    (!find->target->capture_sink && node->direction == find->target->direction)) {
		pw_log_debug(".. same direction");
		return 0;
	}
	if (strcmp(node->media, find->target->media) != 0) {
		pw_log_debug(".. incompatible media %s <-> %s", node->media, find->target->media);
		return 0;
	}
	plugged = node->plugged;
	priority = node->priority;

	if (node->media) {
		bool is_default = false;
		if (strcmp(node->media, "Audio") == 0) {
			if (node->direction == PW_DIRECTION_INPUT)
				is_default = impl->default_audio_sink == node->id;
			else if (node->direction == PW_DIRECTION_OUTPUT)
				is_default = impl->default_audio_source == node->id;
		} else if (strcmp(node->media, "Video") == 0) {
			if (node->direction == PW_DIRECTION_OUTPUT)
				is_default = impl->default_video_source == node->id;
		}
		if (is_default)
			priority += 10000;
	}

	if ((find->exclusive && node->obj->info->state == PW_NODE_STATE_RUNNING) ||
	    (node->peer && node->peer->exclusive)) {
		pw_log_debug(NAME " %p: node '%d' in use", impl, node->id);
		return 0;
	}

	pw_log_debug(NAME " %p: found node '%d' %"PRIu64" prio:%d", impl,
			node->id, plugged, priority);

	if (find->node == NULL ||
	    priority > find->priority ||
	    (priority == find->priority && plugged > find->plugged)) {
		pw_log_debug(NAME " %p: new best %d %" PRIu64, impl, priority, plugged);
		find->node = node;
		find->priority = priority;
		find->plugged = plugged;
	}
	return 0;
}

static int link_nodes(struct node *node, struct node *peer)
{
	struct impl *impl = node->impl;
	struct pw_properties *props;
	struct node *output, *input;

	pw_log_debug(NAME " %p: link nodes %d %d remix:%d", impl,
			node->id, peer->id, !node->dont_remix);

	if (node->dont_remix)
		configure_node(node, NULL, false);
	else {
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

	if (sm_media_session_create_links(impl->session, &props->dict) > 0) {
		node->peer = peer;
		node->connect_count++;
	}
	pw_properties_free(props);

	return 0;
}

static int unlink_nodes(struct node *node, struct node *peer)
{
	struct impl *impl = node->impl;
	struct pw_properties *props;

	pw_log_debug(NAME " %p: unlink nodes %d %d", impl, node->id, peer->id);

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

static int rescan_node(struct impl *impl, struct node *n)
{
	struct spa_dict *props;
	const char *str;
	bool exclusive, reconnect, autoconnect;
	struct find_data find;
	struct pw_node_info *info;
	struct node *peer;
	struct sm_object *obj;
	uint32_t path_id;

	if (!n->active) {
		pw_log_debug(NAME " %p: node %d is not active", impl, n->id);
		return 0;
	}
	if (n->moving) {
		pw_log_debug(NAME " %p: node %d is moving", impl, n->id);
		return 0;
	}

	if (n->type == NODE_TYPE_DEVICE) {
		configure_node(n, NULL, false);
		return 0;
	}

	if (n->obj->info == NULL || n->obj->info->props == NULL) {
		pw_log_debug(NAME " %p: node %d has no properties", impl, n->id);
		return 0;
	}

	if (n->peer != NULL) {
		pw_log_debug(NAME " %p: node %d is already linked", impl, n->id);
		return 0;
	}

	info = n->obj->info;
	props = info->props;

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
	    strcmp(str, "bluez5") == 0)
		autoconnect = true;

	if (!autoconnect) {
		pw_log_debug(NAME" %p: node %d does not need autoconnect", impl, n->id);
		configure_node(n, NULL, false);
		return 0;
	}

	if (n->media == NULL) {
		pw_log_debug(NAME" %p: node %d has unknown media", impl, n->id);
		return 0;
	}

	str = spa_dict_lookup(props, PW_KEY_NODE_EXCLUSIVE);
	exclusive = str ? pw_properties_parse_bool(str) : false;

	pw_log_debug(NAME " %p: exclusive:%d", impl, exclusive);

	spa_zero(find);
	find.impl = impl;
	find.target = n;
	find.exclusive = exclusive;

	str = spa_dict_lookup(props, PW_KEY_NODE_DONT_RECONNECT);
	reconnect = str ? !pw_properties_parse_bool(str) : true;

	/* we always honour the target node asked for by the client */
	path_id = SPA_ID_INVALID;
	if ((str = spa_dict_lookup(props, PW_KEY_NODE_TARGET)) != NULL)
		path_id = find_device_for_name(impl, str);
	if (path_id == SPA_ID_INVALID && n->obj->target_node != NULL)
		path_id = find_device_for_name(impl, n->obj->target_node);

	pw_log_info("trying to link node %d exclusive:%d reconnect:%d target:%d", n->id,
			exclusive, reconnect, path_id);

	if (path_id != SPA_ID_INVALID) {
		pw_log_debug(NAME " %p: target:%d", impl, path_id);

		if (!reconnect)
			n->obj->target_node = NULL;

		if ((obj = sm_media_session_find_object(impl->session, path_id)) != NULL) {
			pw_log_debug(NAME " %p: found target:%d type:%s", impl,
					path_id, obj->type);
			if (strcmp(obj->type, PW_TYPE_INTERFACE_Node) == 0) {
				peer = sm_object_get_data(obj, SESSION_KEY);
				if (peer == NULL)
					return -ENOENT;
				goto do_link;
			}
		}
		pw_log_warn("node %d target:%d not found, find fallback:%d", n->id,
				path_id, reconnect);
	}
	if (path_id == SPA_ID_INVALID && (reconnect || n->connect_count == 0)) {
		spa_list_for_each(peer, &impl->node_list, link)
			find_node(&find, peer);
	}

	if (find.node == NULL) {
		struct sm_object *obj;

		pw_log_warn("no node found for %d", n->id);

		if (!reconnect)
			sm_media_session_destroy_object(impl->session, n->id);

		obj = sm_media_session_find_object(impl->session, n->client_id);
		pw_log_debug(NAME " %p: client_id:%d object:%p type:%s", impl,
				n->client_id, obj, obj ? obj->type : "None");

		if (obj && strcmp(obj->type, PW_TYPE_INTERFACE_Client) == 0) {
			pw_client_error((struct pw_client*)obj->proxy,
				n->id, -ENOENT, "no node available");
		}
		return -ENOENT;
	}
	peer = find.node;

	if (exclusive && peer->obj->info->state == PW_NODE_STATE_RUNNING) {
		pw_log_warn("node %d busy, can't get exclusive access", peer->id);
		return -EBUSY;
	}
	n->exclusive = exclusive;

	pw_log_debug(NAME" %p: linking to node '%d'", impl, peer->id);

do_link:
	link_nodes(n, peer);
        return 1;
}

static void session_info(void *data, const struct pw_core_info *info)
{
	struct impl *impl = data;

	if (info && (info->change_mask & PW_CORE_CHANGE_MASK_PROPS)) {
		const char *str;

		if ((str = spa_dict_lookup(info->props, "default.clock.rate")) != NULL)
			impl->sample_rate = atoi(str);

		pw_log_debug(NAME" %p: props changed sample_rate:%d", impl, impl->sample_rate);
	}
}

static void session_rescan(void *data, int seq)
{
	struct impl *impl = data;
	struct node *node;

	pw_log_debug(NAME" %p: rescan", impl);

	spa_list_for_each(node, &impl->node_list, link)
		rescan_node(impl, node);
}

static void session_destroy(void *data)
{
	struct impl *impl = data;
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

static int do_move_node(struct node *n, struct node *src, struct node *dst)
{
	n->moving = true;
	if (src)
		unlink_nodes(n, src);
	if (dst)
		link_nodes(n, dst);
	n->moving = false;
	return 0;
}

static int move_node(struct impl *impl, uint32_t source, uint32_t target)
{
	struct node *n, *src_node, *dst_node;
	const char *str;

	if (source == SPA_ID_INVALID || target == SPA_ID_INVALID)
		return 0;

	/* find source and dest node */
	if ((src_node = find_node_by_id(impl, source)) == NULL)
		return -ENOENT;
	if ((dst_node = find_node_by_id(impl, target)) == NULL)
		return -ENOENT;

	if (src_node == dst_node)
		return 0;

	pw_log_info("move %d -> %d", src_node->id, dst_node->id);

	/* unlink all nodes from source and link to target */
	spa_list_for_each(n, &impl->node_list, link) {
		struct pw_node_info *info;

		if (n->peer != src_node)
			continue;

		if ((info = n->obj->info) == NULL)
			continue;

		if ((str = spa_dict_lookup(info->props, PW_KEY_NODE_DONT_RECONNECT)) != NULL &&
		    pw_properties_parse_bool(str))
			continue;

		do_move_node(n, src_node, dst_node);
	}
	return 0;
}

static int handle_move(struct impl *impl, struct node *src_node, struct node *dst_node)
{
	const char *str;
	struct pw_node_info *info;

	if (src_node->peer == dst_node)
		return 0;

	if ((info = src_node->obj->info) == NULL)
		return -EIO;

	if ((str = spa_dict_lookup(info->props, PW_KEY_NODE_DONT_RECONNECT)) != NULL &&
		    pw_properties_parse_bool(str)) {
		pw_log_warn("can't reconnect node %d to %d", src_node->id,
				dst_node->id);
		return -EPERM;
	}

	pw_log_info("move node %d: from peer %d to %d", src_node->id,
			src_node->peer ? src_node->peer->id : SPA_ID_INVALID,
			dst_node->id);

	free(src_node->obj->target_node);
	str = get_device_name(dst_node);
	src_node->obj->target_node = str ? strdup(str) : NULL;

	return do_move_node(src_node, src_node->peer, dst_node);
}

static int metadata_property(void *object, uint32_t subject,
		const char *key, const char *type, const char *value)
{
	struct impl *impl = object;
	uint32_t val = (key && value) ? (uint32_t)atoi(value) : SPA_ID_INVALID;

	if (subject == PW_ID_CORE) {
		if (key == NULL || strcmp(key, "default.audio.sink") == 0) {
			move_node(impl, impl->default_audio_sink, val);
			impl->default_audio_sink = val;
		}
		if (key == NULL || strcmp(key, "default.audio.source") == 0) {
			move_node(impl, impl->default_audio_source, val);
			impl->default_audio_source = val;
		}
		if (key == NULL || strcmp(key, "default.video.source") == 0) {
			move_node(impl, impl->default_video_source, val);
			impl->default_video_source = val;
		}
	} else {
		if (val != SPA_ID_INVALID && strcmp(key, "target.node") == 0) {
			struct node *src_node, *dst_node;

			dst_node = find_node_by_id(impl, val);
			src_node = dst_node ? find_node_by_id(impl, subject) : NULL;

			if (dst_node && src_node)
				handle_move(impl, src_node, dst_node);
		}
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

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->session = session;
	impl->context = session->context;

	impl->sample_rate = 48000;
	impl->default_audio_sink = SPA_ID_INVALID;
	impl->default_audio_source = SPA_ID_INVALID;
	impl->default_video_source = SPA_ID_INVALID;

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
