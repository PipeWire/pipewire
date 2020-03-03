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

#include "media-session.h"

#define NAME		"policy-node"
#define SESSION_KEY	"policy-node"

#define DEFAULT_IDLE_SECONDS	3

struct impl {
	struct timespec now;

	struct sm_media_session *session;
	struct spa_hook listener;

	struct pw_context *context;

	uint32_t sample_rate;

	struct spa_list node_list;
	int seq;
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

	uint64_t plugged;
	unsigned int active:1;
	unsigned int exclusive:1;
	unsigned int enabled:1;
};

static int activate_node(struct node *node)
{
	struct impl *impl = node->impl;
	struct sm_param *p;
	char buf[1024];
	struct spa_pod_builder b = { 0, };
	struct spa_pod *param;
	bool have_format = false;

	pw_log_debug(NAME" %p: node %p activate", impl, node);

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

		if (spa_format_audio_raw_parse(p->param, &info.info.raw) < 0)
			continue;

		if (node->format.info.raw.channels < info.info.raw.channels)
			node->format = info;

		have_format = true;
	}

	if (have_format) {
		node->format.info.raw.rate = impl->sample_rate;

		spa_pod_builder_init(&b, buf, sizeof(buf));
		param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &node->format.info.raw);
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
	}

	node->active = true;
	return 0;
}

static void object_update(void *data)
{
	struct node *node = data;
	struct impl *impl = node->impl;

	pw_log_debug(NAME" %p: node %p %08x", impl, node, node->obj->obj.changed);

	if (node->obj->obj.avail & SM_NODE_CHANGE_MASK_PARAMS &&
	    !node->active)
		activate_node(node);
}

static const struct sm_object_events object_events = {
	SM_VERSION_OBJECT_EVENTS,
	.update = object_update
};

static int
handle_node(struct impl *impl, struct sm_object *object)
{
	const char *str, *media_class;
	enum pw_direction direction;
	struct node *node;
	uint32_t client_id = SPA_ID_INVALID;

	if (object->props) {
		if ((str = pw_properties_get(object->props, PW_KEY_CLIENT_ID)) != NULL)
			client_id = atoi(str);
	}

	media_class = object->props ? pw_properties_get(object->props, PW_KEY_MEDIA_CLASS) : NULL;

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
			node->active = true;
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
			node->active = true;
		}
		else
			return 0;

		if (strcmp(media_class, "Sink") == 0)
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
	if (node->peer)
		node->peer->peer = NULL;
	sm_object_remove_data((struct sm_object*)node->obj, SESSION_KEY);
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
	}
	else
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

	if (node->direction == find->target->direction) {
		pw_log_debug(".. same direction");
		return 0;
	}
	if (strcmp(node->media, find->target->media) != 0) {
		pw_log_debug(".. incompatible media %s <-> %s", node->media, find->target->media);
		return 0;
	}

	plugged = node->plugged;
	priority = node->priority;

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

	pw_log_debug(NAME " %p: link nodes %d %d", impl, node->id, peer->id);

	peer->peer = node;
	node->peer = peer;

	if (node->direction == PW_DIRECTION_INPUT) {
		struct node *t = node;
		node = peer;
		peer = t;
	}
	props = pw_properties_new(NULL, NULL);
	pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%d", node->id);
	pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%d", peer->id);
	pw_log_debug(NAME " %p: node %d -> node %d", impl,
			node->id, peer->id);

	sm_media_session_create_links(impl->session, &props->dict);

	pw_properties_free(props);

	return 0;
}

static int rescan_node(struct impl *impl, struct node *n)
{
	struct spa_dict *props;
        const char *str;
        bool exclusive;
        struct find_data find;
	struct pw_node_info *info;
	struct node *peer;
	struct sm_object *obj;

	if (n->type == NODE_TYPE_DEVICE)
		return 0;

	if (!n->active) {
		pw_log_debug(NAME " %p: node %d is not active", impl, n->id);
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

        str = spa_dict_lookup(props, PW_KEY_NODE_AUTOCONNECT);
        if (str == NULL || !pw_properties_parse_bool(str)) {
		pw_log_debug(NAME" %p: node %d does not need autoconnect", impl, n->id);
                return 0;
	}

	if (n->media == NULL) {
		pw_log_debug(NAME" %p: node %d has unknown media", impl, n->id);
		return 0;
	}

	spa_zero(find);

	if ((str = spa_dict_lookup(props, PW_KEY_NODE_EXCLUSIVE)) != NULL)
		exclusive = pw_properties_parse_bool(str);
	else
		exclusive = false;

	find.impl = impl;
	find.target = n;
	find.exclusive = exclusive;

	pw_log_info(NAME " %p: exclusive:%d", impl, exclusive);

	str = spa_dict_lookup(props, PW_KEY_NODE_TARGET);
	if (str != NULL) {
		uint32_t path_id = atoi(str);
		pw_log_info(NAME " %p: target:%d", impl, path_id);

		if ((obj = sm_media_session_find_object(impl->session, path_id)) != NULL) {
			if (strcmp(obj->type, PW_TYPE_INTERFACE_Node) == 0) {
				peer = sm_object_get_data(obj, SESSION_KEY);
				if (peer == NULL)
					return -ENOENT;
				goto do_link;
			}
		}
	}

	spa_list_for_each(peer, &impl->node_list, link)
		find_node(&find, peer);

	if (find.node == NULL) {
		struct sm_object *obj;

		pw_log_warn(NAME " %p: no node found for %d", impl, n->id);

		str = spa_dict_lookup(props, PW_KEY_NODE_DONT_RECONNECT);
		if (str != NULL && pw_properties_parse_bool(str)) {
//			pw_registry_destroy(impl->registry, n->id);
		}

		obj = sm_media_session_find_object(impl->session, n->client_id);
		if (obj && strcmp(obj->type, PW_TYPE_INTERFACE_Client) == 0) {
			pw_client_error((struct pw_client*)obj->proxy,
				n->id, -ENOENT, "no node available");
		}
		return -ENOENT;
	}
	peer = find.node;

	if (exclusive && peer->obj->info->state == PW_NODE_STATE_RUNNING) {
		pw_log_warn(NAME" %p: node %d busy, can't get exclusive access", impl, peer->id);
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

int sm_policy_node_start(struct sm_media_session *session)
{
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->session = session;
	impl->context = session->context;

	impl->sample_rate = 48000;

	spa_list_init(&impl->node_list);

	sm_media_session_add_listener(impl->session, &impl->listener, &session_events, impl);

	return 0;
}
