/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
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

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <pthread.h>
#include <limits.h>
#include <linux/videodev2.h>

#include "pipewire-v4l2.h"

#include <spa/utils/result.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/pod/filter.h>
#include <spa/param/video/format-utils.h>

#include <pipewire/pipewire.h>

PW_LOG_TOPIC_STATIC(v4l2_log_topic, "v4l2");
#define PW_LOG_TOPIC_DEFAULT v4l2_log_topic

#define MIN_BUFFERS	2u
#define MAX_BUFFERS	32u
#define DEFAULT_TIMEOUT	30

struct globals {
	pthread_mutex_t lock;

	struct fops old_fops;

	struct spa_list files;
	struct spa_list maps;
};

static struct globals globals;

struct global;

struct map {
	struct spa_list link;
	int ref;
	void *addr;
	struct file *file;
};

struct buffer {
	struct v4l2_buffer v4l2;
	struct pw_buffer *buf;
	uint32_t id;
};

struct file {
	struct spa_list link;
	int ref;

	struct pw_properties *props;
	struct pw_thread_loop *loop;
	struct pw_loop *l;
	struct pw_context *context;

	struct pw_core *core;
	struct spa_hook core_listener;

	int last_seq;
	int pending_seq;
	int error;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	struct spa_list globals;
	struct global *node;

	struct pw_stream *stream;
	struct spa_hook stream_listener;

	struct spa_video_info format;
	unsigned int have_format:1;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;
	uint32_t size;

	int fd;
};

#define MAX_PARAMS	32

struct global_info {
	const char *type;
	uint32_t version;
	const void *events;
	pw_destroy_t destroy;
	int (*init) (struct global *g);
};

struct global {
	struct spa_list link;

	struct file *file;

	const struct global_info *ginfo;

	uint32_t id;
	uint32_t permissions;
	struct pw_properties *props;

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
	struct spa_hook object_listener;

	int changed;
	void *info;
	struct spa_list param_list;
	int param_seq[MAX_PARAMS];

	union {
		struct {
#define NODE_FLAG_SOURCE	(1<<0)
#define NODE_FLAG_SINK		(1<<0)
			uint32_t flags;
			uint32_t device_id;
			int priority;
		} node;
	};
};

struct param {
	struct spa_list link;
	uint32_t id;
	struct spa_pod *param;
};

static uint32_t clear_params(struct spa_list *param_list, uint32_t id)
{
	struct param *p, *t;
	uint32_t count = 0;

	spa_list_for_each_safe(p, t, param_list, link) {
		if (id == SPA_ID_INVALID || p->id == id) {
			spa_list_remove(&p->link);
			free(p);
			count++;
		}
	}
	return count;
}

static struct param *add_param(struct spa_list *params,
		int seq, int *param_seq, uint32_t id, const struct spa_pod *param)
{
	struct param *p;

	if (id == SPA_ID_INVALID) {
		if (param == NULL || !spa_pod_is_object(param)) {
			errno = EINVAL;
			return NULL;
		}
		id = SPA_POD_OBJECT_ID(param);
	}

	if (id >= MAX_PARAMS) {
		pw_log_error("too big param id %d", id);
		errno = EINVAL;
		return NULL;
	}

	if (seq != param_seq[id]) {
		pw_log_debug("ignoring param %d, seq:%d != current_seq:%d",
				id, seq, param_seq[id]);
		errno = EBUSY;
		return NULL;
	}

	p = malloc(sizeof(*p) + (param != NULL ? SPA_POD_SIZE(param) : 0));
	if (p == NULL)
		return NULL;

	p->id = id;
	if (param != NULL) {
		p->param = SPA_PTROFF(p, sizeof(*p), struct spa_pod);
		memcpy(p->param, param, SPA_POD_SIZE(param));
	} else {
		clear_params(params, id);
		p->param = NULL;
	}
	spa_list_append(params, &p->link);

	return p;
}

#define ATOMIC_DEC(s)                   __atomic_sub_fetch(&(s), 1, __ATOMIC_SEQ_CST)
#define ATOMIC_INC(s)                   __atomic_add_fetch(&(s), 1, __ATOMIC_SEQ_CST)

static struct file *make_file(void)
{
	struct file *file;

	file = calloc(1, sizeof(*file));
	if (file == NULL)
		return NULL;

	file->ref = 1;
	file->fd = -1;
	spa_list_init(&file->link);
	spa_list_init(&file->globals);
	return file;
}

static void put_file(struct file *file)
{
	pthread_mutex_lock(&globals.lock);
	spa_list_append(&globals.files, &file->link);
	pthread_mutex_unlock(&globals.lock);
}

