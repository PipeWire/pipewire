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

#include <errno.h>
#include <stdio.h>
#include <sys/mman.h>

#include <SDL2/SDL.h>

#include <spa/support/type-map.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/props.h>
#include <spa/node/io.h>
#include <spa/lib/debug.h>

#include <pipewire/pipewire.h>
#include <pipewire/module.h>
#include <pipewire/factory.h>

#define M_PI_M2 ( M_PI + M_PI )

#define MAX_BUFFERS	32

struct type {
	uint32_t prop_param;
	uint32_t io_prop_param;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_video format_video;
	struct spa_type_video_format video_format;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->prop_param = spa_type_map_get_id(map, SPA_TYPE_PROPS__contrast);
	type->io_prop_param = spa_type_map_get_id(map, SPA_TYPE_IO_PROP_BASE "contrast");
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_video_map(map, &type->format_video);
	spa_type_video_format_map(map, &type->video_format);
}

#define DEFAULT_PARAM 0.1

struct props {
	double param;
};

static void reset_props(struct props *props)
{
	props->param = DEFAULT_PARAM;
}

#define WIDTH   640
#define HEIGHT  480
#define BPP    3

struct data {
	struct type type;
	struct props props;

	const char *path;

	SDL_Renderer *renderer;
	SDL_Window *window;
	SDL_Texture *texture;

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
	struct spa_io_buffers *io;
	struct spa_pod_double *ctrl_param;
	double param_accum;

	uint8_t buffer[1024];

	struct spa_video_info_raw format;
	int32_t stride;

	struct spa_param *params[2];

	struct spa_buffer *buffers[MAX_BUFFERS];
	uint32_t n_buffers;
};

static void handle_events(struct data *data)
{
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_QUIT:
			pw_main_loop_quit(data->loop);
			break;
		}
	}
}

static struct {
	Uint32 format;
	uint32_t id;
} video_formats[] = {
	{ SDL_PIXELFORMAT_UNKNOWN, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_INDEX1LSB, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_UNKNOWN, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_INDEX1LSB, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_INDEX1MSB, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_INDEX4LSB, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_INDEX4MSB, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_INDEX8, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGB332, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGB444, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGB555, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_BGR555, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_ARGB4444, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGBA4444, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_ABGR4444, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_BGRA4444, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_ARGB1555, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGBA5551, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_ABGR1555, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_BGRA5551, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGB565, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_BGR565, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGB24, offsetof(struct spa_type_video_format, RGB),},
	{ SDL_PIXELFORMAT_RGB888, offsetof(struct spa_type_video_format, RGB),},
	{ SDL_PIXELFORMAT_RGBX8888, offsetof(struct spa_type_video_format, RGBx),},
	{ SDL_PIXELFORMAT_BGR24, offsetof(struct spa_type_video_format, BGR),},
	{ SDL_PIXELFORMAT_BGR888, offsetof(struct spa_type_video_format, BGR),},
	{ SDL_PIXELFORMAT_BGRX8888, offsetof(struct spa_type_video_format, BGRx),},
	{ SDL_PIXELFORMAT_ARGB2101010, offsetof(struct spa_type_video_format, UNKNOWN),},
	{ SDL_PIXELFORMAT_RGBA8888, offsetof(struct spa_type_video_format, RGBA),},
	{ SDL_PIXELFORMAT_ARGB8888, offsetof(struct spa_type_video_format, ARGB),},
	{ SDL_PIXELFORMAT_BGRA8888, offsetof(struct spa_type_video_format, BGRA),},
	{ SDL_PIXELFORMAT_ABGR8888, offsetof(struct spa_type_video_format, ABGR),},
	{ SDL_PIXELFORMAT_YV12, offsetof(struct spa_type_video_format, YV12),},
	{ SDL_PIXELFORMAT_IYUV, offsetof(struct spa_type_video_format, I420),},
	{ SDL_PIXELFORMAT_YUY2, offsetof(struct spa_type_video_format, YUY2),},
	{ SDL_PIXELFORMAT_UYVY, offsetof(struct spa_type_video_format, UYVY),},
	{ SDL_PIXELFORMAT_YVYU, offsetof(struct spa_type_video_format, YVYU),},
#if SDL_VERSION_ATLEAST(2,0,4)
	{ SDL_PIXELFORMAT_NV12, offsetof(struct spa_type_video_format, NV12),},
	{ SDL_PIXELFORMAT_NV21, offsetof(struct spa_type_video_format, NV21),},
#endif
};

static uint32_t sdl_format_to_id(struct data *data, Uint32 format)
{
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(video_formats); i++) {
		if (video_formats[i].format == format)
			return *SPA_MEMBER(&data->type.video_format, video_formats[i].id, uint32_t);
	}
	return data->type.video_format.UNKNOWN;
}

