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

#include <errno.h>
#include <stdio.h>
#include <math.h>
#include <sys/mman.h>

#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/node/io.h>
#include <spa/debug/format.h>

#include <pipewire/pipewire.h>

#define M_PI_M2 ( M_PI + M_PI )

#define BUFFER_SAMPLES	128

struct buffer {
	uint32_t id;
	struct spa_buffer *buffer;
	struct spa_list link;
	void *ptr;
	bool mapped;
};

struct data {
	const char *path;

	struct pw_main_loop *loop;

	struct pw_core *core;

	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct spa_port_info port_info;
	struct spa_dict port_props;
	struct spa_dict_item port_items[1];

	struct spa_node impl_node;
	const struct spa_node_callbacks *callbacks;
	void *callbacks_data;
	struct spa_io_buffers *io;
	struct spa_io_control *io_notify;
	uint32_t io_notify_size;

	struct spa_audio_info_raw format;

	struct buffer buffers[32];
	uint32_t n_buffers;
	struct spa_list empty;

	double accumulator;
	double volume_accum;
};

static void update_volume(struct data *data)
{
	struct spa_pod_builder b = { 0, };
	struct spa_pod_frame f[2];

	if (data->io_notify == NULL)
		return;

	spa_pod_builder_init(&b, data->io_notify, data->io_notify_size);
	spa_pod_builder_push_sequence(&b, &f[0], 0);
	spa_pod_builder_control(&b, 0, SPA_CONTROL_Properties);
	spa_pod_builder_push_object(&b, &f[1], SPA_TYPE_OBJECT_Props, 0);
	spa_pod_builder_prop(&b, SPA_PROP_volume, 0);
	spa_pod_builder_float(&b, (sin(data->volume_accum) / 2.0) + 0.5);
	spa_pod_builder_pop(&b, &f[1]);
	spa_pod_builder_pop(&b, &f[0]);

        data->volume_accum += M_PI_M2 / 1000.0;
        if (data->volume_accum >= M_PI_M2)
                data->volume_accum -= M_PI_M2;
}

static int impl_send_command(struct spa_node *node, const struct spa_command *command)
{
	return 0;
}

static int impl_set_callbacks(struct spa_node *node,
			      const struct spa_node_callbacks *callbacks, void *data)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	d->callbacks = callbacks;
	d->callbacks_data = data;
	return 0;
}

static int impl_set_io(struct spa_node *node,
			uint32_t id, void *data, size_t size)
{
	return 0;
}


static int impl_get_n_ports(struct spa_node *node,
			    uint32_t *n_input_ports,
			    uint32_t *max_input_ports,
			    uint32_t *n_output_ports,
			    uint32_t *max_output_ports)
{
	*n_input_ports = *max_input_ports = 0;
	*n_output_ports = *max_output_ports = 1;
	return 0;
}

static int impl_get_port_ids(struct spa_node *node,
                             uint32_t *input_ids,
                             uint32_t n_input_ids,
                             uint32_t *output_ids,
                             uint32_t n_output_ids)
{
	if (n_output_ids > 0)
                output_ids[0] = 0;
	return 0;
}

static int impl_port_set_io(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			    uint32_t id, void *data, size_t size)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);

	switch (id) {
	case SPA_IO_Buffers:
		d->io = data;
		break;
	case SPA_IO_Notify:
		d->io_notify = data;
		d->io_notify_size = size;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_port_get_info(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			      const struct spa_port_info **info)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);

	d->port_info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
	d->port_info.rate = 0;
	d->port_info.props = &d->port_props;

	d->port_items[0] = SPA_DICT_ITEM_INIT("port.dsp", "32 bit float mono audio");
	d->port_props = SPA_DICT_INIT(d->port_items, 1);

	*info = &d->port_info;

	return 0;
}

static int port_enum_formats(struct spa_node *node,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t *index,
			     const struct spa_pod *filter,
			     struct spa_pod **param,
			     struct spa_pod_builder *builder)
{
	if (*index != 0)
		return 0;

	*param = spa_pod_builder_add_object(builder,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
		SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_AUDIO_format,   SPA_POD_CHOICE_ENUM_Id(5,
						SPA_AUDIO_FORMAT_S16,
						SPA_AUDIO_FORMAT_S16P,
						SPA_AUDIO_FORMAT_S16,
						SPA_AUDIO_FORMAT_F32P,
						SPA_AUDIO_FORMAT_F32),
		SPA_FORMAT_AUDIO_channels, SPA_POD_CHOICE_RANGE_Int(2, 1, INT32_MAX),
		SPA_FORMAT_AUDIO_rate,     SPA_POD_CHOICE_RANGE_Int(44100, 1, INT32_MAX));

