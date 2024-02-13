/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2018 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include "config.h"

#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/utils/hook.h>
#include <spa/utils/result.h>
#include <spa/utils/names.h>
#include <spa/utils/string.h>
#include <spa/utils/type-info.h>
#include <spa/param/format.h>
#include <spa/param/audio/format.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/format-utils.h>
#include <spa/debug/types.h>
#include <spa/utils/json-pod.h>

#include "pipewire/pipewire.h"

#include "modules/spa/spa-node.h"

#define NAME "adapter"

PW_LOG_TOPIC_EXTERN(mod_topic);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct buffer {
	struct spa_buffer buf;
	struct spa_data datas[1];
	struct spa_chunk chunk[1];
};

struct node {
	struct pw_context *context;

	struct pw_impl_node *node;
	struct spa_hook node_listener;

	struct spa_node *follower;

	void *user_data;
	enum pw_direction direction;
	struct pw_properties *props;

	uint32_t media_type;
	uint32_t media_subtype;

	struct spa_list ports;
};

/** \endcond */
static void node_free(void *data)
{
	struct node *n = data;
	spa_hook_remove(&n->node_listener);
	pw_properties_free(n->props);
}

static const struct pw_impl_node_events node_events = {
	PW_VERSION_IMPL_NODE_EVENTS,
	.free = node_free,
};

static int find_format(struct spa_node *node, enum pw_direction direction,
		uint32_t *media_type, uint32_t *media_subtype)
{
	uint32_t state = 0;
	uint8_t buffer[4096];
	struct spa_pod_builder b;
	int res;
	struct spa_pod *format;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	if ((res = spa_node_port_enum_params_sync(node,
				direction == PW_DIRECTION_INPUT ?
					SPA_DIRECTION_INPUT :
					SPA_DIRECTION_OUTPUT, 0,
				SPA_PARAM_EnumFormat, &state,
				NULL, &format, &b)) != 1) {
		res = res < 0 ? res : -ENOENT;
		pw_log_warn("%p: can't get format: %s", node, spa_strerror(res));
		return res;
	}

	if ((res = spa_format_parse(format, media_type, media_subtype)) < 0)
		return res;

	pw_log_debug("%p: %s/%s", node,
			spa_debug_type_find_name(spa_type_media_type, *media_type),
			spa_debug_type_find_name(spa_type_media_subtype, *media_subtype));
	return 0;
}

struct info_data {
	struct spa_hook listener;
	struct spa_node *node;
	struct pw_properties *props;
	uint32_t n_input_ports;
	uint32_t max_input_ports;
	uint32_t n_output_ports;
	uint32_t max_output_ports;
};

static void info_event(void *data, const struct spa_node_info *info)
{
	struct info_data *d = data;

	pw_properties_update(d->props, info->props);

	d->max_input_ports = info->max_input_ports;
	d->max_output_ports = info->max_output_ports;
}

static void port_info_event(void *data, enum spa_direction direction, uint32_t port,
		const struct spa_port_info *info)
{
	struct info_data *d = data;

	if (direction == SPA_DIRECTION_OUTPUT)
		d->n_output_ports++;
	else if (direction == SPA_DIRECTION_INPUT)
		d->n_input_ports++;
}

static const struct spa_node_events node_info_events = {
	.version = SPA_VERSION_NODE_EVENTS,
	.info = info_event,
	.port_info = port_info_event,
};

struct pw_impl_node *pw_adapter_new(struct pw_context *context,
		struct spa_node *follower,
		struct pw_properties *props,
		size_t user_data_size)
{
	struct pw_impl_node *node;
	struct node *n;
	const char *str, *factory_name;
	enum pw_direction direction;
	int res;
	uint32_t media_type, media_subtype;
	struct info_data info;

	spa_zero(info);
	info.node = follower;
	info.props = props;

	res = spa_node_add_listener(info.node, &info.listener, &node_info_events, &info);
	if (res < 0)
		goto error;

	spa_hook_remove(&info.listener);

	pw_log_debug("%p: in %d/%d out %d/%d", info.node,
			info.n_input_ports, info.max_input_ports,
			info.n_output_ports, info.max_output_ports);

	if (info.n_output_ports > 0) {
		direction = PW_DIRECTION_OUTPUT;
	} else if (info.n_input_ports > 0) {
		direction = PW_DIRECTION_INPUT;
	} else {
		res = -EINVAL;
		goto error;
	}

	if ((str = pw_properties_get(props, PW_KEY_NODE_ID)) != NULL)
		pw_properties_set(props, PW_KEY_NODE_SESSION, str);

	if (pw_properties_get(props, "factory.mode") == NULL) {
		if (direction == PW_DIRECTION_INPUT)
			str = "merge";
		else
			str = "split";
		pw_properties_set(props, "factory.mode", str);
	}

	if ((res = find_format(follower, direction, &media_type, &media_subtype)) < 0)
		goto error;

	if (media_type == SPA_MEDIA_TYPE_audio) {
		pw_properties_setf(props, "audio.adapt.follower", "pointer:%p", follower);
		pw_properties_set(props, SPA_KEY_LIBRARY_NAME, "audioconvert/libspa-audioconvert");
		if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
			pw_properties_setf(props, PW_KEY_MEDIA_CLASS, "Audio/%s",
				direction == PW_DIRECTION_INPUT ? "Sink" : "Source");
		factory_name = SPA_NAME_AUDIO_ADAPT;
	}
	else if (media_type == SPA_MEDIA_TYPE_video) {
		pw_properties_setf(props, "video.adapt.follower", "pointer:%p", follower);
		pw_properties_set(props, SPA_KEY_LIBRARY_NAME, "videoconvert/libspa-videoconvert");
		if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
			pw_properties_setf(props, PW_KEY_MEDIA_CLASS, "Video/%s",
				direction == PW_DIRECTION_INPUT ? "Sink" : "Source");
		factory_name = SPA_NAME_VIDEO_ADAPT;
	} else {
		res = -ENOTSUP;
		goto error;
	}

	node = pw_spa_node_load(context,
				factory_name,
				PW_SPA_NODE_FLAG_ACTIVATE | PW_SPA_NODE_FLAG_NO_REGISTER,
				pw_properties_copy(props), sizeof(struct node) + user_data_size);
        if (node == NULL) {
		res = -errno;
		pw_log_error("can't load spa node: %m");
		goto error;
	}

	n = pw_spa_node_get_user_data(node);
	n->context = context;
	n->node = node;
	n->follower = follower;
	n->direction = direction;
	n->props = props;
	n->media_type = media_type;
	n->media_subtype = media_subtype;
	spa_list_init(&n->ports);

	if (user_data_size > 0)
		n->user_data = SPA_PTROFF(n, sizeof(struct node), void);

	pw_impl_node_add_listener(node, &n->node_listener, &node_events, n);

	return node;

error:
	pw_properties_free(props);
	errno = -res;
	return NULL;
}

void *pw_adapter_get_user_data(struct pw_impl_node *node)
{
	struct node *n = pw_spa_node_get_user_data(node);
	return n->user_data;
}