static Uint32 id_to_sdl_format(struct data *data, uint32_t id)
{
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(video_formats); i++) {
		if (*SPA_MEMBER(&data->type.video_format, video_formats[i].id, uint32_t) == id)
			return video_formats[i].format;
	}
	return SDL_PIXELFORMAT_UNKNOWN;
}

static void update_param(struct data *data)
{
	if (data->ctrl_param == NULL)
		return;

        data->ctrl_param->value = (sin(data->param_accum) * 127.0) + 127.0;
        data->param_accum += M_PI_M2 / 30.0;
        if (data->param_accum >= M_PI_M2)
                data->param_accum -= M_PI_M2;
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

static int impl_get_n_ports(struct spa_node *node,
			    uint32_t *n_input_ports,
			    uint32_t *max_input_ports,
			    uint32_t *n_output_ports,
			    uint32_t *max_output_ports)
{
	*n_input_ports = *max_input_ports = 1;
	*n_output_ports = *max_output_ports = 0;
	return 0;
}

static int impl_get_port_ids(struct spa_node *node,
                             uint32_t *input_ids,
                             uint32_t n_input_ids,
                             uint32_t *output_ids,
                             uint32_t n_output_ids)
{
	if (n_input_ids > 0)
                input_ids[0] = 0;
	return 0;
}

static int impl_port_set_io(struct spa_node *node,
			    enum spa_direction direction, uint32_t port_id,
			    uint32_t id, void *data, size_t size)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);

	if (id == d->t->io.Buffers)
		d->io = data;
	else if (id == d->type.io_prop_param) {
		if (data && size >= sizeof(struct spa_pod_double))
			d->ctrl_param = data;
		else
			d->ctrl_param = NULL;
	}
	else
		return -ENOENT;

	return 0;
}

static int impl_port_get_info(struct spa_node *node, enum spa_direction direction, uint32_t port_id,
			      const struct spa_port_info **info)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);

	d->port_info.flags = SPA_PORT_INFO_FLAG_CAN_USE_BUFFERS;
	d->port_info.rate = 0;
	d->port_info.props = NULL;

	*info = &d->port_info;

	return 0;
}

static int port_enum_formats(struct spa_node *node,
			     enum spa_direction direction, uint32_t port_id,
			     uint32_t *index,
			     const struct spa_pod *filter,
			     struct spa_pod **result,
			     struct spa_pod_builder *builder)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	SDL_RendererInfo info;
	uint32_t i, c;

	if (*index != 0)
		return 0;

	SDL_GetRendererInfo(d->renderer, &info);

	spa_pod_builder_push_object(builder,
				    d->t->param.idEnumFormat, d->t->spa_format);
	spa_pod_builder_id(builder, d->type.media_type.video);
	spa_pod_builder_id(builder, d->type.media_subtype.raw);

	spa_pod_builder_push_prop(builder, d->type.format_video.format,
				  SPA_POD_PROP_FLAG_UNSET |
				  SPA_POD_PROP_RANGE_ENUM);
	for (i = 0, c = 0; i < info.num_texture_formats; i++) {
		uint32_t id = sdl_format_to_id(d, info.texture_formats[i]);
		if (id == 0)
			continue;
		if (c++ == 0)
			spa_pod_builder_id(builder, id);
		spa_pod_builder_id(builder, id);
	}
	for (i = 0; i < SPA_N_ELEMENTS(video_formats); i++) {
		uint32_t id =
		    *SPA_MEMBER(&d->type.video_format, video_formats[i].id,
				uint32_t);
		if (id != d->type.video_format.UNKNOWN)
			spa_pod_builder_id(builder, id);
	}
	spa_pod_builder_pop(builder);
	spa_pod_builder_add(builder,
		":", d->type.format_video.size,      "Rru", &SPA_RECTANGLE(WIDTH, HEIGHT),
			SPA_POD_PROP_MIN_MAX(&SPA_RECTANGLE(1,1),
					     &SPA_RECTANGLE(info.max_texture_width,
							    info.max_texture_height)),
		":", d->type.format_video.framerate, "Fru", &SPA_FRACTION(25,1),
			SPA_POD_PROP_MIN_MAX(&SPA_FRACTION(0,1),
					     &SPA_FRACTION(30,1)),
		NULL);
	*result = spa_pod_builder_pop(builder);

	(*index)++;

	return 1;
}