	(*index)++;

	return 1;
}

static int impl_port_enum_params(struct spa_node *node,
				 enum spa_direction direction, uint32_t port_id,
				 uint32_t id, uint32_t *index,
				 const struct spa_pod *filter,
				 struct spa_pod **result,
				 struct spa_pod_builder *builder)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	struct spa_pod *param;

	switch (id) {
	case SPA_PARAM_List:
	{
		uint32_t list[] = { SPA_PARAM_EnumFormat,
				    SPA_PARAM_Format,
				    SPA_PARAM_Buffers,
				    SPA_PARAM_Meta,
				    SPA_PARAM_IO };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamList, id,
				SPA_PARAM_LIST_id, SPA_POD_Id(list[*index]));
		else
			return 0;
		break;
	}
	case SPA_PARAM_EnumFormat:
		return port_enum_formats(node, direction, port_id, index, filter, result, builder);

	case SPA_PARAM_Format:
		if (*index != 0)
			return 0;
		if (d->format.format == 0)
			return 0;
		param = spa_format_audio_raw_build(builder, id, &d->format);
		break;

	case SPA_PARAM_Buffers:
		if (*index > 0)
			return 0;

		param = spa_pod_builder_add_object(builder,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(1, 1, 32),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(BUFFER_SAMPLES * sizeof(float), 32, 4096),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int(0),
			SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16));
		break;

	case SPA_PARAM_Meta:
		switch (*index) {
		case 0:
			param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamMeta, id,
				SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
				SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
			break;
		default:
			return 0;
		}
		break;
	case SPA_PARAM_IO:
		switch (*index) {
		case 0:
			param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_Buffers),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
			break;
		case 1:
			param = spa_pod_builder_add_object(builder,
				SPA_TYPE_OBJECT_ParamIO, id,
				SPA_PARAM_IO_id, SPA_POD_Id(SPA_IO_Notify),
				SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_sequence) + 1024));
			break;
		default:
			return 0;
		}
		break;
	default:
		return -ENOENT;
	}

	(*index)++;
	*result = param;

	return 1;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags, const struct spa_pod *format)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);

	if (format == NULL) {
		d->format.format = 0;
		return 0;
	}

	spa_debug_format(0, NULL, format);

	if (spa_format_audio_raw_parse(format, &d->format) < 0)
		return -EINVAL;

	if (d->format.format != SPA_AUDIO_FORMAT_S16 &&
	    d->format.format != SPA_AUDIO_FORMAT_F32)
		return -EINVAL;

	return 0;
}

static int impl_port_set_param(struct spa_node *node,
			       enum spa_direction direction, uint32_t port_id,
			       uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	if (id == SPA_PARAM_Format) {
		return port_set_format(node, direction, port_id, flags, param);
	}
	else
		return -ENOENT;
}

static int impl_port_use_buffers(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
				 struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	uint32_t i;
	for (i = 0; i < n_buffers; i++) {
		struct buffer *b = &d->buffers[i];
		struct spa_data *datas = buffers[i]->datas;

		if (datas[0].data != NULL) {
			b->ptr = datas[0].data;
			b->mapped = false;
		}
		else if (datas[0].type == SPA_DATA_MemFd ||
			 datas[0].type == SPA_DATA_DmaBuf) {
			b->ptr = mmap(NULL, datas[0].maxsize + datas[0].mapoffset, PROT_WRITE,
				      MAP_SHARED, datas[0].fd, 0);
			if (b->ptr == MAP_FAILED) {
				pw_log_error("failed to buffer mem");
				return -errno;

			}
			b->ptr = SPA_MEMBER(b->ptr, datas[0].mapoffset, void);
			b->mapped = true;
		}
		else {
			pw_log_error("invalid buffer mem");
			return -EINVAL;
		}
		b->id = i;
		b->buffer = buffers[i];
		pw_log_info("got buffer %d size %d", i, datas[0].maxsize);
		spa_list_append(&d->empty, &b->link);
	}
	d->n_buffers = n_buffers;
	return 0;
}

static inline void reuse_buffer(struct data *d, uint32_t id)
{
	pw_log_trace("export-source %p: recycle buffer %d", d, id);
        spa_list_append(&d->empty, &d->buffers[id].link);
}

static int impl_port_reuse_buffer(struct spa_node *node, uint32_t port_id, uint32_t buffer_id)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	reuse_buffer(d, buffer_id);
	return 0;
}

