/* PipeWire
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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>
#include <time.h>

#include "config.h"

#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/utils/hook.h>
#include <spa/utils/names.h>
#include <spa/utils/type-info.h>
#include <spa/param/format.h>
#include <spa/param/format-utils.h>
#include <spa/debug/types.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"

#include "modules/spa/spa-node.h"

#define NAME "adapter"

#define PORT_BUFFERS	1
#define MAX_BUFFER_SIZE	2048

extern const struct spa_handle_factory spa_floatmix_factory;

struct buffer {
	struct spa_buffer buf;
	struct spa_data datas[1];
	struct spa_chunk chunk[1];
};

struct node;

struct port {
	struct spa_list link;

	struct pw_port *port;
	struct node *node;

	struct buffer buffers[PORT_BUFFERS];

	struct spa_buffer *bufs[PORT_BUFFERS];

	struct spa_handle *spa_handle;
	struct spa_node *spa_node;

	float empty[MAX_BUFFER_SIZE + 15];
};

struct node {
	struct pw_core *core;

	struct pw_node *node;
	struct spa_hook node_listener;

	struct pw_node *slave;

	void *user_data;
	enum pw_direction direction;
	struct pw_properties *props;

	uint32_t media_type;
	uint32_t media_subtype;

	struct spa_list ports;
};

/** \endcond */

static void init_buffer(struct port *port, uint32_t id)
{
	struct buffer *b = &port->buffers[id];
	b->buf.n_metas = 0;
	b->buf.metas = NULL;
	b->buf.n_datas = 1;
	b->buf.datas = b->datas;
	b->datas[0].type = SPA_DATA_MemPtr;
	b->datas[0].flags = SPA_DATA_FLAG_DYNAMIC;
	b->datas[0].fd = -1;
	b->datas[0].mapoffset = 0;
	b->datas[0].maxsize = SPA_ROUND_DOWN_N(sizeof(port->empty), 16);
	b->datas[0].data = SPA_PTR_ALIGN(port->empty, 16, void);
	b->datas[0].chunk = b->chunk;
	b->datas[0].chunk->offset = 0;
	b->datas[0].chunk->size = 0;
	b->datas[0].chunk->stride = 0;
	port->bufs[id] = &b->buf;
	memset(port->empty, 0, sizeof(port->empty));
	pw_log_debug("%p %d", b->datas[0].data, b->datas[0].maxsize);
}

static void init_port(struct port *p, enum spa_direction direction)
{
	int i;
	for (i = 0; i < PORT_BUFFERS; i++)
		init_buffer(p, i);
}

static int port_use_buffers(void *data,
		uint32_t flags,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct port *p = data;
	struct pw_port *port = p->port;
	struct pw_node *node = port->node;
	int res, i;

	pw_log_debug(NAME " %p: port %p %p %d", p->node->node, port, buffers, n_buffers);

	if (n_buffers > 0) {
		for (i = 0; i < PORT_BUFFERS; i++)
			init_buffer(p, i);

		n_buffers = PORT_BUFFERS;
		buffers = p->bufs;
	}

	res = spa_node_port_use_buffers(port->mix,
			pw_direction_reverse(port->direction), 0,
			flags,
			buffers, n_buffers);
	res = spa_node_port_use_buffers(node->node,
			port->direction, port->port_id,
			flags,
			buffers, n_buffers);
	return res;
}

static const struct pw_port_implementation port_implementation = {
	.use_buffers = port_use_buffers,
};

static void node_destroy(void *data)
{
	struct node *n = data;
	struct port *p;

	spa_list_for_each(p, &n->ports, link)
		pw_port_set_mix(p->port, NULL, 0);
}

static void node_free(void *data)
{
	struct node *n = data;
	struct port *p;

	spa_list_consume(p, &n->ports, link) {
		spa_list_remove(&p->link);
		spa_handle_clear(p->spa_handle);
		free(p);
	}
	pw_properties_free(n->props);
}

