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

#define NAME "pipe-source"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic


#define DEFAULT_FILE_NAME "/tmp/music.input"

struct module_pipesrc_data {
	struct module *module;
	struct pw_core *core;

	struct pw_stream *playback;

	struct spa_hook core_listener;
	struct spa_hook playback_listener;

	struct pw_properties *playback_props;

	struct spa_audio_info_raw info;

	bool do_unlink;
	char *filename;
	int fd;

	uint32_t stride;

	uint32_t leftover_count;
	uint8_t leftover[]; /* `stride` bytes for storing a partial sample */
};

static void playback_process(void *data)
{
	struct module_pipesrc_data *impl = data;
	struct spa_chunk *chunk;
	struct pw_buffer *buffer;
	struct spa_data *d;
	uint32_t left, leftover;
	ssize_t bytes_read;

	if ((buffer = pw_stream_dequeue_buffer(impl->playback)) == NULL) {
		pw_log_warn("Out of playback buffers: %m");
		return;
	}

	d = &buffer->buffer->datas[0];
	if (d->data == NULL)
		return;

	left = d->maxsize;
	spa_assert(left >= impl->leftover_count);

	chunk = d->chunk;

	chunk->offset = 0;
	chunk->stride = impl->stride;
	chunk->size = impl->leftover_count;

	memcpy(d->data, impl->leftover, impl->leftover_count);

	left -= impl->leftover_count;

	bytes_read = read(impl->fd, SPA_PTROFF(d->data, chunk->size, void), left);
	if (bytes_read < 0) {
		const bool important = !(errno == EINTR
					 || errno == EAGAIN
					 || errno == EWOULDBLOCK);

		if (important)
			pw_log_warn("failed to read from pipe (%s): %s",
				    impl->filename, strerror(errno));
	}
	else {
		chunk->size += bytes_read;
	}

	leftover = chunk->size % impl->stride;
	chunk->size -= leftover;

	memcpy(impl->leftover, SPA_PTROFF(d->data, chunk->size, void), leftover);
	impl->leftover_count = leftover;

	pw_stream_queue_buffer(impl->playback, buffer);
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct module_pipesrc_data *d = data;

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
	struct module_pipesrc_data *d = data;

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

static const struct pw_stream_events out_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_stream_state_changed,
	.process = playback_process
};

