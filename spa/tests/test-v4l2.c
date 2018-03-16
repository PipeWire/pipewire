/* Spa
 * Copyright (C) 2016 Wim Taymans <wim.taymans@gmail.com>
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
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>
#include <poll.h>
#include <pthread.h>
#include <errno.h>
#include <sys/mman.h>

#include <SDL2/SDL.h>

#include <spa/support/log-impl.h>
#include <spa/support/loop.h>
#include <spa/support/type-map-impl.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/format-utils.h>

#include <lib/debug.h>

static SPA_TYPE_MAP_IMPL(default_map, 4096);
static SPA_LOG_IMPL(default_log);

struct type {
	uint32_t node;
	uint32_t props;
	uint32_t format;
	uint32_t props_device;
	uint32_t SDL_Texture;
	struct spa_type_io io;
	struct spa_type_param param;
	struct spa_type_meta meta;
	struct spa_type_data data;
	struct spa_type_media_type media_type;
	struct spa_type_media_subtype media_subtype;
	struct spa_type_format_video format_video;
	struct spa_type_video_format video_format;
	struct spa_type_event_node event_node;
	struct spa_type_command_node command_node;
};

static inline void init_type(struct type *type, struct spa_type_map *map)
{
	type->node = spa_type_map_get_id(map, SPA_TYPE__Node);
	type->props = spa_type_map_get_id(map, SPA_TYPE__Props);
	type->format = spa_type_map_get_id(map, SPA_TYPE__Format);
	type->props_device = spa_type_map_get_id(map, SPA_TYPE_PROPS__device);
	type->SDL_Texture = spa_type_map_get_id(map, SPA_TYPE_POINTER_BASE "SDL_Texture");
	spa_type_io_map(map, &type->io);
	spa_type_param_map(map, &type->param);
	spa_type_meta_map(map, &type->meta);
	spa_type_data_map(map, &type->data);
	spa_type_media_type_map(map, &type->media_type);
	spa_type_media_subtype_map(map, &type->media_subtype);
	spa_type_format_video_map(map, &type->format_video);
	spa_type_video_format_map(map, &type->video_format);
	spa_type_event_node_map(map, &type->event_node);
	spa_type_command_node_map(map, &type->command_node);
}

#define MAX_BUFFERS     8

struct buffer {
	struct spa_buffer buffer;
	struct spa_meta metas[1];
	struct spa_meta_header header;
	struct spa_data datas[1];
	struct spa_chunk chunks[1];
	SDL_Texture *texture;
};

struct data {
	struct type type;

	struct spa_type_map *map;
	struct spa_log *log;
	struct spa_loop data_loop;

	struct spa_support support[4];
	uint32_t n_support;

	struct spa_node *source;
	struct spa_io_buffers source_output[1];

	SDL_Renderer *renderer;
	SDL_Window *window;
	SDL_Texture *texture;

	bool use_buffer;

	bool running;
	pthread_t thread;

	struct spa_source sources[16];
	unsigned int n_sources;

	bool rebuild_fds;
	struct pollfd fds[16];
	unsigned int n_fds;

	struct spa_buffer *bp[MAX_BUFFERS];
	struct buffer buffers[MAX_BUFFERS];
	unsigned int n_buffers;
};

static int make_node(struct data *data, struct spa_node **node, const char *lib, const char *name)
{
	struct spa_handle *handle;
	int res;
	void *hnd;
	spa_handle_factory_enum_func_t enum_func;
	uint32_t i;

	if ((hnd = dlopen(lib, RTLD_NOW)) == NULL) {
		printf("can't load %s: %s\n", lib, dlerror());
		return -errno;
	}
	if ((enum_func = dlsym(hnd, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME)) == NULL) {
		printf("can't find enum function\n");
		return -errno;
	}

	for (i = 0;;) {
		const struct spa_handle_factory *factory;
		void *iface;

		if ((res = enum_func(&factory, &i)) <= 0) {
			if (res != 0)
				printf("can't enumerate factories: %s\n", spa_strerror(res));
			break;
		}
		if (strcmp(factory->name, name))
			continue;

		handle = calloc(1, factory->size);
		if ((res =
		     spa_handle_factory_init(factory, handle, NULL, data->support,
					     data->n_support)) < 0) {
			printf("can't make factory instance: %d\n", res);
			return res;
		}
		if ((res = spa_handle_get_interface(handle, data->type.node, &iface)) < 0) {
			printf("can't get interface %d\n", res);
			return res;
		}
		*node = iface;
		return 0;
	}
	return -EBADF;
}

static void handle_events(struct data *data)
{
	SDL_Event event;
	while (SDL_PollEvent(&event)) {
		switch (event.type) {
		case SDL_QUIT:
			exit(0);
			break;
		}
	}
}

static void on_source_done(void *data, int seq, int res)
{
	printf("got done %d %d\n", seq, res);
}

static void on_source_event(void *_data, struct spa_event *event)
{
	struct data *data = _data;

	handle_events(data);

	printf("got event %d\n", SPA_EVENT_TYPE(event));
}

static void on_source_have_output(void *_data)
{
	struct data *data = _data;
	int res;
	struct buffer *b;
	void *sdata, *ddata;
	int sstride, dstride;
	int i;
	uint8_t *src, *dst;
	struct spa_data *datas;
	struct spa_io_buffers *io = &data->source_output[0];

	handle_events(data);

	b = &data->buffers[io->buffer_id];

	datas = b->buffer.datas;

	if (b->texture) {
		SDL_Texture *texture = b->texture;

		SDL_UnlockTexture(texture);

		SDL_RenderClear(data->renderer);
		SDL_RenderCopy(data->renderer, texture, NULL, NULL);
		SDL_RenderPresent(data->renderer);

		if (SDL_LockTexture(texture, NULL, &sdata, &sstride) < 0) {
			fprintf(stderr, "Couldn't lock texture: %s\n", SDL_GetError());
			return;
		}
		datas[0].type = data->type.data.MemPtr;
		datas[0].flags = 0;
		datas[0].fd = -1;
		datas[0].mapoffset = 0;
		datas[0].maxsize = sstride * 240;
		datas[0].data = sdata;
		datas[0].chunk->offset = 0;
		datas[0].chunk->size = sstride * 240;
		datas[0].chunk->stride = sstride;
	} else {
		uint8_t *map;

		if (SDL_LockTexture(data->texture, NULL, &ddata, &dstride) < 0) {
			fprintf(stderr, "Couldn't lock texture: %s\n", SDL_GetError());
			return;
		}
		sdata = datas[0].data;
		if (datas[0].type == data->type.data.MemFd ||
		    datas[0].type == data->type.data.DmaBuf) {
			map = mmap(NULL, datas[0].maxsize + datas[0].mapoffset, PROT_READ,
				   MAP_PRIVATE, datas[0].fd, 0);
			sdata = SPA_MEMBER(map, datas[0].mapoffset, uint8_t);
		} else if (datas[0].type == data->type.data.MemPtr) {
			map = NULL;
			sdata = datas[0].data;
		} else
			return;

		sstride = datas[0].chunk->stride;

		for (i = 0; i < 240; i++) {
			src = ((uint8_t *) sdata + i * sstride);
			dst = ((uint8_t *) ddata + i * dstride);
			memcpy(dst, src, SPA_MIN(sstride, dstride));
		}
		SDL_UnlockTexture(data->texture);

		SDL_RenderClear(data->renderer);
		SDL_RenderCopy(data->renderer, data->texture, NULL, NULL);
		SDL_RenderPresent(data->renderer);

		if (map)
			munmap(map, datas[0].maxsize + datas[0].mapoffset);
	}

	io->status = SPA_STATUS_NEED_BUFFER;

	if ((res = spa_node_process(data->source)) < 0)
		printf("got pull error %d\n", res);
}

static const struct spa_node_callbacks source_callbacks = {
	SPA_VERSION_NODE_CALLBACKS,
	.done = on_source_done,
	.event = on_source_event,
	.have_output = on_source_have_output
};

static int do_add_source(struct spa_loop *loop, struct spa_source *source)
{
	struct data *data = SPA_CONTAINER_OF(loop, struct data, data_loop);

	data->sources[data->n_sources] = *source;
	data->n_sources++;
	data->rebuild_fds = true;

	return 0;
}

static int do_update_source(struct spa_source *source)
{
	return 0;
}

static void do_remove_source(struct spa_source *source)
{
}

static int
do_invoke(struct spa_loop *loop,
	  spa_invoke_func_t func, uint32_t seq, const void *data, size_t size, bool block, void *user_data)
{
	return func(loop, false, seq, data, size, user_data);
}

static int make_nodes(struct data *data, const char *device)
{
	int res;
	struct spa_pod *props;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[256];

	if ((res =
	     make_node(data, &data->source, "build/spa/plugins/v4l2/libspa-v4l2.so",
		       "v4l2-source")) < 0) {
		printf("can't create v4l2-source: %d\n", res);
		return res;
	}

	spa_node_set_callbacks(data->source, &source_callbacks, data);

	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	props = spa_pod_builder_object(&b,
		0, data->type.props,
		":", data->type.props_device, "s", device ? device : "/dev/video0");

	if ((res = spa_node_set_param(data->source, data->type.param.idProps, 0, props)) < 0)
		printf("got set_props error %d\n", res);

	return res;
}

static int setup_buffers(struct data *data)
{
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		struct buffer *b = &data->buffers[i];

		data->bp[i] = &b->buffer;

		b->texture = NULL;

		b->buffer.id = i;
		b->buffer.metas = b->metas;
		b->buffer.n_metas = 1;
		b->buffer.datas = b->datas;
		b->buffer.n_datas = 1;

		b->header.flags = 0;
		b->header.seq = 0;
		b->header.pts = 0;
		b->header.dts_offset = 0;
		b->metas[0].type = data->type.meta.Header;
		b->metas[0].data = &b->header;
		b->metas[0].size = sizeof(b->header);

		b->datas[0].type = 0;
		b->datas[0].flags = 0;
		b->datas[0].fd = -1;
		b->datas[0].mapoffset = 0;
		b->datas[0].maxsize = 0;
		b->datas[0].data = NULL;
		b->datas[0].chunk = &b->chunks[0];
		b->datas[0].chunk->offset = 0;
		b->datas[0].chunk->size = 0;
		b->datas[0].chunk->stride = 0;
	}
	data->n_buffers = MAX_BUFFERS;
	return 0;
}

static int sdl_alloc_buffers(struct data *data)
{
	int i;

	for (i = 0; i < MAX_BUFFERS; i++) {
		struct buffer *b = &data->buffers[i];
		SDL_Texture *texture;
		void *ptr;
		int stride;

		texture = SDL_CreateTexture(data->renderer,
					    SDL_PIXELFORMAT_YUY2,
					    SDL_TEXTUREACCESS_STREAMING, 320, 240);
		if (!texture) {
			printf("can't create texture: %s\n", SDL_GetError());
			return -ENOMEM;
		}
		if (SDL_LockTexture(texture, NULL, &ptr, &stride) < 0) {
			fprintf(stderr, "Couldn't lock texture: %s\n", SDL_GetError());
			return -EIO;
		}
		b->texture = texture;

		b->datas[0].type = data->type.data.MemPtr;
		b->datas[0].maxsize = stride * 240;
		b->datas[0].data = ptr;
		b->datas[0].chunk->offset = 0;
		b->datas[0].chunk->size = stride * 240;
		b->datas[0].chunk->stride = stride;
	}
	return 0;
}

static int negotiate_formats(struct data *data)
{
	int res;
	const struct spa_port_info *info;
	struct spa_pod *format;
	uint8_t buffer[256];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	data->source_output[0] = SPA_IO_BUFFERS_INIT;

	if ((res =
	     spa_node_port_set_io(data->source,
				  SPA_DIRECTION_OUTPUT, 0,
				  data->type.io.Buffers,
				  &data->source_output[0], sizeof(data->source_output[0]))) < 0)
		return res;

	format = spa_pod_builder_object(&b,
			0, data->type.format,
			"I", data->type.media_type.video,
			"I", data->type.media_subtype.raw,
			":", data->type.format_video.format,    "I", data->type.video_format.YUY2,
			":", data->type.format_video.size,      "R", &SPA_RECTANGLE(320, 240),
			":", data->type.format_video.framerate, "F", &SPA_FRACTION(25,1));

	if ((res = spa_node_port_set_param(data->source,
					   SPA_DIRECTION_OUTPUT, 0,
					   data->type.param.idFormat, 0,
					   format)) < 0)
		return res;

	if ((res = spa_node_port_get_info(data->source, SPA_DIRECTION_OUTPUT, 0, &info)) < 0)
		return res;


	setup_buffers(data);

	if (data->use_buffer) {
		if ((res = sdl_alloc_buffers(data)) < 0)
			return res;

		if ((res = spa_node_port_use_buffers(data->source, SPA_DIRECTION_OUTPUT, 0, data->bp,
					 data->n_buffers)) < 0) {
			printf("can't allocate buffers: %s\n", spa_strerror(res));
			return -1;
		}
	} else {
		unsigned int n_buffers;

		data->texture = SDL_CreateTexture(data->renderer,
						  SDL_PIXELFORMAT_YUY2,
						  SDL_TEXTUREACCESS_STREAMING, 320, 240);
		if (!data->texture) {
			printf("can't create texture: %s\n", SDL_GetError());
			return -1;
		}
		n_buffers = MAX_BUFFERS;
		if ((res =
		     spa_node_port_alloc_buffers(data->source, SPA_DIRECTION_OUTPUT, 0, NULL, 0,
						 data->bp, &n_buffers)) < 0) {
			printf("can't allocate buffers: %s\n", spa_strerror(res));
			return -1;
		}
		data->n_buffers = n_buffers;
	}
	return 0;
}

static void *loop(void *user_data)
{
	struct data *data = user_data;

	printf("enter thread\n");
	while (data->running) {
		int i, r;

		/* rebuild */
		if (data->rebuild_fds) {
			for (i = 0; i < data->n_sources; i++) {
				struct spa_source *p = &data->sources[i];
				data->fds[i].fd = p->fd;
				data->fds[i].events = p->mask;
			}
			data->n_fds = data->n_sources;
			data->rebuild_fds = false;
		}

		r = poll((struct pollfd *) data->fds, data->n_fds, -1);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (r == 0) {
			fprintf(stderr, "select timeout");
			break;
		}

		/* after */
		for (i = 0; i < data->n_sources; i++) {
			struct spa_source *p = &data->sources[i];
			p->rmask = 0;
			if (data->fds[i].revents & POLLIN)
				p->rmask |= SPA_IO_IN;
			if (data->fds[i].revents & POLLOUT)
				p->rmask |= SPA_IO_OUT;
			if (data->fds[i].revents & POLLHUP)
				p->rmask |= SPA_IO_HUP;
			if (data->fds[i].revents & POLLERR)
				p->rmask |= SPA_IO_ERR;
		}
		for (i = 0; i < data->n_sources; i++) {
			struct spa_source *p = &data->sources[i];
			if (p->rmask)
				p->func(p);
		}
	}
	printf("leave thread\n");
	return NULL;
}