static void node_port_init(void *data, struct pw_port *port)
{
	struct node *n = data;
	struct port *p;
	const struct pw_properties *old;
	enum pw_direction direction;
	struct pw_properties *new;
	const char *str, *node_name, *media_class;
	void *iface;
	const struct spa_support *support;
	uint32_t n_support;
	char position[8], *prefix;
	bool is_monitor, is_device;

	direction = pw_port_get_direction(port);

	old = pw_port_get_properties(port);

	is_monitor = (str = pw_properties_get(old, PW_KEY_PORT_MONITOR)) != NULL &&
			pw_properties_parse_bool(str);

	if (!is_monitor && direction != n->direction)
		return;

	node_name = pw_properties_get(n->props, PW_KEY_NODE_NAME);
	media_class = pw_properties_get(n->props, PW_KEY_MEDIA_CLASS);

	if (media_class != NULL &&
	    (strstr(media_class, "Sink") != NULL ||
	     strstr(media_class, "Source") != NULL))
		is_device = true;
	else
		is_device = false;

	new = pw_properties_new(NULL, NULL);

	if (is_monitor)
		prefix = "monitor";
	else if (is_device)
		prefix = direction == PW_DIRECTION_INPUT ?
			"playback" : "capture";
	else
		prefix = direction == PW_DIRECTION_INPUT ?
			"input" : "output";

	if ((str = pw_properties_get(old, PW_KEY_AUDIO_CHANNEL)) == NULL ||
	    strcmp(str, "UNK") == 0) {
		snprintf(position, 7, "%d", port->port_id);
		str = position;
	}

	pw_properties_setf(new, PW_KEY_PORT_NAME, "%s_%s", prefix, str);

	if (direction == n->direction) {
		const char *api = pw_properties_get(n->props, PW_KEY_DEVICE_API);

		pw_properties_setf(new, PW_KEY_PORT_ALIAS1, "%s_pcm:%s:%s%s",
				api ? api : "adapter",
				node_name ? node_name : "node",
				direction == PW_DIRECTION_INPUT ? "in" : "out",
				str);

		pw_properties_set(new, PW_KEY_PORT_PHYSICAL, "1");
		pw_properties_set(new, PW_KEY_PORT_TERMINAL, "1");
	}

	pw_port_update_properties(port, &new->dict);
	pw_properties_free(new);

	if (direction != n->direction)
		return;

	if (n->media_type == SPA_MEDIA_TYPE_audio) {
		p = calloc(1, sizeof(struct port) +
			spa_handle_factory_get_size(&spa_floatmix_factory, NULL));
		p->node = n;
		p->port = port;
		init_port(p, direction);
		p->spa_handle = SPA_MEMBER(p, sizeof(struct port), struct spa_handle);

		support = pw_core_get_support(n->core, &n_support);
		spa_handle_factory_init(&spa_floatmix_factory,
				p->spa_handle, NULL,
				support, n_support);

		spa_handle_get_interface(p->spa_handle, SPA_TYPE_INTERFACE_Node, &iface);
		p->spa_node = iface;

		if (direction == PW_DIRECTION_INPUT) {
			pw_log_debug("mix node %p", p->spa_node);
			pw_port_set_mix(port, p->spa_node, PW_PORT_MIX_FLAG_MULTI);
			port->impl = SPA_CALLBACKS_INIT(&port_implementation, p);
		}
		spa_list_append(&n->ports, &p->link);
	}
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = node_destroy,
	.free = node_free,
	.port_init = node_port_init,
};


static int find_format(struct pw_node *node, enum pw_direction direction,
		uint32_t *media_type, uint32_t *media_subtype)
{
	uint32_t state = 0;
	uint8_t buffer[4096];
	struct spa_pod_builder b;
	int res;
	struct spa_pod *format;

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	if ((res = spa_node_port_enum_params_sync(node->node,
				direction == PW_DIRECTION_INPUT ?
					SPA_DIRECTION_INPUT :
					SPA_DIRECTION_OUTPUT, 0,
				SPA_PARAM_EnumFormat, &state,
				NULL, &format, &b)) != 1) {
		pw_log_warn(NAME " %p: no format given", node);
		return -ENOENT;
	}

	if ((res = spa_format_parse(format, media_type, media_subtype)) < 0)
		return res;

	pw_log_debug(NAME " %p: %s/%s", node,
			spa_debug_type_find_name(spa_type_media_type, *media_type),
			spa_debug_type_find_name(spa_type_media_subtype, *media_subtype));
	return 0;
}