static struct file *find_file(int fd)
{
	struct file *f, *res = NULL;
	pthread_mutex_lock(&globals.lock);
	spa_list_for_each(f, &globals.files, link)
		if (f->fd == fd) {
			res = f;
			ATOMIC_INC(f->ref);
			break;
		}
	pthread_mutex_unlock(&globals.lock);
	return res;
}

static void free_file(struct file *file)
{
	struct map *m, *t;

	pthread_mutex_lock(&globals.lock);
	spa_list_remove(&file->link);
	spa_list_for_each_safe(m, t, &globals.maps, link) {
		if (m->file == file) {
			spa_list_remove(&m->link);
			free(m);
		}
	}
	pthread_mutex_unlock(&globals.lock);

	if (file->loop)
		pw_thread_loop_stop(file->loop);

	if (file->registry) {
		spa_hook_remove(&file->registry_listener);
		pw_proxy_destroy((struct pw_proxy*)file->registry);
	}
	if (file->stream) {
		spa_hook_remove(&file->stream_listener);
		pw_stream_destroy(file->stream);
	}
	if (file->core) {
		spa_hook_remove(&file->core_listener);
		pw_core_disconnect(file->core);
	}
	if (file->context)
		pw_context_destroy(file->context);
	if (file->loop)
		pw_thread_loop_destroy(file->loop);

	if (file->fd != -1)
		globals.old_fops.close(file->fd);
	free(file);
}

static void unref_file(struct file *file)
{
	if (ATOMIC_DEC(file->ref) <= 0)
		free_file(file);
}

static struct map *make_map(struct file *file, void *addr)
{
	struct map *map;

	map = calloc(1, sizeof(*map));
	if (map == NULL)
		return NULL;

	map->ref = 1;
	map->file = file;
	map->addr = addr;
	spa_list_init(&map->link);
	return map;
}

static void put_map(struct map *map)
{
	pthread_mutex_lock(&globals.lock);
	spa_list_append(&globals.maps, &map->link);
	pthread_mutex_unlock(&globals.lock);
}

static struct map *find_map(void *addr)
{
	struct map *m, *res = NULL;
	pthread_mutex_lock(&globals.lock);
	spa_list_for_each(m, &globals.maps, link)
		if (m->addr == addr) {
			res = m;
			ATOMIC_INC(m->ref);
			break;
		}
	pthread_mutex_unlock(&globals.lock);
	return res;
}

static void free_map(struct map *map)
{
	pthread_mutex_lock(&globals.lock);
	spa_list_remove(&map->link);
	pthread_mutex_unlock(&globals.lock);
	free(map);
}

static void unref_map(struct map *map)
{
	if (ATOMIC_DEC(map->ref) <= 0)
		free_map(map);
}

static void do_resync(struct file *file)
{
	file->pending_seq = pw_core_sync(file->core, PW_ID_CORE, file->pending_seq);
}

static int wait_resync(struct file *file)
{
	int res;
	do_resync(file);

	while (true) {
		pw_thread_loop_wait(file->loop);

		res = file->error;
		if (res < 0) {
			file->error = 0;
			return res;
		}
		if (file->pending_seq == file->last_seq)
			break;
	}
	return 0;
}

static void on_sync_reply(void *data, uint32_t id, int seq)
{
	struct file *file = data;

	if (id != PW_ID_CORE)
		return;

	file->last_seq = seq;
	if (file->pending_seq == seq)
		pw_thread_loop_signal(file->loop, false);
}

static void on_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct file *file = data;

	pw_log_warn("%p: error id:%u seq:%d res:%d (%s): %s", file,
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE) {
		switch (res) {
		case -ENOENT:
			break;
		default:
			file->error = res;
		}
	}
	pw_thread_loop_signal(file->loop, false);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_sync_reply,
	.error = on_error,
};

/** node */
static void node_event_info(void *object, const struct pw_node_info *info)
{
	struct global *g = object;
	struct file *file = g->file;
	const char *str;
	uint32_t i;

	info = g->info = pw_node_info_merge(g->info, info, g->changed == 0);

	pw_log_info("update %d %"PRIu64, g->id, info->change_mask);

	if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS && info->props) {
		if ((str = spa_dict_lookup(info->props, PW_KEY_DEVICE_ID)))
			g->node.device_id = atoi(str);
		else
			g->node.device_id = SPA_ID_INVALID;

		if ((str = spa_dict_lookup(info->props, PW_KEY_PRIORITY_SESSION)))
			g->node.priority = atoi(str);
		if ((str = spa_dict_lookup(info->props, PW_KEY_MEDIA_CLASS))) {
			if (spa_streq(str, "Video/Sink"))
				g->node.flags |= NODE_FLAG_SINK;
			else if (spa_streq(str, "Video/Source"))
				g->node.flags |= NODE_FLAG_SOURCE;
		}
	}
	if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			uint32_t id = info->params[i].id;
			int res;

			if (info->params[i].user == 0)
				continue;
			info->params[i].user = 0;

			if (id >= MAX_PARAMS) {
				pw_log_error("too big param id %d", id);
				continue;
			}

			if (id != SPA_PARAM_EnumFormat)
				continue;

			add_param(&g->param_list, g->param_seq[id], g->param_seq, id, NULL);
			if (!(info->params[i].flags & SPA_PARAM_INFO_READ))
				continue;

			res = pw_node_enum_params((struct pw_node*)g->proxy,
					++g->param_seq[id], id, 0, -1, NULL);
			if (SPA_RESULT_IS_ASYNC(res))
				g->param_seq[id] = res;
		}
	}
	do_resync(file);
}