static int module_pipesource_load(struct client *client, struct module *module)
{
	struct module_pipesrc_data *data = module->user_data;
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

	pw_properties_setf(data->playback_props, "pulse.module.id",
			"%u", module->index);

	data->playback = pw_stream_new(data->core,
			"pipesource playback", data->playback_props);
	data->playback_props = NULL;
	if (data->playback == NULL)
		return -errno;

	pw_stream_add_listener(data->playback,
			&data->playback_listener,
			&out_stream_events, data);

	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&data->info);

	if ((res = pw_stream_connect(data->playback,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}

static int module_pipesource_unload(struct module *module)
{
	struct module_pipesrc_data *d = module->user_data;

	pw_properties_free(d->playback_props);
	if (d->playback != NULL)
		pw_stream_destroy(d->playback);
	if (d->core != NULL)
		pw_core_disconnect(d->core);
	if (d->do_unlink)
		unlink(d->filename);
	free(d->filename);
	if (d->fd >= 0)
		close(d->fd);

	return 0;
}

static const struct module_methods module_pipesource_methods = {
	VERSION_MODULE_METHODS,
	.load = module_pipesource_load,
	.unload = module_pipesource_unload,
};

static const struct spa_dict_item module_pipesource_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Pipe source" },
	{ PW_KEY_MODULE_USAGE, "file=<name of the FIFO special file to use> "
				"source_name=<name for the source> "
				"format=<sample format> "
				"rate=<sample rate> "
				"channels=<number of channels> "
				"channel_map=<channel map> " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct module *create_module_pipe_source(struct impl *impl, const char *argument)
{
	struct module *module;
	struct module_pipesrc_data *d;
	struct pw_properties *props = NULL, *playback_props = NULL;
	struct spa_audio_info_raw info = { 0 };
	struct stat st;
	const char *str;
	bool do_unlink = false;
	char *filename = NULL;
	int stride, res = 0;
	int fd = -1;

	PW_LOG_TOPIC_INIT(mod_topic);

	props = pw_properties_new_dict(&SPA_DICT_INIT_ARRAY(module_pipesource_info));
	playback_props = pw_properties_new(NULL, NULL);
	if (!props || !playback_props) {
		res = -errno;
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

	switch (info.format) {
	case SPA_AUDIO_FORMAT_U8:
		stride = info.channels;
		break;
	case SPA_AUDIO_FORMAT_S16_LE:
	case SPA_AUDIO_FORMAT_S16_BE:
	case SPA_AUDIO_FORMAT_S16P:
		stride = 2 * info.channels;
		break;
	case SPA_AUDIO_FORMAT_S24_LE:
	case SPA_AUDIO_FORMAT_S24_BE:
	case SPA_AUDIO_FORMAT_S24P:
		stride = 3 * info.channels;
		break;
	case SPA_AUDIO_FORMAT_F32_LE:
	case SPA_AUDIO_FORMAT_F32_BE:
	case SPA_AUDIO_FORMAT_F32P:
	case SPA_AUDIO_FORMAT_S32_LE:
	case SPA_AUDIO_FORMAT_S32_BE:
	case SPA_AUDIO_FORMAT_S32P:
	case SPA_AUDIO_FORMAT_S24_32_LE:
	case SPA_AUDIO_FORMAT_S24_32_BE:
	case SPA_AUDIO_FORMAT_S24_32P:
		stride = 4 * info.channels;
		break;
	default:
		pw_log_error("Invalid audio format");
		res = -EINVAL;
		goto out;
	}

	if ((str = pw_properties_get(props, "source_name")) != NULL) {
		pw_properties_set(playback_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "source_name", NULL);
	}

	if ((str = pw_properties_get(props, "file")) != NULL) {
		filename = strdup(str);
		pw_properties_set(props, "file", NULL);
	} else {
		filename = strdup(DEFAULT_FILE_NAME);
	}

	if (filename == NULL) {
		res = -ENOMEM;
		goto out;
	}

	if (mkfifo(filename, 0666) < 0) {
		if (errno != EEXIST) {
			res = -errno;
			pw_log_error("mkfifo('%s'): %s", filename, spa_strerror(res));
			goto out;
		}

		do_unlink = false;
	} else {
		/*
		 * Our umask is 077, so the pipe won't be created with the
		 * requested permissions. Let's fix the permissions with chmod().
		 */
		if (chmod(filename, 0666) < 0)
			pw_log_warn("chmod('%s'): %s", filename, spa_strerror(-errno));

		do_unlink = true;
	}

	if ((fd = open(filename, O_RDONLY | O_CLOEXEC | O_NONBLOCK, 0)) <= 0) {
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
		res = -EEXIST;
		pw_log_error("'%s' is not a FIFO", filename);
		goto out;
	}

	if (pw_properties_get(playback_props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_set(playback_props, PW_KEY_NODE_GROUP, "pipewire.dummy");
	if (pw_properties_get(playback_props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(playback_props, PW_KEY_NODE_VIRTUAL, "true");
	pw_properties_set(playback_props, PW_KEY_MEDIA_CLASS, "Audio/Source");

	module = module_new(impl, &module_pipesource_methods, sizeof(*d) + stride);
	if (module == NULL) {
		res = -errno;
		goto out;
	}

	module->props = props;
	d = module->user_data;
	d->module = module;
	d->playback_props = playback_props;
	d->info = info;
	d->fd = fd;
	d->filename = filename;
	d->do_unlink = do_unlink;
	d->stride = stride;

	pw_log_info("Successfully loaded module-pipe-source");

	return module;
out:
	pw_properties_free(props);
	pw_properties_free(playback_props);
	if (do_unlink)
		unlink(filename);
	free(filename);
	if (fd >= 0)
		close(fd);
	errno = -res;

	return NULL;
}
