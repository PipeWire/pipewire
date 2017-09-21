/* PipeWire
 * Copyright (C) 2017 Wim Taymans <wim.taymans@gmail.com>
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

#include <stdio.h>
#include <math.h>
#include <sys/mman.h>

#include <spa/type-map.h>
#include <spa/format-utils.h>
#include <spa/audio/format-utils.h>
#include <spa/format-builder.h>
#include <spa/props.h>
#include <spa/lib/debug.h>

#include <pipewire/pipewire.h>

#define M_PI_M2 ( M_PI + M_PI )

struct type {
	uint32_t format;
	uint32_t props;
	struct spa_type_meta meta;
	struct spa_type_data data;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_audio format_audio;
	struct spa_type_audio_format audio_format;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
	type->props = spa_type_map_get_id(map, SPA_TYPE__Props);
	spa_type_meta_map(map, &type->meta);
	spa_type_data_map(map, &type->data);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_audio_map(map, &type->format_audio);
	spa_type_audio_format_map(map, &type->audio_format);
}

struct buffer {
	struct spa_buffer *buffer;
	struct spa_list link;
	void *ptr;
	size_t size;
	bool mapped;
};

struct data {
	struct type type;

	const char *path;

	struct pw_main_loop *loop;

	struct pw_core *core;
	struct pw_type *t;

	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_node *node;
	struct spa_port_info port_info;

	struct spa_node impl_node;
	const struct spa_node_callbacks *callbacks;
	void *callbacks_data;
	struct spa_port_io *io;

	uint8_t buffer[1024];

	struct spa_audio_info_raw format;

	uint8_t params_buffer[1024];
	struct spa_param *params[2];

	struct buffer buffers[32];
	int n_buffers;
	struct spa_list empty;

	double accumulator;
};

static int impl_send_command(struct spa_node *node, const struct spa_command *command)
{
	return SPA_RESULT_OK;
}

static int impl_set_callbacks(struct spa_node *node,
			      const struct spa_node_callbacks *callbacks, void *data)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	d->callbacks = callbacks;
	d->callbacks_data = data;
	return SPA_RESULT_OK;
}

static int impl_get_n_ports(struct spa_node *node,
			    uint32_t *n_input_ports,
			    uint32_t *max_input_ports,
			    uint32_t *n_output_ports,
			    uint32_t *max_output_ports)
{
	*n_input_ports = *max_input_ports = 0;
	*n_output_ports = *max_output_ports = 1;
	return SPA_RESULT_OK;
}

static int impl_get_port_ids(struct spa_node *node,
                             uint32_t n_input_ports,
                             uint32_t *input_ids,
                             uint32_t n_output_ports,
                             uint32_t *output_ids)
{
	if (n_output_ports > 0)
                output_ids[0] = 0;
	return SPA_RESULT_OK;
}

static int impl_port_set_io(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			    struct spa_port_io *io)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	d->io = io;
	return SPA_RESULT_OK;
}

static int impl_port_enum_formats(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
				  struct spa_format **format,
				  const struct spa_format *filter,
				  uint32_t index)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(d->buffer, sizeof(d->buffer));

	if (index != 0)
		return SPA_RESULT_ENUM_END;

	*format = spa_pod_builder_format(&b, d->type.format,
		d->type.media_type.audio, d->type.media_subtype.raw,
		":", d->type.format_audio.format,   "I", d->type.audio_format.S16,
		":", d->type.format_audio.channels, "i", 2,
		":", d->type.format_audio.rate,     "i", 44100);

	spa_debug_format(*format);

	return SPA_RESULT_OK;
}

static int impl_port_set_format(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
				uint32_t flags, const struct spa_format *format)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	struct pw_type *t = d->t;
	struct spa_pod_builder b = { NULL };

	if (format == NULL)
		return SPA_RESULT_OK;

	spa_debug_format(format);

	if (spa_format_audio_raw_parse(format, &d->format, &d->type.format_audio) < 0)
		return SPA_RESULT_INVALID_MEDIA_TYPE;

	if (d->format.format != d->type.audio_format.S16)
		return SPA_RESULT_ERROR;

	spa_pod_builder_init(&b, d->params_buffer, sizeof(d->params_buffer));
	d->params[0] = spa_pod_builder_param(&b,
		t->param_alloc_buffers.Buffers,
		":", t->param_alloc_buffers.size,    "iru", 1024,
									2, 32, 4096,
		":", t->param_alloc_buffers.stride,  "i",   0,
		":", t->param_alloc_buffers.buffers, "iru", 2,
									2, 2, 32,
		":", t->param_alloc_buffers.align,   "i",  16);

	d->params[1] = spa_pod_builder_param(&b,
		t->param_alloc_meta_enable.MetaEnable,
		":", t->param_alloc_meta_enable.type, "I", t->meta.Header,
		":", t->param_alloc_meta_enable.size, "i", sizeof(struct spa_meta_header));

	return SPA_RESULT_OK;
}

static int impl_port_get_format(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
				const struct spa_format **format)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(d->buffer, sizeof(d->buffer));

	*format = spa_pod_builder_format(&b, d->type.format,
		d->type.media_type.audio, d->type.media_subtype.raw,
		":", d->type.format_audio.format,   "I",  d->format.format,
		":", d->type.format_audio.channels, "i", d->format.channels,
		":", d->type.format_audio.rate,     "i", d->format.rate);

	return SPA_RESULT_OK;
}

static int impl_port_get_info(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			      const struct spa_port_info **info)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);

	d->port_info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
	d->port_info.rate = 0;
	d->port_info.props = NULL;

	*info = &d->port_info;

	return SPA_RESULT_OK;
}

static int impl_port_enum_params(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
				 uint32_t index, struct spa_param **param)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);

	if (index >= 2)
		return SPA_RESULT_ENUM_END;

	*param = d->params[index];

	return SPA_RESULT_OK;
}

static int impl_port_use_buffers(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
				 struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	int i;
	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &d->buffers[i];
		struct spa_data *datas = buffers[i]->datas;

		if (datas[0].data != NULL) {
			b->ptr = datas[0].data;
			b->mapped = false;
		}
		else if (datas[0].type == d->type.data.MemFd ||
			 datas[0].type == d->type.data.DmaBuf) {
			b->ptr = mmap(NULL, datas[0].maxsize + datas[0].mapoffset, PROT_WRITE,
				      MAP_SHARED, datas[0].fd, 0);
			if (b->ptr == MAP_FAILED) {
				pw_log_error("failed to buffer mem");
				return SPA_RESULT_ERROR;

			}
			b->ptr = SPA_MEMBER(b->ptr, datas[0].mapoffset, void);
			b->mapped = true;
		}
		else {
			pw_log_error("invalid buffer mem");
			return SPA_RESULT_ERROR;
		}
		b->size = datas[0].maxsize;
		b->buffer = buffers[i];
		pw_log_info("got buffer %d size %zd", i, b->size);
		spa_list_append(&d->empty, &b->link);
	}
	d->n_buffers = n_buffers;
	return SPA_RESULT_OK;
}

static inline void reuse_buffer(struct data *d, uint32_t id)
{
	pw_log_trace("sine-source %p: recycle buffer %d", d, id);
        spa_list_append(&d->empty, &d->buffers[id].link);
}

static int impl_node_process_output(struct spa_node *node)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	struct buffer *b;
	int i, c, n_samples;
	int16_t *dst;
        struct spa_port_io *io = d->io;

	if (io->buffer_id < d->n_buffers) {
		reuse_buffer(d, io->buffer_id);
		io->buffer_id = SPA_ID_INVALID;
	}
	if (spa_list_is_empty(&d->empty)) {
                pw_log_error("sine-source %p: out of buffers", d);
                return SPA_RESULT_OUT_OF_BUFFERS;
        }
        b = spa_list_first(&d->empty, struct buffer, link);
        spa_list_remove(&b->link);

	dst = b->ptr;
        n_samples = b->size / (sizeof(int16_t) * d->format.channels);

        for (i = 0; i < n_samples; i++) {
                int16_t val;

                d->accumulator += M_PI_M2 * 440 / d->format.rate;
                if (d->accumulator >= M_PI_M2)
                        d->accumulator -= M_PI_M2;

                val = (int16_t) (sin(d->accumulator) * 32767.0);

                for (c = 0; c < d->format.channels; c++)
                        *dst++ = val;
        }
	io->buffer_id = b->buffer->id;
	io->status = SPA_RESULT_HAVE_BUFFER;

	return SPA_RESULT_HAVE_BUFFER;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	.send_command = impl_send_command,
	.set_callbacks = impl_set_callbacks,
	.get_n_ports = impl_get_n_ports,
	.get_port_ids = impl_get_port_ids,
	.port_set_io = impl_port_set_io,
	.port_enum_formats = impl_port_enum_formats,
	.port_set_format = impl_port_set_format,
	.port_get_format = impl_port_get_format,
	.port_get_info = impl_port_get_info,
	.port_enum_params = impl_port_enum_params,
	.port_use_buffers = impl_port_use_buffers,
	.process_output = impl_node_process_output,
};

static void make_node(struct data *data)
{
	struct pw_properties *props;

	props = pw_properties_new(PW_NODE_PROP_AUTOCONNECT, "1", NULL);
	if (data->path)
		pw_properties_set(props, PW_NODE_PROP_TARGET_NODE, data->path);

	data->node = pw_node_new(data->core, "sine-source", props, 0);
	data->impl_node = impl_node;
	pw_node_set_implementation(data->node, &data->impl_node);

	pw_node_register(data->node, NULL, NULL);

	pw_remote_export(data->remote, data->node);
}

static void on_state_changed(void *_data, enum pw_remote_state old,
			     enum pw_remote_state state, const char *error)
{
	struct data *data = _data;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		printf("remote error: %s\n", error);
		pw_main_loop_quit(data->loop);
		break;

	case PW_REMOTE_STATE_CONNECTED:
		make_node(data);
		break;

	default:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));
		break;
	}
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_state_changed,
};

int main(int argc, char *argv[])
{
	struct data data = { 0, };

	pw_init(&argc, &argv);

	data.loop = pw_main_loop_new(NULL);
	data.core = pw_core_new(pw_main_loop_get_loop(data.loop), NULL);
	data.t = pw_core_get_type(data.core);
        data.remote = pw_remote_new(data.core, NULL, 0);
	data.path = argc > 1 ? argv[1] : NULL;

	spa_list_init(&data.empty);
	init_type(&data.type, data.t->map);
	spa_debug_set_type_map(data.t->map);

	pw_remote_add_listener(data.remote, &data.remote_listener, &remote_events, &data);

        pw_remote_connect(data.remote);

	pw_main_loop_run(data.loop);

	pw_core_destroy(data.core);
	pw_main_loop_destroy(data.loop);

	return 0;
}