static void node_event_param(void *object, int seq,
                uint32_t id, uint32_t index, uint32_t next,
                const struct spa_pod *param)
{
	struct global *g = object;

	pw_log_info("update param %d %d", g->id, id);
	add_param(&g->param_list, seq, g->param_seq, id, param);
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_event_info,
	.param = node_event_param,
};

static const struct global_info node_info = {
	.type = PW_TYPE_INTERFACE_Node,
	.version = PW_VERSION_NODE,
	.events = &node_events,
};

/** proxy */
static void proxy_removed(void *data)
{
	struct global *g = data;
	pw_proxy_destroy(g->proxy);
}

static void proxy_destroy(void *data)
{
	struct global *g = data;
	spa_list_remove(&g->link);
	g->proxy = NULL;
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = proxy_removed,
	.destroy = proxy_destroy
};

static void registry_event_global(void *data, uint32_t id,
		uint32_t permissions, const char *type, uint32_t version,
		const struct spa_dict *props)
{
	struct file *file = data;
	const struct global_info *info = NULL;
	struct pw_proxy *proxy;
	const char *str;

	pw_log_debug("got %d %s", id, type);

	if (spa_streq(type, PW_TYPE_INTERFACE_Node)) {
		if (file->node != NULL)
			return;

		if (props == NULL ||
		    ((str = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS)) == NULL) ||
		    ((!spa_streq(str, "Video/Sink")) &&
		     (!spa_streq(str, "Video/Source"))))
			return;

		pw_log_debug("found node %d type:%s", id, str);
		info = &node_info;
	}
	if (info) {
		struct global *g;

		proxy = pw_registry_bind(file->registry,
				id, info->type, info->version,
				sizeof(struct global));

		g = pw_proxy_get_user_data(proxy);
		g->file = file;
		g->ginfo = info;
		g->id = id;
		g->permissions = permissions;
		g->props = props ? pw_properties_new_dict(props) : NULL;
		g->proxy = proxy;
		spa_list_init(&g->param_list);
		spa_list_append(&file->globals, &g->link);

		pw_proxy_add_listener(proxy,
				&g->proxy_listener,
				&proxy_events, g);

		if (info->events) {
			pw_proxy_add_object_listener(proxy,
					&g->object_listener,
					info->events, g);
		}
		if (info->init)
			info->init(g);

		file->node = g;

		do_resync(file);
	}
}

static void registry_event_global_remove(void *object, uint32_t id)
{
}