static int port_get_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t *index,
			   const struct spa_pod *filter,
			   struct spa_pod **result,
			   struct spa_pod_builder *builder)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);

	if (*index != 0 || d->format.format == 0)
		return 0;

	*result = spa_pod_builder_object(builder,
		d->t->param.idFormat, d->t->spa_format,
		"I", d->type.media_type.video,
		"I", d->type.media_subtype.raw,
		":", d->type.format_video.format,    "I", d->format.format,
		":", d->type.format_video.size,      "R", &d->format.size,
		":", d->type.format_video.framerate, "F", &d->format.framerate);

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
	struct pw_type *t = d->t;
	struct spa_pod *param;

	if (id == t->param.idList) {
		uint32_t list[] = { t->param.idEnumFormat,
				    t->param.idFormat,
				    t->param.idBuffers,
				    t->param.idMeta,
				    t->param_io.idPropsOut };

		if (*index < SPA_N_ELEMENTS(list))
			param = spa_pod_builder_object(builder,
				id, t->param.List,
				":", t->param.listId, "I", list[*index]);
		else
			return 0;
	}
	else if (id == t->param.idEnumFormat) {
		return port_enum_formats(node, direction, port_id, index, filter, result, builder);
	}
	else if (id == t->param.idFormat) {
		return port_get_format(node, direction, port_id, index, filter, result, builder);
	}
	else if (id == t->param.idBuffers) {
		if (*index != 0)
			return 0;

		param = spa_pod_builder_object(builder,
			id, t->param_buffers.Buffers,
			":", t->param_buffers.size,    "i", d->stride * d->format.size.height,
			":", t->param_buffers.stride,  "i", d->stride,
			":", t->param_buffers.buffers, "iru", 2,
				SPA_POD_PROP_MIN_MAX(2, MAX_BUFFERS),
			":", t->param_buffers.align,   "i", 16);
	}
	else if (id == t->param.idMeta) {
		if (*index != 0)
			return 0;

		param = spa_pod_builder_object(builder,
			id, t->param_meta.Meta,
			":", t->param_meta.type, "I", t->meta.Header,
			":", t->param_meta.size, "i", sizeof(struct spa_meta_header));
	}
	else if (id == t->param_io.idPropsOut) {
		struct props *p = &d->props;

		switch (*index) {
		case 0:
			param = spa_pod_builder_object(builder,
				id, t->param_io.Prop,
				":", t->param_io.id, "I", d->type.io_prop_param,
				":", t->param_io.size, "i", sizeof(struct spa_pod_double),
				":", t->param.propId, "I", d->type.prop_param,
				":", t->param.propType, "dru", p->param,
					SPA_POD_PROP_MIN_MAX(0.0, 10.0));
			break;
		default:
			return 0;
		}
	}
	else
		return -ENOENT;

	(*index)++;

	*result = param;

	return 1;
}

static int port_set_format(struct spa_node *node,
			   enum spa_direction direction, uint32_t port_id,
			   uint32_t flags, const struct spa_pod *format)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	Uint32 sdl_format;
	void *dest;

	if (format == NULL)
		return 0;

	spa_debug_pod(format, SPA_DEBUG_FLAG_FORMAT);

	spa_format_video_raw_parse(format, &d->format, &d->type.format_video);

	sdl_format = id_to_sdl_format(d, d->format.format);
	if (sdl_format == SDL_PIXELFORMAT_UNKNOWN)
		return -EINVAL;

	d->texture = SDL_CreateTexture(d->renderer,
					  sdl_format,
					  SDL_TEXTUREACCESS_STREAMING,
					  d->format.size.width,
					  d->format.size.height);
	SDL_LockTexture(d->texture, NULL, &dest, &d->stride);
	SDL_UnlockTexture(d->texture);

	return 0;
}

static int impl_port_set_param(struct spa_node *node,
			       enum spa_direction direction, uint32_t port_id,
			       uint32_t id, uint32_t flags,
			       const struct spa_pod *param)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	struct pw_type *t = d->t;

	if (id == t->param.idFormat) {
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

	for (i = 0; i < n_buffers; i++)
		d->buffers[i] = buffers[i];
	d->n_buffers = n_buffers;
	return 0;
}

static int do_render(struct spa_loop *loop, bool async, uint32_t seq,
		     const void *_data, size_t size, void *user_data)
{
	struct data *d = user_data;
	const struct spa_buffer *buf = *(struct spa_buffer**)_data;
	uint8_t *map;
	void *sdata, *ddata;
	int sstride, dstride, ostride;
	uint32_t i;
	uint8_t *src, *dst;

	handle_events(d);

	if (buf->datas[0].type == d->t->data.MemFd ||
	    buf->datas[0].type == d->t->data.DmaBuf) {
		map = mmap(NULL, buf->datas[0].maxsize + buf->datas[0].mapoffset, PROT_READ,
			   MAP_PRIVATE, buf->datas[0].fd, 0);
		sdata = SPA_MEMBER(map, buf->datas[0].mapoffset, uint8_t);
	} else if (buf->datas[0].type == d->t->data.MemPtr) {
		map = NULL;
		sdata = buf->datas[0].data;
	} else
		return -EINVAL;

	if (SDL_LockTexture(d->texture, NULL, &ddata, &dstride) < 0) {
		fprintf(stderr, "Couldn't lock texture: %s\n", SDL_GetError());
		return -EIO;
	}
	sstride = buf->datas[0].chunk->stride;
	ostride = SPA_MIN(sstride, dstride);

	src = sdata;
	dst = ddata;
	for (i = 0; i < d->format.size.height; i++) {
		memcpy(dst, src, ostride);
		src += sstride;
		dst += dstride;
	}
	SDL_UnlockTexture(d->texture);

	SDL_RenderClear(d->renderer);
	SDL_RenderCopy(d->renderer, d->texture, NULL, NULL);
	SDL_RenderPresent(d->renderer);

	if (map)
		munmap(map, buf->datas[0].maxsize + buf->datas[0].mapoffset);

	return 0;
}

