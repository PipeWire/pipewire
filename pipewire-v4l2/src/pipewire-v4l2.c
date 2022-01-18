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

#define DEFAULT_DRIVER		"PipeWire"
#define DEFAULT_CARD		"PipeWire Camera"
#define DEFAULT_BUS_INFO	"PipeWire"

struct file_map {
	void *addr;
	struct file *file;
};

struct fd_map {
	int fd;
	struct file *file;
};

struct globals {
	struct fops old_fops;

	pthread_mutex_t lock;
	struct pw_array fd_maps;
	struct pw_array file_maps;
};

static struct globals globals;

struct global;

struct buffer_map {
	void *addr;
	uint32_t id;
};

struct buffer {
	struct v4l2_buffer v4l2;
	struct pw_buffer *buf;
	uint32_t id;
};

struct file {
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

	struct v4l2_format v4l2_format;
	uint32_t reqbufs;

	struct buffer buffers[MAX_BUFFERS];
	uint32_t n_buffers;
	uint32_t size;

	struct pw_array buffer_maps;

	uint32_t last_fourcc;

	unsigned int running:1;
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
	spa_list_init(&file->globals);
	pw_array_init(&file->buffer_maps, sizeof(struct buffer_map) * MAX_BUFFERS);
	return file;
}

static void free_file(struct file *file)
{
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
	if (file->fd != -1)
		spa_system_close(file->l->system, file->fd);
	if (file->loop)
		pw_thread_loop_destroy(file->loop);

	pw_array_clear(&file->buffer_maps);
	free(file);
}

static void unref_file(struct file *file)
{
	if (ATOMIC_DEC(file->ref) <= 0)
		free_file(file);
}

static int add_fd_map(int fd, struct file *file)
{
	struct fd_map *map;
	pthread_mutex_lock(&globals.lock);
	map = pw_array_add(&globals.fd_maps, sizeof(*map));
	if (map != NULL) {
		map->fd = fd;
		map->file = file;
		ATOMIC_INC(file->ref);
	}
	pthread_mutex_unlock(&globals.lock);
	return 0;
}

/* must be called with `globals.lock` held */
static struct fd_map *find_fd_map_unlocked(int fd)
{
	struct fd_map *map;

	pw_array_for_each(map, &globals.fd_maps) {
		if (map->fd == fd) {
			ATOMIC_INC(map->file->ref);
			return map;
		}
	}

	return NULL;
}

static struct file *find_file(int fd)
{
	pthread_mutex_lock(&globals.lock);

	struct fd_map *map = find_fd_map_unlocked(fd);
	struct file *file = NULL;

	if (map != NULL)
		file = map->file;

	pthread_mutex_unlock(&globals.lock);

	return file;
}

static struct file *remove_fd_map(int fd)
{
	pthread_mutex_lock(&globals.lock);

	struct fd_map *map = find_fd_map_unlocked(fd);
	struct file *file = NULL;

	if (map != NULL) {
		file = map->file;
		pw_array_remove(&globals.fd_maps, map);
	}

	pthread_mutex_unlock(&globals.lock);

	if (file != NULL)
		unref_file(file);

	return file;
}

static int add_file_map(void *addr, struct file *file)
{
	struct file_map *map;
	pthread_mutex_lock(&globals.lock);
	map = pw_array_add(&globals.file_maps, sizeof(*map));
	if (map != NULL) {
		map->addr = addr;
		map->file = file;
	}
	pthread_mutex_unlock(&globals.lock);
	return 0;
}

/* must be called with `globals.lock` held */
static struct file_map *find_file_map_unlocked(void *addr)
{
	struct file_map *map;

	pw_array_for_each(map, &globals.file_maps) {
		if (map->addr == addr)
			return map;
	}

	return NULL;
}
static struct file *remove_file_map(void *addr)
{
	pthread_mutex_lock(&globals.lock);

	struct file_map *map = find_file_map_unlocked(addr);
	struct file *file = NULL;

	if (map != NULL) {
		file = map->file;
		pw_array_remove(&globals.file_maps, map);
	}

	pthread_mutex_unlock(&globals.lock);

	return file;
}