static void run_async_source(struct data *data)
{
	int res;
	int err;

	{
		struct spa_command cmd = SPA_COMMAND_INIT(data->type.command_node.Start);
		if ((res = spa_node_send_command(data->source, &cmd)) < 0)
			printf("got error %d\n", res);
	}

	data->running = true;
	if ((err = pthread_create(&data->thread, NULL, loop, data)) != 0) {
		printf("can't create thread: %d %s", err, strerror(err));
		data->running = false;
	}

	sleep(10000);

	if (data->running) {
		data->running = false;
		pthread_join(data->thread, NULL);
	}

	{
		struct spa_command cmd = SPA_COMMAND_INIT(data->type.command_node.Pause);
		if ((res = spa_node_send_command(data->source, &cmd)) < 0)
			printf("got error %d\n", res);
	}
}

int main(int argc, char *argv[])
{
	struct data data = { 0 };
	int res;
	const char *str;

	data.use_buffer = true;

	data.map = &default_map.map;
	data.log = &default_log.log;

	if ((str = getenv("SPA_DEBUG")))
		data.log->level = atoi(str);

	data.data_loop.version = SPA_VERSION_LOOP;
	data.data_loop.add_source = do_add_source;
	data.data_loop.update_source = do_update_source;
	data.data_loop.remove_source = do_remove_source;
	data.data_loop.invoke = do_invoke;

	data.support[0].type = SPA_TYPE__TypeMap;
	data.support[0].data = data.map;
	data.support[1].type = SPA_TYPE__Log;
	data.support[1].data = data.log;
	data.support[2].type = SPA_TYPE_LOOP__DataLoop;
	data.support[2].data = &data.data_loop;
	data.support[3].type = SPA_TYPE_LOOP__MainLoop;
	data.support[3].data = &data.data_loop;
	data.n_support = 4;

	init_type(&data.type, data.map);

	if (SDL_Init(SDL_INIT_VIDEO) < 0) {
		printf("can't initialize SDL: %s\n", SDL_GetError());
		return -1;
	}

	if (SDL_CreateWindowAndRenderer
	    (320, 240, SDL_WINDOW_RESIZABLE, &data.window, &data.renderer)) {
		printf("can't create window: %s\n", SDL_GetError());
		return -1;
	}


	if ((res = make_nodes(&data, argv[1])) < 0) {
		printf("can't make nodes: %d\n", res);
		return -1;
	}
	if ((res = negotiate_formats(&data)) < 0) {
		printf("can't negotiate nodes: %d\n", res);
		return -1;
	}

	run_async_source(&data);

	SDL_DestroyRenderer(data.renderer);

	return 0;
}