static int impl_node_process(struct spa_node *node)
{
	struct data *d = SPA_CONTAINER_OF(node, struct data, impl_node);
	struct spa_buffer *buf;
	int res;

	if (d->io->status != SPA_STATUS_HAVE_BUFFER)
		return SPA_STATUS_NEED_BUFFER;

	if (d->io->buffer_id >= d->n_buffers)
		return SPA_STATUS_NEED_BUFFER;

	buf = d->buffers[d->io->buffer_id];

	if ((res = pw_loop_invoke(pw_main_loop_get_loop(d->loop), do_render,
				  SPA_ID_INVALID, &buf, sizeof(struct spa_buffer *),
				  false, d)) < 0)
		return res;

	update_param(d);

	return d->io->status = SPA_STATUS_NEED_BUFFER;
}

static const struct spa_node impl_node = {
	SPA_VERSION_NODE,
	.set_callbacks = impl_set_callbacks,
	.send_command = impl_send_command,
	.get_n_ports = impl_get_n_ports,
	.get_port_ids = impl_get_port_ids,
	.port_set_io = impl_port_set_io,
	.port_get_info = impl_port_get_info,
	.port_enum_params = impl_port_enum_params,
	.port_set_param = impl_port_set_param,
	.port_use_buffers = impl_port_use_buffers,
	.process = impl_node_process,
};

static void make_node(struct data *data)
{
	struct pw_properties *props;

	props = pw_properties_new(PW_NODE_PROP_AUTOCONNECT, "1", NULL);
	if (data->path)
		pw_properties_set(props, PW_NODE_PROP_TARGET_NODE, data->path);

	data->node = pw_node_new(data->core, "SDL-sink", props, 0);
	data->impl_node = impl_node;
	pw_node_set_implementation(data->node, &data->impl_node);
	pw_node_register(data->node, NULL, NULL, NULL);
	pw_node_set_active(data->node, true);

	pw_remote_export(data->remote, data->node);
}

static void on_state_changed(void *_data, enum pw_remote_state old, enum pw_remote_state state, const char *error)
{
	struct data *data = _data;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		printf("remote error: %s\n", error);
		pw_main_loop_quit(data->loop);
		break;

	case PW_REMOTE_STATE_CONNECTED:
	{
		struct spa_dict_item items[5];
		int i = 0;

		/* set specific permissions on all existing objects without permissions */
		items[i++] = SPA_DICT_ITEM_INIT(PW_CORE_PROXY_PERMISSIONS_EXISTING, "r--");
		/* set default permission for new objects and objects without
		 * specific permissions */
		items[i++] = SPA_DICT_ITEM_INIT(PW_CORE_PROXY_PERMISSIONS_DEFAULT, "---");
		/* an example, set specific permissions on one object, this is the
		 * core object, we already have a binding to it that is not affected
		 * by the removal of X permissions, only future bindings. */
		items[i++] = SPA_DICT_ITEM_INIT(PW_CORE_PROXY_PERMISSIONS_GLOBAL, "0:rw-");

		pw_core_proxy_permissions(pw_remote_get_core_proxy(data->remote),
					  &SPA_DICT_INIT(items, i));

		make_node(data);
		break;
	}
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

	init_type(&data.type, data.t->map);
	reset_props(&data.props);

	spa_debug_set_type_map(data.t->map);

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		printf("can't initialize SDL: %s\n", SDL_GetError());
		return -1;
	}

	if (SDL_CreateWindowAndRenderer
	    (WIDTH, HEIGHT, SDL_WINDOW_RESIZABLE, &data.window, &data.renderer)) {
		printf("can't create window: %s\n", SDL_GetError());
		return -1;
	}

	pw_remote_add_listener(data.remote, &data.remote_listener, &remote_events, &data);

        pw_remote_connect(data.remote);

	pw_main_loop_run(data.loop);

	pw_core_destroy(data.core);
	pw_main_loop_destroy(data.loop);

	return 0;
}