static int add_buffer_map(struct file *file, void *addr, uint32_t id)
{
	struct buffer_map *map;
	map = pw_array_add(&file->buffer_maps, sizeof(*map));
	if (map != NULL) {
		map->addr = addr;
		map->id = id;
	}
	return 0;
}
static struct buffer_map *find_buffer_map(struct file *file, void *addr)
{
	struct buffer_map *map;
	pw_array_for_each(map, &file->buffer_maps) {
		if (map->addr == addr)
			return map;
	}
	return NULL;
}
static void remove_buffer_map(struct file *file, struct buffer_map *map)
{
	pw_array_remove(&file->buffer_maps, map);
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

	pw_log_debug("update %d %"PRIu64, g->id, info->change_mask);

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

	pw_log_debug("update param %d %d %d %d", g->id, id, seq, g->param_seq[id]);
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

static struct global *find_global(struct file *file, uint32_t id)
{
	struct global *g;
	spa_list_for_each(g, &file->globals, link) {
		if (g->id == id)
			return g;
	}
	return NULL;
}

static void registry_event_global_remove(void *data, uint32_t id)
{
	struct file *file = data;
	struct global *g;

	if ((g = find_global(file, id)) == NULL)
		return;

	pw_proxy_destroy(g->proxy);
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

	add_fd_map(res, file);

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

	res = globals.old_fops.dup(oldfd);
	if (res < 0)
		return res;

	if ((file = find_file(oldfd)) != NULL) {
		add_fd_map(res, file);
		unref_file(file);
		pw_log_info("fd:%d -> %d (%s)", oldfd,
				res, strerror(res < 0 ? errno : 0));
	}
	return res;
}

static int v4l2_close(int fd)
{
	struct file *file;

	if ((file = remove_fd_map(fd)) == NULL)
		return globals.old_fops.close(fd);

	if (fd != file->fd)
		spa_system_close(file->l->system, fd);

	unref_file(file);

	return 0;
}

#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))

static int vidioc_querycap(struct file *file, struct v4l2_capability *arg)
{
	int res = 0;

	spa_scnprintf((char*)arg->driver, sizeof(arg->driver), "%s", DEFAULT_DRIVER);
	spa_scnprintf((char*)arg->card, sizeof(arg->card), "%s", DEFAULT_CARD);
	spa_scnprintf((char*)arg->bus_info, sizeof(arg->bus_info), "%s:%d", DEFAULT_BUS_INFO, 1);

	arg->version = KERNEL_VERSION(5, 2, 0);
	arg->device_caps = V4L2_CAP_VIDEO_CAPTURE
		| V4L2_CAP_STREAMING
		| V4L2_CAP_EXT_PIX_FORMAT;
	arg->capabilities = arg->device_caps | V4L2_CAP_DEVICE_CAPS;
	memset(arg->reserved, 0, sizeof(arg->reserved));

	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}

struct format_info {
	uint32_t fourcc;
	uint32_t media_type;
	uint32_t media_subtype;
	uint32_t format;
	uint32_t bpp;
	const char *desc;
};

#define MAKE_FORMAT(fcc,mt,mst,bpp,fmt)	\
	{ V4L2_PIX_FMT_ ## fcc, SPA_MEDIA_TYPE_ ## mt, SPA_MEDIA_SUBTYPE_ ## mst, SPA_VIDEO_FORMAT_ ## fmt, bpp, #fcc }

static const struct format_info format_info[] = {
	/* RGB formats */
	MAKE_FORMAT(RGB332, video, raw, 4, UNKNOWN),
	MAKE_FORMAT(ARGB555, video, raw, 4, UNKNOWN),
	MAKE_FORMAT(XRGB555, video, raw, 4, RGB15),
	MAKE_FORMAT(ARGB555X, video, raw, 4, UNKNOWN),
	MAKE_FORMAT(XRGB555X, video, raw, 4, BGR15),
	MAKE_FORMAT(RGB565, video, raw, 4, RGB16),
	MAKE_FORMAT(RGB565X, video, raw, 4, UNKNOWN),
	MAKE_FORMAT(BGR666, video, raw, 4, UNKNOWN),
	MAKE_FORMAT(BGR24, video, raw, 4, BGR),
	MAKE_FORMAT(RGB24, video, raw, 4, RGB),
	MAKE_FORMAT(ABGR32, video, raw, 4, BGRA),
	MAKE_FORMAT(XBGR32, video, raw, 4, BGRx),
	MAKE_FORMAT(ARGB32, video, raw, 4, ARGB),
	MAKE_FORMAT(XRGB32, video, raw, 4, xRGB),

	/* Deprecated Packed RGB Image Formats (alpha ambiguity) */
	MAKE_FORMAT(RGB444, video, raw, 2, UNKNOWN),
	MAKE_FORMAT(RGB555, video, raw, 2, RGB15),
	MAKE_FORMAT(RGB555X, video, raw, 2, BGR15),
	MAKE_FORMAT(BGR32, video, raw, 4, BGRx),
	MAKE_FORMAT(RGB32, video, raw, 4, xRGB),

        /* Grey formats */
	MAKE_FORMAT(GREY, video, raw, 1, GRAY8),
	MAKE_FORMAT(Y4, video, raw, 1, UNKNOWN),
	MAKE_FORMAT(Y6, video, raw, 1, UNKNOWN),
	MAKE_FORMAT(Y10, video, raw, 2, UNKNOWN),
	MAKE_FORMAT(Y12, video, raw, 2, UNKNOWN),
	MAKE_FORMAT(Y16, video, raw, 2, GRAY16_LE),
	MAKE_FORMAT(Y16_BE, video, raw, 2, GRAY16_BE),
	MAKE_FORMAT(Y10BPACK, video, raw, 2, UNKNOWN),

	/* Palette formats */
	MAKE_FORMAT(PAL8, video, raw, 1, UNKNOWN),

	/* Chrominance formats */
	MAKE_FORMAT(UV8, video, raw, 2, UNKNOWN),

	/* Luminance+Chrominance formats */
	MAKE_FORMAT(YVU410, video, raw, 1, YVU9),
	MAKE_FORMAT(YVU420, video, raw, 1, YV12),
	MAKE_FORMAT(YVU420M, video, raw, 1, UNKNOWN),
	MAKE_FORMAT(YUYV, video, raw, 2, YUY2),
	MAKE_FORMAT(YYUV, video, raw, 2, UNKNOWN),
	MAKE_FORMAT(YVYU, video, raw, 2, YVYU),
	MAKE_FORMAT(UYVY, video, raw, 2, UYVY),
	MAKE_FORMAT(VYUY, video, raw, 2, UNKNOWN),
	MAKE_FORMAT(YUV422P, video, raw, 1, Y42B),
	MAKE_FORMAT(YUV411P, video, raw, 1, Y41B),
	MAKE_FORMAT(Y41P, video, raw, 1, UNKNOWN),
	MAKE_FORMAT(YUV444, video, raw, 1, UNKNOWN),
	MAKE_FORMAT(YUV555, video, raw, 1, UNKNOWN),
	MAKE_FORMAT(YUV565, video, raw, 1, UNKNOWN),
	MAKE_FORMAT(YUV32, video, raw, 1, UNKNOWN),
	MAKE_FORMAT(YUV410, video, raw, 1, YUV9),
	MAKE_FORMAT(YUV420, video, raw, 1, I420),
	MAKE_FORMAT(YUV420M, video, raw, 1, I420),
	MAKE_FORMAT(HI240, video, raw, 1, UNKNOWN),
	MAKE_FORMAT(HM12, video, raw, 1, UNKNOWN),
	MAKE_FORMAT(M420, video, raw, 1, UNKNOWN),

	/* two planes -- one Y, one Cr + Cb interleaved  */
	MAKE_FORMAT(NV12, video, raw, 1, NV12),
	MAKE_FORMAT(NV12M, video, raw, 1,  NV12),
	MAKE_FORMAT(NV12MT, video, raw, 1,  NV12_64Z32),
	MAKE_FORMAT(NV12MT_16X16, video, raw, 1, UNKNOWN),
	MAKE_FORMAT(NV21, video, raw, 1, NV21),
	MAKE_FORMAT(NV21M, video, raw, 1, NV21),
	MAKE_FORMAT(NV16, video, raw, 1, NV16),
	MAKE_FORMAT(NV16M, video, raw, 1, NV16),
	MAKE_FORMAT(NV61, video, raw, 1, NV61),
	MAKE_FORMAT(NV61M, video, raw, 1, NV61),
	MAKE_FORMAT(NV24, video, raw, 1, NV24),
	MAKE_FORMAT(NV42, video, raw, 1, UNKNOWN),

	/* Bayer formats - see http://www.siliconimaging.com/RGB%20Bayer.htm */
	MAKE_FORMAT(SBGGR8, video, bayer, 1, UNKNOWN),
	MAKE_FORMAT(SGBRG8, video, bayer, 1, UNKNOWN),
	MAKE_FORMAT(SGRBG8, video, bayer, 1, UNKNOWN),
	MAKE_FORMAT(SRGGB8, video, bayer, 1, UNKNOWN),

	/* compressed formats */
	MAKE_FORMAT(MJPEG, video, mjpg, 1, ENCODED),
	MAKE_FORMAT(JPEG, video, mjpg, 1, ENCODED),
	MAKE_FORMAT(PJPG, video, mjpg, 1, ENCODED),
	MAKE_FORMAT(DV, video, dv, 1, ENCODED),
	MAKE_FORMAT(MPEG, video, mpegts, 1, ENCODED),
	MAKE_FORMAT(H264, video, h264, 1, ENCODED),
	MAKE_FORMAT(H264_NO_SC, video, h264, 1, ENCODED),
	MAKE_FORMAT(H264_MVC, video, h264, 1, ENCODED),
	MAKE_FORMAT(H263, video, h263, 1, ENCODED),
	MAKE_FORMAT(MPEG1, video, mpeg1, 1, ENCODED),
	MAKE_FORMAT(MPEG2, video, mpeg2, 1, ENCODED),
	MAKE_FORMAT(MPEG4, video, mpeg4, 1, ENCODED),
	MAKE_FORMAT(XVID, video, xvid, 1, ENCODED),
	MAKE_FORMAT(VC1_ANNEX_G, video, vc1, 1, ENCODED),
	MAKE_FORMAT(VC1_ANNEX_L, video, vc1, 1, ENCODED),
	MAKE_FORMAT(VP8, video, vp8, 1, ENCODED),

	/*  Vendor-specific formats   */
	MAKE_FORMAT(WNVA, video, raw, 1, UNKNOWN),
	MAKE_FORMAT(SN9C10X, video, raw, 1, UNKNOWN),
	MAKE_FORMAT(PWC1, video, raw, 1, UNKNOWN),
	MAKE_FORMAT(PWC2, video, raw, 1, UNKNOWN),
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

static int format_to_info(const struct v4l2_format *arg, struct spa_video_info *info)
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
	case SPA_MEDIA_SUBTYPE_mjpg:
	case SPA_MEDIA_SUBTYPE_jpeg:
		pod = spa_format_video_mjpg_build(builder, id, &info->info.mjpg);
		break;
	case SPA_MEDIA_SUBTYPE_h264:
		pod = spa_format_video_h264_build(builder, id, &info->info.h264);
		break;
	default:
		return NULL;
	}
	return pod;
}

static struct spa_pod *fmt_to_param(struct spa_pod_builder *builder, uint32_t id,
		const struct v4l2_format *fmt)
{
	struct spa_video_info info;
	if (format_to_info(fmt, &info) < 0)
		return NULL;
	return info_to_param(builder, id, &info);
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

static int info_to_fmt(const struct spa_video_info *info, struct v4l2_format *fmt)
{
	const struct format_info *fi;
	uint32_t format;

	if (info->media_type != SPA_MEDIA_TYPE_video)
		return -EINVAL;

	if (info->media_subtype == SPA_MEDIA_SUBTYPE_raw) {
		format = info->info.raw.format;
	} else {
		format = SPA_VIDEO_FORMAT_ENCODED;
	}

	fi = format_info_from_media_type(info->media_type, info->media_subtype,
			format);
	if (fi == NULL)
		return -EINVAL;

	spa_zero(*fmt);
	fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt->fmt.pix.pixelformat = fi->fourcc;
	fmt->fmt.pix.field = V4L2_FIELD_NONE;

	switch (info->media_subtype) {
	case SPA_MEDIA_SUBTYPE_raw:
		fmt->fmt.pix.width = info->info.raw.size.width;
		fmt->fmt.pix.height = info->info.raw.size.height;
		break;
	case SPA_MEDIA_SUBTYPE_mjpg:
	case SPA_MEDIA_SUBTYPE_jpeg:
		fmt->fmt.pix.width = info->info.mjpg.size.width;
		fmt->fmt.pix.height = info->info.mjpg.size.height;
		break;
	case SPA_MEDIA_SUBTYPE_h264:
		fmt->fmt.pix.width = info->info.h264.size.width;
		fmt->fmt.pix.height = info->info.h264.size.height;
		break;
	default:
		return -EINVAL;
	}
	fmt->fmt.pix.bytesperline = SPA_ROUND_UP_N(fmt->fmt.pix.width, 4) * fi->bpp;
	fmt->fmt.pix.sizeimage = fmt->fmt.pix.bytesperline *
		SPA_ROUND_UP_N(fmt->fmt.pix.height, 2);
	return 0;
}

static int param_to_fmt(const struct spa_pod *param, struct v4l2_format *fmt)
{
	struct spa_video_info info;
	struct spa_pod *copy;
	int res;

	copy = spa_pod_copy(param);
	spa_pod_fixate(copy);
	res = param_to_info(copy, &info);
	free(copy);

	if (res < 0)
		return -EINVAL;
	if (info_to_fmt(&info, fmt) < 0)
		return -EINVAL;
	return 0;
}

static void on_stream_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct file *file = data;
	const struct spa_pod *params[4];
	uint32_t n_params = 0;
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	uint32_t buffers, size;
	struct v4l2_format fmt;

	if (param == NULL || id != SPA_PARAM_Format)
		return;

	if (param_to_fmt(param, &fmt) < 0)
		return;

	file->v4l2_format = fmt;

	buffers = SPA_CLAMP(file->reqbufs, 2u, MAX_BUFFERS);
	size = 0;

	params[n_params++] = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(buffers,
							2, MAX_BUFFERS),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_CHOICE_RANGE_Int(size, 0, INT_MAX),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_CHOICE_RANGE_Int(0, 0, INT_MAX),
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

static int vidioc_enum_framesizes(struct file *file, struct v4l2_frmsizeenum *arg)
{
	uint32_t count = 0;
	struct global *g = file->node;
	struct param *p;
	bool found = false;

	pw_log_info("index: %u", arg->index);
	pw_log_info("format: %.4s", (char*)&arg->pixel_format);

	pw_thread_loop_lock(file->loop);
	spa_list_for_each(p, &g->param_list, link) {
		const struct format_info *fi;
		uint32_t media_type, media_subtype, format;
		struct spa_rectangle size;

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

		fi = format_info_from_media_type(media_type, media_subtype, format);
		if (fi == NULL)
			continue;

		if (fi->fourcc != arg->pixel_format)
			continue;
		if (spa_pod_parse_object(p->param,
				SPA_TYPE_OBJECT_Format, NULL,
				SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&size)) < 0)
			continue;

		arg->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		arg->discrete.width = size.width;
		arg->discrete.height = size.height;

		pw_log_debug("count:%d %d %dx%d", count, fi->fourcc,
				size.width, size.height);
		if (count == arg->index) {
			found = true;
			break;
		}
		count++;
	}
	pw_thread_loop_unlock(file->loop);

	if (!found)
		return -EINVAL;

	switch (arg->type) {
	case V4L2_FRMSIZE_TYPE_DISCRETE:
		pw_log_info("type: discrete");
		pw_log_info("width: %u", arg->discrete.width);
		pw_log_info("height: %u", arg->discrete.height);
		break;
	case V4L2_FRMSIZE_TYPE_CONTINUOUS:
	case V4L2_FRMSIZE_TYPE_STEPWISE:
		pw_log_info("type: stepwise");
		pw_log_info("min-width: %u", arg->stepwise.min_width);
		pw_log_info("max-width: %u", arg->stepwise.max_width);
		pw_log_info("step-width: %u", arg->stepwise.step_width);
		pw_log_info("min-height: %u", arg->stepwise.min_height);
		pw_log_info("max-height: %u", arg->stepwise.max_height);
		pw_log_info("step-height: %u", arg->stepwise.step_height);
		break;
	}

	memset(arg->reserved, 0, sizeof(arg->reserved));

	return 0;
}

static int vidioc_enum_fmt(struct file *file, struct v4l2_fmtdesc *arg)
{
	uint32_t count = 0, last_fourcc = 0;
	struct global *g = file->node;
	struct param *p;
	bool found = false;

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

		fi = format_info_from_media_type(media_type, media_subtype, format);
		if (fi == NULL)
			continue;

		if (fi->fourcc == last_fourcc)
			continue;
		pw_log_info("count:%d %d %d", count, fi->fourcc, last_fourcc);

		arg->flags = fi->format == SPA_VIDEO_FORMAT_ENCODED ? V4L2_FMT_FLAG_COMPRESSED : 0;
		arg->pixelformat = fi->fourcc;
		last_fourcc = fi->fourcc;
		if (count == arg->index) {
			found = true;
			break;
		}
		count++;
	}
	pw_thread_loop_unlock(file->loop);

	if (!found)
		return -EINVAL;

	pw_log_info("format: %u", arg->pixelformat);
	pw_log_info("flags: %u", arg->type);
	memset(arg->reserved, 0, sizeof(arg->reserved));

	return 0;
}