static void fill_f32(struct data *d, void *dest, int avail)
{
	float *dst = dest;
	int n_samples = avail / (sizeof(float) * d->format.channels);
	int i;
	uint32_t c;

        for (i = 0; i < n_samples; i++) {
		float val;

                d->accumulator += M_PI_M2 * 440 / d->format.rate;
                if (d->accumulator >= M_PI_M2)
                        d->accumulator -= M_PI_M2;

                val = sin(d->accumulator);

                for (c = 0; c < d->format.channels; c++)
                        *dst++ = val;
        }
}

static void fill_s16(struct data *d, void *dest, int avail)
{
	int16_t *dst = dest;
	int n_samples = avail / (sizeof(int16_t) * d->format.channels);
	int i;
	uint32_t c;

        for (i = 0; i < n_samples; i++) {
                int16_t val;

                d->accumulator += M_PI_M2 * 440 / d->format.rate;
                if (d->accumulator >= M_PI_M2)
                        d->accumulator -= M_PI_M2;

                val = (int16_t) (sin(d->accumulator) * 32767.0);

                for (c = 0; c < d->format.channels; c++)
                        *dst++ = val;
        }
}

static int impl_node_process(struct spa_node *node)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	struct buffer *b;
	int avail;
        struct spa_io_buffers *io = d->io;
	uint32_t maxsize, index = 0;
	uint32_t filled, offset;
	struct spa_data *od;

	if (io->buffer_id < d->n_buffers) {
		reuse_buffer(d, io->buffer_id);
		io->buffer_id = SPA_ID_INVALID;
	}
	if (spa_list_is_empty(&d->empty)) {
                pw_log_error("export-source %p: out of buffers", d);
                return -EPIPE;
        }
        b = spa_list_first(&d->empty, struct buffer, link);
        spa_list_remove(&b->link);

	od = b->buffer->datas;

	maxsize = od[0].maxsize;

	filled = 0;
	index = 0;
	avail = maxsize - filled;
	offset = index % maxsize;

	if (offset + avail > maxsize)
		avail = maxsize - offset;

	if (d->format.format == SPA_AUDIO_FORMAT_S16)
		fill_s16(d, SPA_MEMBER(b->ptr, offset, void), avail);
	else if (d->format.format == SPA_AUDIO_FORMAT_F32)
		fill_f32(d, SPA_MEMBER(b->ptr, offset, void), avail);

	od[0].chunk->offset = 0;
	od[0].chunk->size = avail;
	od[0].chunk->stride = 0;

	io->buffer_id = b->id;
	io->status = SPA_STATUS_HAVE_BUFFER;

	update_volume(d);

	return SPA_STATUS_HAVE_BUFFER;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	.set_callbacks = impl_set_callbacks,
	.set_io = impl_set_io,
	.send_command = impl_send_command,
	.get_n_ports = impl_get_n_ports,
	.get_port_ids = impl_get_port_ids,
	.port_set_io = impl_port_set_io,
	.port_get_info = impl_port_get_info,
	.port_enum_params = impl_port_enum_params,
	.port_set_param = impl_port_set_param,
	.port_use_buffers = impl_port_use_buffers,
	.port_reuse_buffer = impl_port_reuse_buffer,
	.process = impl_node_process,
};

static void make_node(struct data *data)
{
	struct pw_properties *props;

	props = pw_properties_new(PW_NODE_PROP_AUTOCONNECT, "1",
				  PW_NODE_PROP_EXCLUSIVE, "1",
				  PW_NODE_PROP_MEDIA, "Audio",
				  PW_NODE_PROP_CATEGORY, "Playback",
				  PW_NODE_PROP_ROLE, "Music",
				  NULL);
	if (data->path)
		pw_properties_set(props, PW_NODE_PROP_TARGET_NODE, data->path);

	data->impl_node = impl_node;
	pw_remote_export(data->remote, SPA_TYPE_INTERFACE_Node, props, &data->impl_node);
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
	data.core = pw_core_new(pw_main_loop_get_loop(data.loop), NULL, 0);
        data.remote = pw_remote_new(data.core, NULL, 0);
	data.path = argc > 1 ? argv[1] : NULL;

	spa_list_init(&data.empty);

	pw_remote_add_listener(data.remote, &data.remote_listener, &remote_events, &data);

        pw_remote_connect(data.remote);

	pw_main_loop_run(data.loop);

	pw_core_destroy(data.core);
	pw_main_loop_destroy(data.loop);

	return 0;
}