static const struct pw_registry_events registry_events = {
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static int v4l2_openat(int dirfd, const char *path, int oflag, mode_t mode)
{
	int res;
	struct file *file;

	if (!spa_strstartswith(path, "/dev/video0"))
		return globals.old_fops.openat(dirfd, path, oflag, mode);

	if ((file = make_file()) == NULL)
		goto error;

	file->props = pw_properties_new(
			PW_KEY_CLIENT_API, "v4l2",
			NULL);
	file->loop = pw_thread_loop_new("v4l2", NULL);
	if (file->loop == NULL)
		goto error;

	file->l = pw_thread_loop_get_loop(file->loop);
	file->context = pw_context_new(file->l,
			pw_properties_copy(file->props), 0);
	if (file->context == NULL)
		goto error;

	pw_thread_loop_start(file->loop);

	pw_thread_loop_lock(file->loop);

	file->core = pw_context_connect(file->context,
			pw_properties_copy(file->props), 0);
	if (file->core == NULL)
		goto error_unlock;

	pw_core_add_listener(file->core,
			&file->core_listener,
			&core_events, file);
	file->registry = pw_core_get_registry(file->core,
			PW_VERSION_REGISTRY, 0);
	if (file->registry == NULL)
		goto error_unlock;

	pw_registry_add_listener(file->registry,
			&file->registry_listener,
			&registry_events, file);

	res = wait_resync(file);
	if (res < 0) {
		errno = -res;
		goto error_unlock;
	}
	if (file->node == NULL) {
		errno = -ENOENT;
		goto error_unlock;
	}
	pw_thread_loop_unlock(file->loop);

	res = file->fd = spa_system_eventfd_create(file->l->system,
		SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	if (res < 0)
		goto error;

	pw_log_info("path:%s oflag:%d mode:%d -> %d (%s)", path, oflag, mode,
			res, strerror(res < 0 ? errno : 0));

	put_file(file);

	return res;

error_unlock:
	pw_thread_loop_unlock(file->loop);
error:
	if (file)
		free_file(file);
	return -1;
}

static int v4l2_dup(int oldfd)
{
	int res;
	struct file *file;

	if ((file = find_file(oldfd)) == NULL)
		return globals.old_fops.dup(oldfd);

	res = globals.old_fops.dup(oldfd);

	pw_log_info("fd:%d -> %d (%s)", oldfd,
			res, strerror(res < 0 ? errno : 0));
	unref_file(file);

	return res;
}

static int v4l2_close(int fd)
{
	int res = 0;
	struct file *file;

	if ((file = find_file(fd)) == NULL)
		return globals.old_fops.close(fd);

	free_file(file);

	pw_log_info("fd:%d -> %d (%s)", fd,
			res, strerror(res < 0 ? errno : 0));
	return res;
}

#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))

static int vidioc_querycap(struct file *file, struct v4l2_capability *arg)
{
	int res = 0;

	spa_scnprintf((char*)arg->driver, sizeof(arg->driver), "%s", "pipewire");
	spa_scnprintf((char*)arg->card, sizeof(arg->card), "%s", "cam1");
	spa_scnprintf((char*)arg->bus_info, sizeof(arg->bus_info), "%s:%d", "pipewire", 1);

	arg->version = KERNEL_VERSION(5, 2, 0);
	arg->device_caps = V4L2_CAP_VIDEO_CAPTURE
		| V4L2_CAP_STREAMING
		| V4L2_CAP_EXT_PIX_FORMAT;
	arg->capabilities = arg->device_caps | V4L2_CAP_DEVICE_CAPS;
	memset(arg->reserved, 0, sizeof(arg->reserved));

	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}

static int vidioc_enum_framesizes(struct file *file, struct v4l2_frmsizeenum *arg)
{
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}


struct format_info {
	uint32_t fourcc;
	uint32_t media_type;
	uint32_t media_subtype;
	uint32_t format;
	const char *desc;
};

#define MAKE_FORMAT(fcc,mt,mst,fmt)	\
	{ V4L2_PIX_FMT_ ## fcc, SPA_MEDIA_TYPE_ ## mt, SPA_MEDIA_SUBTYPE_ ## mst, SPA_VIDEO_FORMAT_ ## fmt, #fcc }

static const struct format_info format_info[] = {
	/* RGB formats */
	MAKE_FORMAT(RGB332, video, raw, UNKNOWN),
	/* Luminance+Chrominance formats */
        MAKE_FORMAT(YUYV, video, raw, YUY2),
};

static const struct format_info *format_info_from_media_type(uint32_t type,
		uint32_t subtype, uint32_t format)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		if ((format_info[i].media_type == type) &&
		    (format_info[i].media_subtype == subtype) &&
		    (format == 0 || format_info[i].format == format))
			return &format_info[i];
	}
	return NULL;
}

static const struct format_info *format_info_from_fourcc(uint32_t fourcc)
{
	size_t i;
	for (i = 0; i < SPA_N_ELEMENTS(format_info); i++) {
		if (format_info[i].fourcc == fourcc)
			return &format_info[i];
	}
	return NULL;
}

static int param_to_info(const struct spa_pod *param, struct spa_video_info *info)
{
	int res;

	spa_zero(*info);
	if (spa_format_parse(param, &info->media_type, &info->media_subtype) < 0)
		return -EINVAL;

	if (info->media_type != SPA_MEDIA_TYPE_video)
		return -EINVAL;

	switch (info->media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		res = spa_format_video_raw_parse(param, &info->info.raw);
		break;
	case SPA_MEDIA_SUBTYPE_h264:
		res = spa_format_video_h264_parse(param, &info->info.h264);
		break;
	case SPA_MEDIA_SUBTYPE_mjpg:
	case SPA_MEDIA_SUBTYPE_jpeg:
		res = spa_format_video_mjpg_parse(param, &info->info.mjpg);
		break;
	default:
		return -EINVAL;
	}
	return res;
}

static void on_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct file *file = data;
	const struct spa_pod *params[4];
	uint32_t n_params = 0;
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t buffers, size;
	struct spa_video_info info;

	if (param == NULL || id != SPA_PARAM_Format)
		return;

	if (param_to_info(param, &info) < 0)
		return;

	file->format = info;
	file->have_format = true;

	buffers = 4;
	size = 0;

	params[n_params++] = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(buffers, 4, 4),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(size, 0, INT_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_CHOICE_RANGE_Int(0, 0, INT_MAX),
			SPA_PARAM_BUFFERS_align,   SPA_POD_Int(16),
			SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1<<SPA_DATA_MemFd)));

	pw_stream_update_params(file->stream, params, n_params);

}

