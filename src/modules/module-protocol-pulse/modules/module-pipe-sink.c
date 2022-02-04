/* PipeWire
 *
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
 * Copyright © 2021 Sanchayan Maity <sanchayan@asymptotic.io>
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

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <pipewire/private.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/hook.h>

#include "../defs.h"
#include "../module.h"
#include "registry.h"

#define NAME "pipe-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define DEFAULT_FILE_NAME "/tmp/music.output"

struct module_pipesink_data {
	struct module *module;
	struct pw_core *core;

	struct pw_stream *capture;

	struct spa_hook core_listener;
	struct spa_hook capture_listener;

	struct pw_properties *capture_props;

	struct spa_audio_info_raw info;

	char *filename;
	int fd;
	bool do_unlink_fifo;
};

static void capture_process(void *data)
{
	struct module_pipesink_data *impl = data;
	struct pw_buffer *in;
	struct spa_data *d;
	uint32_t i, size, offset;
	int written;

	if ((in = pw_stream_dequeue_buffer(impl->capture)) == NULL) {
		pw_log_warn("Out of capture buffers: %m");
		return;
	}

	for (i = 0; i < in->buffer->n_datas; i++) {
		d = &in->buffer->datas[i];
		size = d->chunk->size;
		offset = d->chunk->offset;

		while (size > 0) {
			written = write(impl->fd, SPA_MEMBER(d->data, offset, void), size);
			if (written < 0) {
				if (errno == EINTR) {
					/* retry if interrupted */
					continue;
				} else if (errno == EAGAIN || errno == EWOULDBLOCK) {
					/* Don't continue writing */
					break;
				} else {
					pw_log_warn("Failed to write to pipe sink");
				}
			}

			offset += written;
			size -= written;
		}
	}
	pw_stream_queue_buffer(impl->capture, in);
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct module_pipesink_data *d = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		module_schedule_unload(d->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

static void on_stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct module_pipesink_data *d = data;

	switch (state) {
		case PW_STREAM_STATE_UNCONNECTED:
			pw_log_info("stream disconnected, unloading");
			module_schedule_unload(d->module);
			break;
		case PW_STREAM_STATE_ERROR:
			pw_log_error("stream error: %s", error);
			break;
		default:
			break;
	}
}

static const struct pw_stream_events in_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.process = capture_process
};

static int module_pipesink_load(struct client *client, struct module *module)
{
	struct module_pipesink_data *data = module->user_data;
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	data->core = pw_context_connect(module->impl->context,
			pw_properties_copy(client->props),
			0);
	if (data->core == NULL)
		return -errno;

	pw_core_add_listener(data->core,
			&data->core_listener,
			&core_events, data);

	pw_properties_setf(data->capture_props, "pulse.module.id", "%u", module->index);

	data->capture = pw_stream_new(data->core,
			"pipesink capture", data->capture_props);
	data->capture_props = NULL;
	if (data->capture == NULL)
		return -errno;

	pw_stream_add_listener(data->capture,
			&data->capture_listener,
			&in_stream_events, data);

	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&data->info);

	if ((res = pw_stream_connect(data->capture,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}

static int module_pipesink_unload(struct module *module)
{
	struct module_pipesink_data *d = module->user_data;

	pw_properties_free(d->capture_props);
	if (d->capture != NULL)
		pw_stream_destroy(d->capture);
	if (d->core != NULL)
		pw_core_disconnect(d->core);
	if (d->filename) {
		if (d->do_unlink_fifo)
			unlink(d->filename);
		free(d->filename);
	}
	if (d->fd >= 0)
		close(d->fd);

	return 0;
}

static const struct module_methods module_pipesink_methods = {
	VERSION_MODULE_METHODS,
	.load = module_pipesink_load,
	.unload = module_pipesink_unload,
};

static const struct spa_dict_item module_pipesink_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Pipe sink" },
	{ PW_KEY_MODULE_USAGE, "file=<name of the FIFO special file to use> "
				"sink_name=<name for the sink> "
				"format=<sample format> "
				"rate=<sample rate> "
				"channels=<number of channels> "
				"channel_map=<channel map> " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct module *create_module_pipe_sink(struct impl *impl, const char *argument)
{
	struct module *module;
	struct module_pipesink_data *d;
	struct pw_properties *props = NULL, *capture_props = NULL;
	struct spa_audio_info_raw info = { 0 };
	struct stat st;
	const char *str;
	char *filename = NULL;
	bool do_unlink_fifo;
	int res = 0;
	int fd = -1;

	PW_LOG_TOPIC_INIT(mod_topic);

	props = pw_properties_new_dict(&SPA_DICT_INIT_ARRAY(module_pipesink_info));
	capture_props = pw_properties_new(NULL, NULL);
	if (!props || !capture_props) {
		res = -EINVAL;
		goto out;
	}
	if (argument)
		module_args_add_props(props, argument);

	if (module_args_to_audioinfo(impl, props, &info) < 0) {
		res = -EINVAL;
		goto out;
	}

	if ((str = pw_properties_get(props, "format")) != NULL) {
		info.format = format_paname2id(str, strlen(str));
		pw_properties_set(props, "format", NULL);
	}

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_properties_set(capture_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "sink_name", NULL);
	}

	if ((str = pw_properties_get(props, "file")) != NULL) {
		filename = strdup(str);
		pw_properties_set(props, "file", NULL);
	} else {
		filename = strdup(DEFAULT_FILE_NAME);
	}

	do_unlink_fifo = false;
	if (mkfifo(filename, 0666) < 0) {
		if (errno != EEXIST) {
			res = -errno;
			pw_log_error("mkfifo('%s'): %s", filename, spa_strerror(res));
			goto out;
		}
	} else {
		do_unlink_fifo = true;
		/*
		 * Our umask is 077, so the pipe won't be created with the
		 * requested permissions. Let's fix the permissions with chmod().
		 */
		if (chmod(filename, 0666) < 0)
			pw_log_warn("chmod('%s'): %s", filename, spa_strerror(-errno));
	}

	if ((fd = open(filename, O_RDWR | O_CLOEXEC | O_NONBLOCK, 0)) <= 0) {
		res = -errno;
		pw_log_error("open('%s'): %s", filename, spa_strerror(res));
		goto out;
	}

	if (fstat(fd, &st) < 0) {
		res = -errno;
		pw_log_error("fstat('%s'): %s", filename, spa_strerror(res));
		goto out;
	}

	if (!S_ISFIFO(st.st_mode)) {
		pw_log_error("'%s' is not a FIFO.", filename);
		goto out;
	}

	if (pw_properties_get(capture_props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_set(capture_props, PW_KEY_NODE_GROUP, "pipewire.dummy");
	if (pw_properties_get(capture_props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(capture_props, PW_KEY_NODE_VIRTUAL, "true");
	pw_properties_set(capture_props, PW_KEY_MEDIA_CLASS, "Audio/Sink");

	module = module_new(impl, &module_pipesink_methods, sizeof(*d));
	if (module == NULL) {
		res = -errno;
		goto out;
	}

	module->props = props;
	d = module->user_data;
	d->module = module;
	d->capture_props = capture_props;
	d->info = info;
	d->fd = fd;
	d->filename = filename;
	d->do_unlink_fifo = do_unlink_fifo;

	pw_log_info("Successfully loaded module-pipe-sink");

	return module;
out:
	pw_properties_free(props);
	pw_properties_free(capture_props);
	if (filename) {
		if (do_unlink_fifo)
			unlink(filename);
		free(filename);
	}
	if (fd >= 0)
		close(fd);
	errno = -res;

	return NULL;
}