struct pw_node *pw_adapter_new(struct pw_core *core,
		struct pw_node *slave,
		struct pw_properties *props,
		size_t user_data_size)
{
	struct pw_node *node;
	struct node *n;
	const char *str, *factory_name;
	const struct pw_node_info *info;
	enum pw_direction direction;
	int res;
	uint32_t media_type, media_subtype;

	info = pw_node_get_info(slave);
	if (info == NULL) {
		res = -EINVAL;
		goto error;
	}

	pw_log_debug(NAME " %p: in %d/%d out %d/%d", slave,
			info->n_input_ports, info->max_input_ports,
			info->n_output_ports, info->max_output_ports);

	pw_properties_update(props, info->props);

	if (info->n_output_ports > 0) {
		direction = PW_DIRECTION_OUTPUT;
	} else if (info->n_input_ports > 0) {
		direction = PW_DIRECTION_INPUT;
	} else {
		res = -EINVAL;
		goto error;
	}

	if ((str = pw_properties_get(props, PW_KEY_NODE_ID)) != NULL)
		pw_properties_set(props, PW_KEY_NODE_SESSION, str);

	if ((str = pw_properties_get(props, "factory.mode")) == NULL) {
		if (direction == PW_DIRECTION_INPUT) {
			str = "merge";
		} else {
			str = "split";
		}
		pw_properties_set(props, "factory.mode", str);
	}

	if ((res = find_format(slave, direction, &media_type, &media_subtype)) < 0)
		goto error;

	if (media_type == SPA_MEDIA_TYPE_audio) {
		pw_properties_setf(props, "audio.adapt.slave", "pointer:%p", slave->node);
		pw_properties_set(props, SPA_KEY_LIBRARY_NAME, "audioconvert/libspa-audioconvert");
		if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
			pw_properties_setf(props, PW_KEY_MEDIA_CLASS, "Audio/%s",
				direction == PW_DIRECTION_INPUT ? "Sink" : "Source");
		factory_name = SPA_NAME_AUDIO_ADAPT;
	}
	else if (media_type == SPA_MEDIA_TYPE_video) {
		pw_properties_setf(props, "video.adapt.slave", "pointer:%p", slave->node);
		pw_properties_set(props, SPA_KEY_LIBRARY_NAME, "videoconvert/libspa-videoconvert");
		if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
			pw_properties_setf(props, PW_KEY_MEDIA_CLASS, "Video/%s",
				direction == PW_DIRECTION_INPUT ? "Sink" : "Source");
		factory_name = SPA_NAME_VIDEO_ADAPT;
	} else {
		res = -ENOTSUP;
		goto error;
	}

	node = pw_spa_node_load(core,
				factory_name,
				PW_SPA_NODE_FLAG_ACTIVATE | PW_SPA_NODE_FLAG_NO_REGISTER,
				pw_properties_copy(props),
				sizeof(struct node) + user_data_size);
        if (node == NULL) {
		res = -errno;
		pw_log_error("can't load spa node: %m");
		goto error;
	}

	n = pw_spa_node_get_user_data(node);
	n->core = core;
	n->node = node;
	n->slave = slave;
	n->direction = direction;
	n->props = props;
	n->media_type = media_type;
	n->media_subtype = media_subtype;
	spa_list_init(&n->ports);

	if (user_data_size > 0)
		n->user_data = SPA_MEMBER(n, sizeof(struct node), void);

	pw_node_add_listener(node, &n->node_listener, &node_events, n);

	return node;

error:
	if (props)
		pw_properties_free(props);
	errno = -res;
	return NULL;
}

void *pw_adapter_get_user_data(struct pw_node *node)
{
	struct node *n = pw_spa_node_get_user_data(node);
	return n->user_data;
}