static void on_stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct file *file = data;

	pw_log_info("%p: state %s", file, pw_stream_state_as_string(state));

	switch (state) {
	case PW_STREAM_STATE_ERROR:
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		break;
	case PW_STREAM_STATE_CONNECTING:
	case PW_STREAM_STATE_PAUSED:
	case PW_STREAM_STATE_STREAMING:
		break;
	}
	pw_thread_loop_signal(file->loop, false);
}

static void on_stream_add_buffer(void *data, struct pw_buffer *b)
{
	struct file *file = data;
	uint32_t id = file->n_buffers;
	struct buffer *buf = &file->buffers[id];
	struct v4l2_buffer vb;
	struct spa_data *d = &b->buffer->datas[0];

	file->size = d->maxsize;

	pw_log_info("%p: id:%d fd:%"PRIi64" size:%u", file, id, d->fd, file->size);

	spa_zero(vb);
	vb.index = id;
	vb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	vb.flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC;
	vb.memory = V4L2_MEMORY_MMAP;
	vb.m.offset = id * file->size;
	vb.length = file->size;

	buf->v4l2 = vb;
	buf->id = id;
	buf->buf = b;
	b->user_data = buf;

	file->n_buffers++;
}

static void on_stream_remove_buffer(void *data, struct pw_buffer *b)
{
	struct file *file = data;
	file->n_buffers--;
}

static void on_stream_process(void *data)
{
	struct file *file = data;
        spa_system_eventfd_write(file->l->system, file->fd, 1);
}

static const struct pw_stream_events stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.param_changed = on_stream_param_changed,
	.state_changed = on_stream_state_changed,
	.add_buffer = on_stream_add_buffer,
	.remove_buffer = on_stream_remove_buffer,
	.process = on_stream_process,
};

