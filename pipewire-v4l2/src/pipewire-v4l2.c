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
#include <linux/videodev2.h>

#include "pipewire-v4l2.h"

#include <spa/utils/result.h>

#include <pipewire/pipewire.h>

PW_LOG_TOPIC_STATIC(v4l2_log_topic, "v4l2");
#define PW_LOG_TOPIC_DEFAULT v4l2_log_topic

struct globals {
	pthread_mutex_t lock;

	struct fops old_fops;

	struct spa_list files;
	struct spa_list maps;
};

static struct globals globals;

struct map {
	struct spa_list link;
	int ref;
	void *addr;
	struct file *file;
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
	int pending_sync;
        int last_sync;
        int last_res;
        bool error;

	struct pw_registry *registry;
	struct spa_hook registry_listener;

	int fd;
	int other_fd;
};

#define ATOMIC_DEC(s)                   __atomic_sub_fetch(&(s), 1, __ATOMIC_SEQ_CST)
#define ATOMIC_INC(s)                   __atomic_add_fetch(&(s), 1, __ATOMIC_SEQ_CST)

static struct file *make_file(void)
{
	struct file *file;

	file = calloc(1, sizeof(*file));
	if (file == NULL)
		return NULL;

	file->ref = 1;
	spa_list_init(&file->link);
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

static void on_sync_reply(void *data, uint32_t id, int seq)
{
	struct file *file = data;
	if (id != PW_ID_CORE)
		return;
	file->last_sync = seq;
	if (file->pending_sync == seq)
		pw_thread_loop_signal(file->loop, false);
}

static void on_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct file *file = data;

	pw_log_warn("%p: error id:%u seq:%d res:%d (%s): %s", file,
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE) {
		file->error = true;
		file->last_res = res;
	}
	pw_thread_loop_signal(file->loop, false);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = on_sync_reply,
	.error = on_error,
};

static void registry_event_global(void *data, uint32_t id,
		uint32_t permissions, const char *type, uint32_t version,
		const struct spa_dict *props)
{
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
	int fds[2];
	struct file *file;

	if (!spa_strstartswith(path, "/dev/video0"))
		return globals.old_fops.openat(dirfd, path, oflag, mode);

	if ((file = make_file()) == NULL)
		return -1;

	file->props = pw_properties_new(
			PW_KEY_CLIENT_API, "v4l2",
			NULL);
	file->loop = pw_thread_loop_new("v4l2", NULL);
	file->l = pw_thread_loop_get_loop(file->loop);
	file->context = pw_context_new(file->l,
			pw_properties_copy(file->props), 0);
	if (file->context == NULL)
		return -1;

	pw_thread_loop_start(file->loop);

	pw_thread_loop_lock(file->loop);

	file->core = pw_context_connect(file->context,
			pw_properties_copy(file->props), 0);
	if (file->core == NULL)
		return -1;

	pw_core_add_listener(file->core,
			&file->core_listener,
			&core_events, file);
	file->registry = pw_core_get_registry(file->core,
			PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(file->registry,
			&file->registry_listener,
			&registry_events, file);

	pw_thread_loop_unlock(file->loop);

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) < 0)
		return -1;

	res = file->fd = fds[0];
	file->other_fd = fds[1];

	pw_log_info("path:%s oflag:%d mode:%d -> %d (%s)", path, oflag, mode,
			res, strerror(res < 0 ? errno : 0));

	put_file(file);

	return res;
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
	int res;
	struct file *file;

	if ((file = find_file(fd)) == NULL)
		return globals.old_fops.close(fd);

	res = globals.old_fops.close(file->fd);
	res = globals.old_fops.close(file->other_fd);

	if (file->loop)
		pw_thread_loop_stop(file->loop);

	if (file->registry) {
		spa_hook_remove(&file->registry_listener);
		pw_proxy_destroy((struct pw_proxy*)file->registry);
	}
	if (file->core) {
		spa_hook_remove(&file->core_listener);
		pw_core_disconnect(file->core);
	}
	if (file->context)
		pw_context_destroy(file->context);
	if (file->loop)
		pw_thread_loop_destroy(file->loop);

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
static int vidioc_enum_fmt(struct file *file, struct v4l2_fmtdesc *arg)
{
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}
static int vidioc_g_fmt(struct file *file, struct v4l2_format *arg)
{
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}
static int vidioc_s_fmt(struct file *file, struct v4l2_format *arg)
{
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}
static int vidioc_try_fmt(struct file *file, struct v4l2_format *arg)
{
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
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
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}
static int vidioc_querybuf(struct file *file, struct v4l2_buffer *arg)
{
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}
static int vidioc_qbuf(struct file *file, struct v4l2_buffer *arg)
{
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}
static int vidioc_dqbuf(struct file *file, struct v4l2_buffer *arg)
{
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}
static int vidioc_streamon(struct file *file, int *arg)
{
	int res = -ENOTTY;
	pw_log_info("file:%p -> %d (%s)", file, res, spa_strerror(res));
	return res;
}
static int vidioc_streamoff(struct file *file, int *arg)
{
	int res = -ENOTTY;
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

	if ((file = find_file(fd)) == NULL)
		return globals.old_fops.mmap(addr, length, prot, flags, fd, offset);

	if ((map = make_map(addr, file)) == NULL) {
		res = MAP_FAILED;
		goto exit;
	}

	res = MAP_FAILED;
	errno = ENOTSUP;

	pw_log_info("addr:%p length:%zu prot:%d flags:%d fd:%d offset:%"PRIi64" -> %p (%s)" ,
			addr, length, prot, flags, fd, offset,
			res, strerror(res == MAP_FAILED ? errno : 0));

	put_map(map);
exit:
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
