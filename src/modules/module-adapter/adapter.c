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

static int handle_node_param(struct pw_impl_node *node, const char *key, const char *value)
{
	const struct spa_type_info *ti;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_pod *pod;
	int res;

	ti = spa_debug_type_find_short(spa_type_param, key);
	if (ti == NULL)
		return -ENOENT;

	if ((res = spa_json_to_pod(&b, 0, ti, value, strlen(value))) < 0)
		return res;

	if ((pod = spa_pod_builder_deref(&b, 0)) == NULL)
		return -ENOSPC;

	if ((res = pw_impl_node_set_param(node, ti->type, 0, pod)) < 0)
		return res;

	return 0;
}

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

static int do_auto_port_config(struct node *n, const char *str)
{
	uint32_t state = 0, i;
	uint8_t buffer[4096];
	struct spa_pod_builder b;
#define POSITION_PRESERVE 0
#define POSITION_AUX 1
#define POSITION_UNKNOWN 2
	int res, position = POSITION_PRESERVE;
	struct spa_pod *param;
	uint32_t media_type, media_subtype;
	bool have_format = false, monitor = false, control = false;
	struct spa_audio_info format = { 0, };
	enum spa_param_port_config_mode mode = SPA_PARAM_PORT_CONFIG_MODE_none;
	struct spa_json it[2];
	char key[1024], val[256];

	spa_json_init(&it[0], str, strlen(str));
	if (spa_json_enter_object(&it[0], &it[1]) <= 0)
		return -EINVAL;

	while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
		if (spa_json_get_string(&it[1], val, sizeof(val)) <= 0)
			break;

		if (spa_streq(key, "mode")) {
			mode = spa_debug_type_find_type_short(spa_type_param_port_config_mode, val);
			if (mode == SPA_ID_INVALID)
				mode = SPA_PARAM_PORT_CONFIG_MODE_none;
		} else if (spa_streq(key, "monitor")) {
			monitor = spa_atob(val);
		} else if (spa_streq(key, "control")) {
			control = spa_atob(val);
		} else if (spa_streq(key, "position")) {
			if (spa_streq(val, "unknown"))
				position = POSITION_UNKNOWN;
			else if (spa_streq(val, "aux"))
				position = POSITION_AUX;
			else
				position = POSITION_PRESERVE;
		}
        }

	while (true) {
		struct spa_audio_info info = { 0, };
		struct spa_pod *position = NULL;
		uint32_t n_position = 0;

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		if ((res = spa_node_port_enum_params_sync(n->follower,
					n->direction == PW_DIRECTION_INPUT ?
						SPA_DIRECTION_INPUT :
						SPA_DIRECTION_OUTPUT, 0,
					SPA_PARAM_EnumFormat, &state,
					NULL, &param, &b)) != 1)
			break;

		if ((res = spa_format_parse(param, &media_type, &media_subtype)) < 0)
			continue;

		if (media_type != SPA_MEDIA_TYPE_audio ||
		    media_subtype != SPA_MEDIA_SUBTYPE_raw)
			continue;

		spa_pod_object_fixate((struct spa_pod_object*)param);

		if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_Format, NULL,
				SPA_FORMAT_AUDIO_format,        SPA_POD_Id(&info.info.raw.format),
				SPA_FORMAT_AUDIO_rate,          SPA_POD_Int(&info.info.raw.rate),
				SPA_FORMAT_AUDIO_channels,      SPA_POD_Int(&info.info.raw.channels),
				SPA_FORMAT_AUDIO_position,      SPA_POD_OPT_Pod(&position)) < 0)
			continue;

		if (position != NULL)
			n_position = spa_pod_copy_array(position, SPA_TYPE_Id,
					info.info.raw.position, SPA_AUDIO_MAX_CHANNELS);
		if (n_position == 0 || n_position != info.info.raw.channels)
			SPA_FLAG_SET(info.info.raw.flags, SPA_AUDIO_FLAG_UNPOSITIONED);

		if (format.info.raw.channels >= info.info.raw.channels)
			continue;

		format = info;
		have_format = true;
	}
	if (!have_format)
		return -ENOENT;

	if (position == POSITION_AUX) {
		for (i = 0; i < format.info.raw.channels; i++)
			format.info.raw.position[i] = SPA_AUDIO_CHANNEL_START_Aux + i;
	} else if (position == POSITION_UNKNOWN) {
		for (i = 0; i < format.info.raw.channels; i++)
			format.info.raw.position[i] = SPA_AUDIO_CHANNEL_UNKNOWN;
	}

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &format.info.raw);
	param = spa_pod_builder_add_object(&b,
		SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
		SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(n->direction),
		SPA_PARAM_PORT_CONFIG_mode,      SPA_POD_Id(mode),
		SPA_PARAM_PORT_CONFIG_monitor,   SPA_POD_Bool(monitor),
		SPA_PARAM_PORT_CONFIG_control,   SPA_POD_Bool(control),
		SPA_PARAM_PORT_CONFIG_format,    SPA_POD_Pod(param));
	pw_impl_node_set_param(n->node, SPA_PARAM_PortConfig, 0, param);

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
	const struct spa_dict_item *it;
	struct pw_properties *copy;
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

	copy = pw_properties_new(NULL, NULL);
	spa_dict_for_each(it, &props->dict) {
		if (!spa_strstartswith(it->key, "node.param.") &&
		    !spa_strstartswith(it->key, "port.param."))
			pw_properties_set(copy, it->key, it->value);
	}
	node = pw_spa_node_load(context,
				factory_name,
				PW_SPA_NODE_FLAG_ACTIVATE | PW_SPA_NODE_FLAG_NO_REGISTER,
				copy, sizeof(struct node) + user_data_size);
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

	if ((str = pw_properties_get(props, "adapter.auto-port-config")) != NULL)
		do_auto_port_config(n, str);

	spa_dict_for_each(it, &props->dict) {
		if (spa_strstartswith(it->key, "node.param.")) {
			if ((res = handle_node_param(node, &it->key[11], it->value)) < 0)
				pw_log_warn("can't set node param: %s", spa_strerror(res));
		}
	}
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