static int vidioc_enum_fmt(struct file *file, struct v4l2_fmtdesc *arg)
{
	uint32_t count = 0;
	struct global *g = file->node;
	struct param *p;

	pw_log_info("index: %u", arg->index);
	pw_log_info("type: %u", arg->type);

	if (arg->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	pw_thread_loop_lock(file->loop);
	spa_list_for_each(p, &g->param_list, link) {
		const struct format_info *fi;
		uint32_t media_type, media_subtype, format;

		if (p->id != SPA_PARAM_EnumFormat || p->param == NULL)
			continue;

		if (spa_format_parse(p->param, &media_type, &media_subtype) < 0)
			continue;
		if (media_type != SPA_MEDIA_TYPE_video)
			continue;
		if (media_subtype == SPA_MEDIA_SUBTYPE_raw) {
			if (spa_pod_parse_object(p->param,
					SPA_TYPE_OBJECT_Format, NULL,
					SPA_FORMAT_VIDEO_format, SPA_POD_Id(&format)) < 0)
				continue;
		} else {
			format = SPA_VIDEO_FORMAT_ENCODED;
		}

		pw_log_info("%d %d", count, arg->index);
		fi = format_info_from_media_type(media_type, media_subtype, format);
		if (fi == NULL)
			continue;

		arg->flags = fi->format == SPA_VIDEO_FORMAT_ENCODED ? V4L2_FMT_FLAG_COMPRESSED : 0;
		arg->pixelformat = fi->fourcc;
		if (count == arg->index)
			break;
		count++;
	}
	pw_thread_loop_unlock(file->loop);

	if (count != arg->index)
		return -EINVAL;

	pw_log_info("format: %u", arg->pixelformat);
	pw_log_info("flags: %u", arg->type);
	memset(arg->reserved, 0, sizeof(arg->reserved));

	return 0;
}

static int vidioc_g_fmt(struct file *file, struct v4l2_format *arg)
{
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}

static int format_to_info(struct v4l2_format *arg, struct spa_video_info *info)
{
	const struct format_info *fi;

	pw_log_info("type: %u", arg->type);
	pw_log_info("width: %u", arg->fmt.pix.width);
	pw_log_info("height: %u", arg->fmt.pix.height);
	pw_log_info("fmt: %u", arg->fmt.pix.pixelformat);

	if (arg->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	fi = format_info_from_fourcc(arg->fmt.pix.pixelformat);
	if (fi == NULL)
		return -EINVAL;

	spa_zero(*info);
	info->media_type = fi->media_type;
	info->media_subtype = fi->media_subtype;

	switch (info->media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		info->info.raw.format = fi->format;
		info->info.raw.size.width = arg->fmt.pix.width;
		info->info.raw.size.height = arg->fmt.pix.height;
		break;
	case SPA_MEDIA_SUBTYPE_h264:
		info->info.h264.size.width = arg->fmt.pix.width;
		info->info.h264.size.height = arg->fmt.pix.height;
		break;
	case SPA_MEDIA_SUBTYPE_mjpg:
	case SPA_MEDIA_SUBTYPE_jpeg:
		info->info.mjpg.size.width = arg->fmt.pix.width;
		info->info.mjpg.size.height = arg->fmt.pix.height;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static struct spa_pod *info_to_param(struct spa_pod_builder *builder, uint32_t id,
		struct spa_video_info *info)
{
	struct spa_pod *pod;

	if (info->media_type != SPA_MEDIA_TYPE_video)
		return NULL;

	switch (info->media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		pod = spa_format_video_raw_build(builder, id, &info->info.raw);
		break;
	case SPA_MEDIA_SUBTYPE_h264:
	case SPA_MEDIA_SUBTYPE_mjpg:
	case SPA_MEDIA_SUBTYPE_jpeg:
	default:
		return NULL;
	}
	return pod;
}

static int try_format(struct file *file, const struct spa_pod *pod)
{
	struct param *p;
	struct global *g = file->node;

	spa_list_for_each(p, &g->param_list, link) {
		char buffer[1024];
		struct spa_pod_builder b;
		struct spa_pod *res;

		if (p->id != SPA_PARAM_EnumFormat ||
		    p->param == NULL)
			continue;

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		if (spa_pod_filter(&b, &res, p->param, pod) >= 0)
			return 0;
	}
	return -EINVAL;
}

static int vidioc_s_fmt(struct file *file, struct v4l2_format *arg)
{
	int res;
	struct global *g = file->node;
	const struct spa_pod *params[1];
	const char *str;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct pw_properties *props;
	struct spa_video_info info;
	struct timespec abstime;
	const char *error = NULL;

	pw_thread_loop_lock(file->loop);
	if ((res = format_to_info(arg, &info)) < 0)
		goto exit_unlock;

	params[0] = info_to_param(&b, SPA_PARAM_EnumFormat, &info);
	if (params[0] == NULL)
		goto exit_unlock;

	if ((res = try_format(file, params[0])) < 0)
		goto exit_unlock;

	if (file->stream != NULL) {
		pw_stream_destroy(file->stream);
		file->stream = NULL;
	}

	props = NULL;
	if ((str = getenv("PIPEWIRE_PROPS")) != NULL)
		props = pw_properties_new_string(str);
	if (props == NULL)
		props = pw_properties_new(NULL, NULL);
	if (props == NULL) {
		res = -errno;
		goto exit_unlock;
	}

	pw_properties_set(props, PW_KEY_CLIENT_API, "v4l2");
	pw_properties_setf(props, PW_KEY_APP_NAME, "%s", pw_get_prgname());

	if (pw_properties_get(props, PW_KEY_MEDIA_TYPE) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_TYPE, "Video");
	if (pw_properties_get(props, PW_KEY_MEDIA_CATEGORY) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_CATEGORY, "Capture");

	file->stream = pw_stream_new(file->core, "v4l2 capture", props);
	if (file->stream == NULL) {
		res = -errno;
		goto exit_unlock;
	}

	pw_stream_add_listener(file->stream, &file->stream_listener, &stream_events, file);

	file->error = 0;

	pw_stream_connect(file->stream,
				PW_DIRECTION_INPUT,
				g->id,
				PW_STREAM_FLAG_DONT_RECONNECT |
				PW_STREAM_FLAG_AUTOCONNECT |
				PW_STREAM_FLAG_RT_PROCESS,
				params, 1);

	pw_thread_loop_get_time (file->loop, &abstime,
			DEFAULT_TIMEOUT * SPA_NSEC_PER_SEC);

	while (true) {
		enum pw_stream_state state = pw_stream_get_state(file->stream, &error);

		if (state == PW_STREAM_STATE_STREAMING)
			break;

		if (state == PW_STREAM_STATE_ERROR) {
			res = -EIO;
			goto exit_unlock;
		}
		if (file->error < 0) {
			res = file->error;
			goto exit_unlock;
		}
		if (pw_thread_loop_timed_wait_full(file->loop, &abstime) < 0) {
			res = -ETIMEDOUT;
			goto exit_unlock;
		}
	}
	/* pause stream */
	pw_stream_set_active(file->stream, false);

exit_unlock:
	pw_thread_loop_unlock(file->loop);

	return res;
}
static int vidioc_try_fmt(struct file *file, struct v4l2_format *arg)
{
	int res;
	struct spa_video_info info;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	if ((res = format_to_info(arg, &info)) < 0)
		goto exit;

	params[0] = info_to_param(&b, SPA_PARAM_EnumFormat, &info);
	if (params[0] == NULL)
		goto exit;

	pw_thread_loop_lock(file->loop);
	res = try_format(file, params[0]);
	pw_thread_loop_unlock(file->loop);
exit:
	return res;
}
static int vidioc_g_priority(struct file *file, enum v4l2_priority *arg)
{
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}
static int vidioc_s_priority(struct file *file, enum v4l2_priority *arg)
{
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}
static int vidioc_enuminput(struct file *file, struct v4l2_input *arg)
{
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}
static int vidioc_g_input(struct file *file, int *arg)
{
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}
static int vidioc_s_input(struct file *file, int *arg)
{
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}

static int vidioc_reqbufs(struct file *file, struct v4l2_requestbuffers *arg)
{
	pw_log_info("count: %u", arg->count);
	pw_log_info("type: %u", arg->type);
	pw_log_info("memory: %u", arg->memory);

	if (arg->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (arg->memory != V4L2_MEMORY_MMAP)
		return -EINVAL;

	pw_thread_loop_lock(file->loop);
	arg->count = file->n_buffers;
	pw_thread_loop_unlock(file->loop);

	arg->capabilities = V4L2_BUF_CAP_SUPPORTS_MMAP;
	memset(arg->reserved, 0, sizeof(arg->reserved));

	pw_log_info("result count: %u", arg->count);

	return 0;
}

static int vidioc_querybuf(struct file *file, struct v4l2_buffer *arg)
{
	int res;

	if (arg->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	pw_thread_loop_lock(file->loop);
	if (arg->index >= file->n_buffers) {
		res = -EINVAL;
		goto exit_unlock;
	}
	*arg = file->buffers[arg->index].v4l2;

	res = 0;

exit_unlock:
	pw_thread_loop_unlock(file->loop);

	return res;
}

static int vidioc_qbuf(struct file *file, struct v4l2_buffer *arg)
{
	int res = 0;
	struct buffer *buf;

	pw_thread_loop_lock(file->loop);
	if (arg->index >= file->n_buffers) {
		res = -EINVAL;
		goto exit;
	}
	buf = &file->buffers[arg->index];

	pw_log_info("file:%p %d -> %d (%s)", file, arg->index, res, spa_strerror(res));
	pw_stream_queue_buffer(file->stream, buf->buf);

exit:
	pw_thread_loop_unlock(file->loop);

	return res;
}
static int vidioc_dqbuf(struct file *file, struct v4l2_buffer *arg)
{
	int res = 0;
	struct pw_buffer *b;
	struct buffer *buf;
	uint64_t val;

	b = pw_stream_dequeue_buffer(file->stream);
	if (b == NULL)
		return -EAGAIN;

	spa_system_eventfd_read(file->l->system, file->fd, &val);

	buf = b->user_data;
	*arg = buf->v4l2;
	arg->bytesused = file->size;

	pw_log_info("file:%p %d -> %d (%s)", file, arg->index, res, spa_strerror(res));
	return res;
}

static int vidioc_streamon(struct file *file, int *arg)
{
	int res;

	pw_thread_loop_lock(file->loop);
	res = pw_stream_set_active(file->stream, true);
	pw_thread_loop_unlock(file->loop);

	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}
static int vidioc_streamoff(struct file *file, int *arg)
{
	int res;

	pw_thread_loop_lock(file->loop);
	res = pw_stream_set_active(file->stream, false);
	pw_thread_loop_unlock(file->loop);

	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}

static int v4l2_ioctl(int fd, unsigned long int request, void *arg)
{
	int res;
	struct file *file;

	if ((file = find_file(fd)) == NULL)
		return globals.old_fops.ioctl(fd, request, arg);

	if (arg == NULL && (_IOC_DIR(request) & (_IOC_WRITE | _IOC_READ))) {
		res = -EFAULT;
		goto done;
	}

	switch (request & 0xffffffff) {
	case VIDIOC_QUERYCAP:
		res = vidioc_querycap(file, (struct v4l2_capability *)arg);
		break;
	case VIDIOC_ENUM_FRAMESIZES:
		res = vidioc_enum_framesizes(file, (struct v4l2_frmsizeenum *)arg);
		break;
	case VIDIOC_ENUM_FMT:
		res = vidioc_enum_fmt(file, (struct v4l2_fmtdesc *)arg);
		break;
	case VIDIOC_G_FMT:
		res = vidioc_g_fmt(file, (struct v4l2_format *)arg);
		break;
	case VIDIOC_S_FMT:
		res = vidioc_s_fmt(file, (struct v4l2_format *)arg);
		break;
	case VIDIOC_TRY_FMT:
		res = vidioc_try_fmt(file, (struct v4l2_format *)arg);
		break;
	case VIDIOC_G_PRIORITY:
		res = vidioc_g_priority(file, (enum v4l2_priority *)arg);
		break;
	case VIDIOC_S_PRIORITY:
		res = vidioc_s_priority(file, (enum v4l2_priority *)arg);
		break;
	case VIDIOC_ENUMINPUT:
		res = vidioc_enuminput(file, (struct v4l2_input *)arg);
		break;
	case VIDIOC_G_INPUT:
		res = vidioc_g_input(file, (int *)arg);
		break;
	case VIDIOC_S_INPUT:
		res = vidioc_s_input(file, (int *)arg);
		break;
	case VIDIOC_REQBUFS:
		res = vidioc_reqbufs(file, (struct v4l2_requestbuffers *)arg);
		break;
	case VIDIOC_QUERYBUF:
		res = vidioc_querybuf(file, (struct v4l2_buffer *)arg);
		break;
	case VIDIOC_QBUF:
		res = vidioc_qbuf(file, (struct v4l2_buffer *)arg);
		break;
	case VIDIOC_DQBUF:
		res = vidioc_dqbuf(file, (struct v4l2_buffer *)arg);
		break;
	case VIDIOC_STREAMON:
		res = vidioc_streamon(file, (int *)arg);
		break;
	case VIDIOC_STREAMOFF:
		res = vidioc_streamoff(file, (int *)arg);
		break;
	default:
		res = -ENOTTY;
		break;
	}
done:
	if (res < 0) {
		errno = -res;
		res = -1;
	}
	pw_log_info("fd:%d request:%lx nr:%d arg:%p -> %d (%s)",
			fd, request, (int)_IOC_NR(request), arg,
			res, strerror(res < 0 ? errno : 0));

	unref_file(file);

	return res;
}

static void *v4l2_mmap(void *addr, size_t length, int prot,
			int flags, int fd, off64_t offset)
{
	void *res;
	struct file *file;
	struct map *map;
	uint32_t id;
	struct pw_map_range range;
	struct buffer *buf;
	struct spa_data *data;

	if ((file = find_file(fd)) == NULL)
		return globals.old_fops.mmap(addr, length, prot, flags, fd, offset);

	pw_thread_loop_lock(file->loop);
	if (file->size == 0) {
		errno = EIO;
		res = MAP_FAILED;
		goto error_unlock;
	}
	id = offset / file->size;
	if ((id * file->size) != offset || file->size != length) {
		errno = EINVAL;
		res = MAP_FAILED;
		goto error_unlock;
	}
	buf = &file->buffers[id];
	data = &buf->buf->buffer->datas[0];

        pw_map_range_init(&range, data->mapoffset, data->maxsize, 1024);

	prot = PROT_READ;

	res = globals.old_fops.mmap(addr, range.size, prot, flags, data->fd, range.offset);

	if ((map = make_map(res, file)) == NULL) {
		res = MAP_FAILED;
		goto error_unlock;
	}

	pw_log_info("addr:%p length:%u prot:%d flags:%d fd:%"PRIi64" offset:%u -> %p (%s)" ,
			addr, range.size, prot, flags, data->fd, range.offset,
			res, strerror(res == MAP_FAILED ? errno : 0));

	put_map(map);

error_unlock:
	pw_thread_loop_unlock(file->loop);
	unref_file(file);
	return res;
}

static int v4l2_munmap(void *addr, size_t length)
{
	int res;
	struct map *map;

	if ((map = find_map(addr)) == NULL)
		return globals.old_fops.munmap(addr, length);

	res = globals.old_fops.munmap(addr, length);

	pw_log_info("addr:%p length:%zu -> %d (%s)", addr, length,
			res, strerror(res < 0 ? errno : 0));

	unref_map(map);

	return res;
}

const struct fops fops = {
	.openat = v4l2_openat,
	.dup = v4l2_dup,
	.close = v4l2_close,
	.ioctl = v4l2_ioctl,
	.mmap = v4l2_mmap,
	.munmap = v4l2_munmap,
};

static void reg(void) __attribute__ ((constructor));
static void reg(void)
{
	globals.old_fops.openat = dlsym(RTLD_NEXT, "openat64");
	globals.old_fops.dup = dlsym(RTLD_NEXT, "dup");
	globals.old_fops.close = dlsym(RTLD_NEXT, "close");
	globals.old_fops.ioctl = dlsym(RTLD_NEXT, "ioctl");
	globals.old_fops.mmap = dlsym(RTLD_NEXT, "mmap64");
	globals.old_fops.munmap = dlsym(RTLD_NEXT, "munmap");

	pw_init(NULL, NULL);
	PW_LOG_TOPIC_INIT(v4l2_log_topic);

	pthread_mutex_init(&globals.lock, NULL);
	spa_list_init(&globals.files);
	spa_list_init(&globals.maps);
}