static int vidioc_g_fmt(struct file *file, struct v4l2_format *arg)
{
	if (arg->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	*arg = file->v4l2_format;
	return 0;
}

static int score_diff(struct v4l2_format *fmt, struct v4l2_format *tmp)
{
	int score = 0;
	if (fmt->fmt.pix.pixelformat != tmp->fmt.pix.pixelformat)
		score += 20000;
	score += SPA_ABS((int)fmt->fmt.pix.width - (int)tmp->fmt.pix.width);
	score += SPA_ABS((int)fmt->fmt.pix.height - (int)tmp->fmt.pix.height);
	return score;
}

static int try_format(struct file *file, struct v4l2_format *fmt)
{
	struct param *p;
	struct global *g = file->node;
	struct v4l2_format best_fmt = *fmt;
	int best = -1;

	pw_log_info("in: type: %u", fmt->type);
	pw_log_info("in: format: %.4s", (char*)&fmt->fmt.pix.pixelformat);
	pw_log_info("in: width: %u", fmt->fmt.pix.width);
	pw_log_info("in: height: %u", fmt->fmt.pix.height);
	pw_log_info("in: field: %u", fmt->fmt.pix.field);
	spa_list_for_each(p, &g->param_list, link) {
		struct v4l2_format tmp;
		int score;

		if (p->id != SPA_PARAM_EnumFormat || p->param == NULL)
			continue;

		if (param_to_fmt(p->param, &tmp) < 0)
			continue;

		score = score_diff(fmt, &tmp);
		pw_log_debug("check: type: %u", tmp.type);
		pw_log_debug("check: format: %.4s", (char*)&tmp.fmt.pix.pixelformat);
		pw_log_debug("check: width: %u", tmp.fmt.pix.width);
		pw_log_debug("check: height: %u", tmp.fmt.pix.height);
		pw_log_debug("check: score: %d best:%d", score, best);

		if (best == -1 || score < best) {
			best = score;
			best_fmt = tmp;
		}
	}
	*fmt = best_fmt;
	pw_log_info("out: format: %.4s", (char*)&fmt->fmt.pix.pixelformat);
	pw_log_info("out: width: %u", fmt->fmt.pix.width);
	pw_log_info("out: height: %u", fmt->fmt.pix.height);
	pw_log_info("out: field: %u", fmt->fmt.pix.field);
	pw_log_info("out: size: %u", fmt->fmt.pix.sizeimage);
	return 0;
}

static int disconnect_stream(struct file *file)
{
	if (file->stream != NULL) {
		pw_stream_destroy(file->stream);
		file->stream = NULL;
		file->n_buffers = 0;
	}
	return 0;
}

static int connect_stream(struct file *file)
{
	int res;
	struct global *g = file->node;
	const char *str;
	struct timespec abstime;
	const char *error = NULL;
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const struct spa_pod *params[1];
	struct pw_properties *props;

	params[0] = fmt_to_param(&b, SPA_PARAM_EnumFormat, &file->v4l2_format);
	if (params[0] == NULL) {
		res = -EINVAL;
		goto exit;
	}

	disconnect_stream(file);

	props = NULL;
	if ((str = getenv("PIPEWIRE_PROPS")) != NULL)
		props = pw_properties_new_string(str);
	if (props == NULL)
		props = pw_properties_new(NULL, NULL);
	if (props == NULL) {
		res = -errno;
		goto exit;
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
		goto exit;
	}

	pw_stream_add_listener(file->stream,
			&file->stream_listener,
			&stream_events, file);

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
			goto exit;
		}
		if (file->error < 0) {
			res = file->error;
			goto exit;
		}
		if (pw_thread_loop_timed_wait_full(file->loop, &abstime) < 0) {
			res = -ETIMEDOUT;
			goto exit;
		}
	}
	/* pause stream */
	res = pw_stream_set_active(file->stream, false);
