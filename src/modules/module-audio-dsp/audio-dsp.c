/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
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
#include <spa/utils/type-info.h>
#include <spa/param/audio/type-info.h>

#include "pipewire/core.h"
#include "pipewire/link.h"
#include "pipewire/log.h"
#include "pipewire/module.h"
#include "pipewire/type.h"
#include "pipewire/private.h"

#include "modules/spa/spa-node.h"

#define NAME "audio-dsp"

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

	float empty[MAX_BUFFER_SIZE];
};

struct node {
	struct pw_core *core;

	struct pw_node *node;
	struct spa_hook node_listener;

	void *user_data;
	enum pw_direction direction;
	struct pw_properties *props;

	struct spa_audio_info format;
	int max_buffer_size;

	struct spa_list ports;
};

/** \endcond */

static void init_buffer(struct port *port, uint32_t id)
{
	struct buffer *b = &port->buffers[id];
	b->buf.id = id;
	b->buf.n_metas = 0;
	b->buf.metas = NULL;
	b->buf.n_datas = 1;
	b->buf.datas = b->datas;
	b->datas[0].type = SPA_DATA_MemPtr;
	b->datas[0].flags = 0;
	b->datas[0].fd = -1;
	b->datas[0].mapoffset = 0;
	b->datas[0].maxsize = sizeof(port->empty);
	b->datas[0].data = port->empty;
	b->datas[0].chunk = b->chunk;
	b->datas[0].chunk->offset = 0;
	b->datas[0].chunk->size = 0;
	b->datas[0].chunk->stride = 0;
	port->bufs[id] = &b->buf;
	memset(port->empty, 0, sizeof(port->empty));
}

static void init_port(struct port *p, enum spa_direction direction)
{
	int i;
	for (i = 0; i < PORT_BUFFERS; i++)
		init_buffer(p, i);
}

static int port_use_buffers(void *data,
			    struct spa_buffer **buffers,
			    uint32_t n_buffers)
{
	struct port *p = data;
	struct pw_port *port = p->port;
	struct pw_node *node = port->node;
	int res, i;

	pw_log_debug(NAME " %p: port %p", p->node->node, port);

	if (n_buffers > 0) {
		for (i = 0; i < PORT_BUFFERS; i++)
			init_buffer(p, i);

		n_buffers = PORT_BUFFERS;
		buffers = p->bufs;
	}

	res = spa_node_port_use_buffers(port->mix,
			pw_direction_reverse(port->direction),
			0,
			buffers,
			n_buffers);
	res = spa_node_port_use_buffers(node->node,
			port->direction,
			port->port_id,
			buffers,
			n_buffers);
	return res;
}

static const struct pw_port_implementation port_implementation = {
	.use_buffers = port_use_buffers,
};

static void node_destroy(void *data)
{
	struct node *n = data;
	pw_properties_free(n->props);
}

static void node_port_init(void *data, struct pw_port *port)
{
	struct node *n = data;
	struct port *p;
	const struct pw_properties *old;
	enum pw_direction direction;
	struct pw_properties *new;
	const char *str;
	void *iface;
	const struct spa_support *support;
	uint32_t n_support;
	char position[8];

	direction = pw_port_get_direction(port);
	if (direction == n->direction)
		return;

	old = pw_port_get_properties(port);

	new = pw_properties_new(
			"port.dsp", "32 bit float mono audio",
			"port.physical", "1",
			"port.terminal", "1",
			NULL);

	if ((str = pw_properties_get(old, "port.channel")) == NULL ||
	    strcmp(str, "UNK") == 0) {
		snprintf(position, 7, "%d", port->port_id);
		str = position;
	}


	pw_properties_setf(new, "port.name", "%s_%s",
		direction == PW_DIRECTION_INPUT ? "playback" : "capture",
		str);

	pw_properties_setf(new, "port.alias1", "%s_pcm:%s:%s%s",
			pw_properties_get(n->props, "device.api"),
			pw_properties_get(n->props, "device.name"),
			direction == PW_DIRECTION_INPUT ? "in" : "out",
			str);

	pw_port_update_properties(port, &new->dict);
	pw_properties_free(new);

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
		port->implementation = &port_implementation;
		port->implementation_data = p;
	}
	spa_list_append(&n->ports, &p->link);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.destroy = node_destroy,
	.port_init = node_port_init,
};

struct pw_node *pw_audio_dsp_new(struct pw_core *core,
		const struct pw_properties *props,
		enum pw_direction direction,
		uint32_t max_buffer_size,
		size_t user_data_size)
{
	struct pw_node *node;
	struct node *n;
	const char *api, *alias, *plugged, *str;
	char node_name[128];
	struct pw_properties *pr;
	int i;

	if ((api = pw_properties_get(props, "device.api")) == NULL)
		goto error;
	if ((alias = pw_properties_get(props, "device.name")) == NULL)
		goto error;

	snprintf(node_name, sizeof(node_name), "system_%s", alias);
	for (i = 0; node_name[i]; i++) {
		if (node_name[i] == ':' || node_name[i] == ',')
			node_name[i] = '_';
	}

	pr = pw_properties_new(
			"media.class",
			direction == PW_DIRECTION_OUTPUT ?
				"Audio/DSP/Playback" :
				"Audio/DSP/Capture",
			"device.name", alias,
			"device.api", api,
			NULL);

	if ((plugged = pw_properties_get(props, "node.plugged")) != NULL)
		pw_properties_set(pr, "node.plugged", plugged);
	if ((str = pw_properties_get(props, "node.id")) != NULL)
		pw_properties_set(pr, "node.session", str);

	node = pw_spa_node_load(core, NULL, NULL,
			"audioconvert/libspa-audioconvert",
			direction == PW_DIRECTION_OUTPUT ?
				"merger" :
				"splitter",
			node_name,
			PW_SPA_NODE_FLAG_ACTIVATE | PW_SPA_NODE_FLAG_NO_REGISTER,
			pr, sizeof(struct node) + user_data_size);

        if (node == NULL)
		goto error;

	n = pw_spa_node_get_user_data(node);
	n->core = core;
	n->node = node;
	n->direction = direction;
	n->props = pw_properties_copy(props);
	spa_list_init(&n->ports);

	n->max_buffer_size = max_buffer_size;

	if (user_data_size > 0)
		n->user_data = SPA_MEMBER(n, sizeof(struct node), void);

	pw_node_add_listener(node, &n->node_listener, &node_events, n);

	return node;

     error:
	return NULL;
}

void *pw_audio_dsp_get_user_data(struct pw_node *node)
{
	struct node *n = pw_spa_node_get_user_data(node);
	return n->user_data;
}