exit:
	return res;
}

static int vidioc_s_fmt(struct file *file, struct v4l2_format *arg)
{
	int res;

	pw_thread_loop_lock(file->loop);
	if ((res = try_format(file, arg)) < 0)
		goto exit_unlock;

	file->v4l2_format = *arg;

exit_unlock:
	pw_thread_loop_unlock(file->loop);
	return res;
}
static int vidioc_try_fmt(struct file *file, struct v4l2_format *arg)
{
	int res;

	pw_thread_loop_lock(file->loop);
	res = try_format(file, arg);
	pw_thread_loop_unlock(file->loop);
	return res;
}

static int vidioc_enuminput(struct file *file, struct v4l2_input *arg)
{
	if (arg->index != 0)
		return -EINVAL;

	spa_zero(*arg);
	spa_scnprintf((char*)arg->name, sizeof(arg->name), "%s", DEFAULT_CARD);
        arg->type = V4L2_INPUT_TYPE_CAMERA;

        return 0;
}
static int vidioc_g_input(struct file *file, int *arg)
{
	*arg = 0;
        return 0;
}
static int vidioc_s_input(struct file *file, int *arg)
{
	if (*arg != 0)
		return -EINVAL;
        return 0;
}

static int vidioc_reqbufs(struct file *file, struct v4l2_requestbuffers *arg)
{
	int res;

	pw_log_info("count: %u", arg->count);
	pw_log_info("type: %u", arg->type);
	pw_log_info("memory: %u", arg->memory);

	if (arg->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (arg->memory != V4L2_MEMORY_MMAP)
		return -EINVAL;

	pw_thread_loop_lock(file->loop);

	if (arg->count == 0) {
		if (pw_array_get_len(&file->buffer_maps, struct buffer_map) != 0) {
			res = -EBUSY;
			goto exit_unlock;
		}
		if (file->running) {
			res = -EBUSY;
			goto exit_unlock;
		}
		file->reqbufs = 0;
		res = disconnect_stream(file);
	} else {
		file->reqbufs = arg->count;

		if ((res = connect_stream(file)) < 0)
			goto exit_unlock;

		arg->count = file->n_buffers;
	}
#ifdef V4L2_BUF_CAP_SUPPORTS_MMAP
	arg->capabilities = V4L2_BUF_CAP_SUPPORTS_MMAP;
#endif
	memset(arg->reserved, 0, sizeof(arg->reserved));

	pw_log_info("result count: %u", arg->count);

exit_unlock:
	pw_thread_loop_unlock(file->loop);
	return res;
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

	if (arg->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (arg->memory != V4L2_MEMORY_MMAP)
		return -EINVAL;

	pw_thread_loop_lock(file->loop);
	if (arg->index >= file->n_buffers) {
		res = -EINVAL;
		goto exit;
	}
	buf = &file->buffers[arg->index];

	if (SPA_FLAG_IS_SET(buf->v4l2.flags, V4L2_BUF_FLAG_QUEUED)) {
		res = -EINVAL;
		goto exit;
	}

	SPA_FLAG_SET(buf->v4l2.flags, V4L2_BUF_FLAG_QUEUED);
	arg->flags = buf->v4l2.flags;

	pw_stream_queue_buffer(file->stream, buf->buf);
	pw_log_debug("file:%p %d -> %d (%s)", file, arg->index, res, spa_strerror(res));

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
	struct spa_data *d;

	if (arg->type != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;
	if (arg->memory != V4L2_MEMORY_MMAP)
		return -EINVAL;

	pw_thread_loop_lock(file->loop);
	if (arg->index >= file->n_buffers) {
		res = -EINVAL;
		goto exit_unlock;
	}
	if (!file->running) {
		res = -EINVAL;
		goto exit_unlock;
	}

	b = pw_stream_dequeue_buffer(file->stream);
	if (b == NULL) {
		res = -EAGAIN;
		goto exit_unlock;
	}
	spa_system_eventfd_read(file->l->system, file->fd, &val);

	buf = b->user_data;
	d = &buf->buf->buffer->datas[0];
	SPA_FLAG_CLEAR(buf->v4l2.flags, V4L2_BUF_FLAG_QUEUED);

	SPA_FLAG_UPDATE(buf->v4l2.flags, V4L2_BUF_FLAG_ERROR,
		SPA_FLAG_IS_SET(d->chunk->flags, SPA_CHUNK_FLAG_CORRUPTED));

	buf->v4l2.bytesused = d->chunk->size;
	*arg = buf->v4l2;

exit_unlock:
	pw_thread_loop_unlock(file->loop);

	pw_log_debug("file:%p %d -> %d (%s)", file, arg->index, res, spa_strerror(res));
	return res;
}

static int vidioc_streamon(struct file *file, int *arg)
{
	int res;

	pw_log_info("file:%p -> %d", file, *arg);

	if (*arg != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	pw_thread_loop_lock(file->loop);
	if (file->n_buffers == 0) {
		res = -EINVAL;
		goto exit_unlock;
	}
	if (file->running) {
		res = 0;
		goto exit_unlock;
	}
	res = pw_stream_set_active(file->stream, true);
	if (res >= 0)
		file->running = true;
exit_unlock:
	pw_thread_loop_unlock(file->loop);

	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}
static int vidioc_streamoff(struct file *file, int *arg)
{
	int res;

	if (*arg != V4L2_BUF_TYPE_VIDEO_CAPTURE)
		return -EINVAL;

	pw_thread_loop_lock(file->loop);
	if (!file->running) {
		res = 0;
		goto exit_unlock;
	}
	res = pw_stream_set_active(file->stream, false);
	file->running = false;

exit_unlock:
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

#ifdef __FreeBSD__
	if (arg == NULL && (request & IOC_DIRMASK != IOC_VOID)) {
#else
	if (arg == NULL && (_IOC_DIR(request) & (_IOC_WRITE | _IOC_READ))) {
#endif
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
	pw_log_debug("fd:%d request:%lx nr:%d arg:%p -> %d (%s)",
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
	off64_t id;
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

	if (!SPA_FLAG_IS_SET(data->flags, SPA_DATA_FLAG_READABLE))
		prot &= ~PROT_READ;
	if (!SPA_FLAG_IS_SET(data->flags, SPA_DATA_FLAG_WRITABLE))
		prot &= ~PROT_WRITE;

	res = globals.old_fops.mmap(addr, range.size, prot, flags, data->fd, range.offset);

	add_file_map(file, addr);
	add_buffer_map(file, addr, id);
	SPA_FLAG_SET(buf->v4l2.flags, V4L2_BUF_FLAG_MAPPED);

	pw_log_info("addr:%p length:%u prot:%d flags:%d fd:%"PRIi64" offset:%u -> %p (%s)" ,
			addr, range.size, prot, flags, data->fd, range.offset,
			res, strerror(res == MAP_FAILED ? errno : 0));

error_unlock:
	pw_thread_loop_unlock(file->loop);
	unref_file(file);
	return res;
}

static int v4l2_munmap(void *addr, size_t length)
{
	int res;
	struct buffer_map *bmap;
	struct file *file;

	if ((file = remove_file_map(addr)) == NULL)
		return globals.old_fops.munmap(addr, length);

	pw_thread_loop_lock(file->loop);

	bmap = find_buffer_map(file, addr);
	if (bmap == NULL) {
		res = -EINVAL;
		goto exit_unlock;
	}
	res = globals.old_fops.munmap(addr, length);

	pw_log_info("addr:%p length:%zu -> %d (%s)", addr, length,
			res, strerror(res < 0 ? errno : 0));

	file->buffers[bmap->id].v4l2.flags &= ~V4L2_BUF_FLAG_MAPPED;
	remove_buffer_map(file, bmap);

exit_unlock:
	pw_thread_loop_unlock(file->loop);

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

static void initialize(void)
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
	pw_array_init(&globals.file_maps, 1024);
	pw_array_init(&globals.fd_maps, 256);
}

const struct fops *get_fops(void)
{
	static pthread_once_t initialized = PTHREAD_ONCE_INIT;
        pthread_once(&initialized, initialize);
	return &fops;
}
