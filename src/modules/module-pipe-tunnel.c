/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Sanchayan Maity <sanchayan@asymptotic.io> */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <limits.h>
#include <math.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/dll.h>
#include <spa/debug/types.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/impl.h>
#include <pipewire/i18n.h>

/** \page page_module_pipe_tunnel PipeWire Module: Unix Pipe Tunnel
 *
 * The pipe-tunnel module provides a source or sink that tunnels all audio to
 * or from a unix pipe respectively.
 *
 * ## Module Options
 *
 * - `tunnel.mode`: the desired tunnel to create. (Default `playback`)
 * - `pipe.filename`: the filename of the pipe.
 * - `stream.props`: Extra properties for the local stream.
 *
 * When `tunnel.mode` is `capture`, a capture stream on the default source is
 * created. The samples captured from the source will be written to the pipe.
 *
 * When `tunnel.mode` is `sink`, a sink node is created. Samples played on the
 * sink will be written to the pipe.
 *
 * When `tunnel.mode` is `playback`, a playback stream on the default sink is
 * created. The samples read from the pipe will be played on the sink.
 *
 * When `tunnel.mode` is `source`, a source node is created. Samples read from
 * the the pipe will be made available on the source.
 *
 * When `pipe.filename` is not given, a default fifo in `/tmp/fifo_input` or
 * `/tmp/fifo_output` will be created that can be written and read respectively,
 * depending on the selected `tunnel.mode`.
 *
 * ## General options
 *
 * Options with well-known behavior.
 *
 * - \ref PW_KEY_REMOTE_NAME
 * - \ref PW_KEY_AUDIO_FORMAT
 * - \ref PW_KEY_AUDIO_RATE
 * - \ref PW_KEY_AUDIO_CHANNELS
 * - \ref SPA_KEY_AUDIO_POSITION
 * - \ref PW_KEY_NODE_LATENCY
 * - \ref PW_KEY_NODE_NAME
 * - \ref PW_KEY_NODE_DESCRIPTION
 * - \ref PW_KEY_NODE_GROUP
 * - \ref PW_KEY_NODE_VIRTUAL
 * - \ref PW_KEY_MEDIA_CLASS
 * - \ref PW_KEY_TARGET_OBJECT to specify the remote name or serial id to link to
 *
 * When not otherwise specified, the pipe will accept or produce a
 * 16 bits, stereo, 48KHz sample stream.
 *
 * ## Example configuration of a pipe playback stream
 *
 *\code{.unparsed}
 * context.modules = [
 * {   name = libpipewire-module-pipe-tunnel
 *     args = {
 *         tunnel.mode = playback
 *         # Set the pipe name to tunnel to
 *         pipe.filename = "/tmp/fifo_output"
 *         #audio.format=<sample format>
 *         #audio.rate=<sample rate>
 *         #audio.channels=<number of channels>
 *         #audio.position=<channel map>
 *         #target.object=<remote target node>
 *         stream.props = {
 *             # extra sink properties
 *         }
 *     }
 * }
 * ]
 *\endcode
 */

#define NAME "pipe-tunnel"

#define DEFAULT_CAPTURE_FILENAME	"/tmp/fifo_input"
#define DEFAULT_PLAYBACK_FILENAME	"/tmp/fifo_output"

#define DEFAULT_FORMAT "S16"
#define DEFAULT_RATE 48000
#define DEFAULT_CHANNELS 2
#define DEFAULT_POSITION "[ FL FR ]"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MODULE_USAGE	"( remote.name=<remote> ) "				\
			"( node.latency=<latency as fraction> ) "		\
			"( node.name=<name of the nodes> ) "			\
			"( node.description=<description of the nodes> ) "	\
			"( target.object=<remote node target name or serial> ) "\
			"( audio.format=<sample format> ) "			\
			"( audio.rate=<sample rate> ) "				\
			"( audio.channels=<number of channels> ) "		\
			"( audio.position=<channel map> ) "			\
			"( tunnel.mode=capture|playback|sink|source )"		\
			"( pipe.filename=<filename> )"				\
			"( stream.props=<properties> ) "


static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create a UNIX pipe tunnel" },
	{ PW_KEY_MODULE_USAGE, MODULE_USAGE },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct impl {
	struct pw_context *context;

#define MODE_PLAYBACK	0
#define MODE_CAPTURE	1
#define MODE_SINK	2
#define MODE_SOURCE	3
	uint32_t mode;
	struct pw_properties *props;

	struct pw_impl_module *module;

	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	char *filename;
	unsigned int unlink_fifo;
	int fd;

	struct pw_properties *stream_props;
	enum pw_direction direction;
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct spa_audio_info_raw info;
	uint32_t frame_size;

	unsigned int do_disconnect:1;
	uint32_t leftover_count;
	uint8_t *leftover;
};

static void stream_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->stream_listener);
	impl->stream = NULL;
}

static void stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct impl *impl = d;
	switch (state) {
	case PW_STREAM_STATE_ERROR:
	case PW_STREAM_STATE_UNCONNECTED:
		pw_impl_module_schedule_destroy(impl->module);
		break;
	case PW_STREAM_STATE_PAUSED:
		break;
	case PW_STREAM_STATE_STREAMING:
		break;
	default:
		break;
	}
}

static void playback_stream_process(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	uint32_t i, size, offs;
	ssize_t written;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	for (i = 0; i < buf->buffer->n_datas; i++) {
		struct spa_data *d;
		d = &buf->buffer->datas[i];

		offs = SPA_MIN(d->chunk->offset, d->maxsize);
		size = SPA_MIN(d->chunk->size, d->maxsize - offs);

		while (size > 0) {
			written = write(impl->fd, SPA_MEMBER(d->data, offs, void), size);
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
			offs += written;
			size -= written;
		}
	}
	pw_stream_queue_buffer(impl->stream, buf);
}

static void capture_stream_process(void *data)
{
	struct impl *impl = data;
	struct pw_buffer *buf;
	struct spa_data *d;
	uint32_t req;
	ssize_t nread;

	if ((buf = pw_stream_dequeue_buffer(impl->stream)) == NULL) {
		pw_log_debug("out of buffers: %m");
		return;
	}

	d = &buf->buffer->datas[0];

	if ((req = buf->requested * impl->frame_size) == 0)
		req = 4096 * impl->frame_size;

	req = SPA_MIN(req, d->maxsize);

	d->chunk->offset = 0;
	d->chunk->stride = impl->frame_size;
	d->chunk->size = SPA_MIN(req, impl->leftover_count);
	memcpy(d->data, impl->leftover, d->chunk->size);
	req -= d->chunk->size;

	nread = read(impl->fd, SPA_PTROFF(d->data, d->chunk->size, void), req);
	if (nread < 0) {
		const bool important = !(errno == EINTR
					 || errno == EAGAIN
					 || errno == EWOULDBLOCK);

		if (important)
			pw_log_warn("failed to read from pipe (%s): %s",
				    impl->filename, strerror(errno));
	}
	else {
		d->chunk->size += nread;
	}

	impl->leftover_count = d->chunk->size % impl->frame_size;
	d->chunk->size -= impl->leftover_count;
	memcpy(impl->leftover, SPA_PTROFF(d->data, d->chunk->size, void), impl->leftover_count);

	pw_stream_queue_buffer(impl->stream, buf);
}

static const struct pw_stream_events playback_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.process = playback_stream_process
};

static const struct pw_stream_events capture_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = stream_destroy,
	.state_changed = stream_state_changed,
	.process = capture_stream_process
};

static int create_stream(struct impl *impl)
{
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b;

	impl->stream = pw_stream_new(impl->core, "pipe", impl->stream_props);
	impl->stream_props = NULL;

	if (impl->stream == NULL)
		return -errno;

	if (impl->direction == PW_DIRECTION_OUTPUT) {
		pw_stream_add_listener(impl->stream,
				&impl->stream_listener,
				&capture_stream_events, impl);
	} else {
		pw_stream_add_listener(impl->stream,
				&impl->stream_listener,
				&playback_stream_events, impl);
	}

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &impl->info);

	if ((res = pw_stream_connect(impl->stream,
			impl->direction,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}

static int create_fifo(struct impl *impl)
{
	struct stat st;
	const char *filename;
	bool do_unlink_fifo = false;
	int fd = -1, res;

	if ((filename = pw_properties_get(impl->props, "pipe.filename")) == NULL)
		filename = impl->direction == PW_DIRECTION_INPUT ?
			DEFAULT_CAPTURE_FILENAME :
			DEFAULT_PLAYBACK_FILENAME;

	if (mkfifo(filename, 0666) < 0) {
		if (errno != EEXIST) {
			res = -errno;
			pw_log_error("mkfifo('%s'): %s", filename, spa_strerror(res));
			goto error;
		}
	} else {
		/*
		 * Our umask is 077, so the pipe won't be created with the
		 * requested permissions. Let's fix the permissions with chmod().
		 */
		if (chmod(filename, 0666) < 0)
			pw_log_warn("chmod('%s'): %s", filename, spa_strerror(-errno));

		do_unlink_fifo = true;
	}

	if ((fd = open(filename, O_RDWR | O_CLOEXEC | O_NONBLOCK, 0)) < 0) {
		res = -errno;
		pw_log_error("open('%s'): %s", filename, spa_strerror(res));
		goto error;
	}

	if (fstat(fd, &st) < 0) {
		res = -errno;
		pw_log_error("fstat('%s'): %s", filename, spa_strerror(res));
		goto error;
	}

	if (!S_ISFIFO(st.st_mode)) {
		res = -EINVAL;
		pw_log_error("'%s' is not a FIFO.", filename);
		goto error;
	}
	pw_log_info("%s fifo '%s' with format:%s channels:%d rate:%d",
			impl->direction == PW_DIRECTION_OUTPUT ? "reading from" : "writing to",
			filename,
			spa_debug_type_find_name(spa_type_audio_format, impl->info.format),
			impl->info.channels, impl->info.rate);

	impl->filename = strdup(filename);
	impl->unlink_fifo = do_unlink_fifo;
	impl->fd = fd;
	return 0;

error:
	if (do_unlink_fifo)
		unlink(filename);
	if (fd >= 0)
		close(fd);
	return res;
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = core_error,
};

static void core_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->core_listener);
	impl->core = NULL;
	pw_impl_module_schedule_destroy(impl->module);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void impl_destroy(struct impl *impl)
{
	if (impl->stream)
		pw_stream_destroy(impl->stream);
	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);

	if (impl->filename) {
		if (impl->unlink_fifo)
			unlink(impl->filename);
		free(impl->filename);
	}
	if (impl->fd >= 0)
		close(impl->fd);

	pw_properties_free(impl->stream_props);
	pw_properties_free(impl->props);

	free(impl->leftover);
	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->module_listener);
	impl_destroy(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static uint32_t channel_from_name(const char *name)
{
	int i;
	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (spa_streq(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)))
			return spa_type_audio_channel[i].type;
	}
	return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static void parse_position(struct spa_audio_info_raw *info, const char *val, size_t len)
{
	struct spa_json it[2];
	char v[256];

	spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

	info->channels = 0;
	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
	    info->channels < SPA_AUDIO_MAX_CHANNELS) {
		info->position[info->channels++] = channel_from_name(v);
	}
}

static inline uint32_t format_from_name(const char *name, size_t len)
{
	int i;
	for (i = 0; spa_type_audio_format[i].name; i++) {
		if (strncmp(name, spa_debug_type_short_name(spa_type_audio_format[i].name), len) == 0)
			return spa_type_audio_format[i].type;
	}
	return SPA_AUDIO_FORMAT_UNKNOWN;
}

static void parse_audio_info(const struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	spa_zero(*info);
	if ((str = pw_properties_get(props, PW_KEY_AUDIO_FORMAT)) == NULL)
		str = DEFAULT_FORMAT;
	info->format = format_from_name(str, strlen(str));

	info->rate = pw_properties_get_uint32(props, PW_KEY_AUDIO_RATE, info->rate);
	if (info->rate == 0)
		info->rate = DEFAULT_RATE;

	info->channels = pw_properties_get_uint32(props, PW_KEY_AUDIO_CHANNELS, info->channels);
	info->channels = SPA_MIN(info->channels, SPA_AUDIO_MAX_CHANNELS);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(info, str, strlen(str));
	if (info->channels == 0)
		parse_position(info, DEFAULT_POSITION, strlen(DEFAULT_POSITION));
}

static int calc_frame_size(const struct spa_audio_info_raw *info)
{
	int res = info->channels;
	switch (info->format) {
	case SPA_AUDIO_FORMAT_U8:
	case SPA_AUDIO_FORMAT_S8:
	case SPA_AUDIO_FORMAT_ALAW:
	case SPA_AUDIO_FORMAT_ULAW:
		return res;
	case SPA_AUDIO_FORMAT_S16:
	case SPA_AUDIO_FORMAT_S16_OE:
	case SPA_AUDIO_FORMAT_U16:
		return res * 2;
	case SPA_AUDIO_FORMAT_S24:
	case SPA_AUDIO_FORMAT_S24_OE:
	case SPA_AUDIO_FORMAT_U24:
		return res * 3;
	case SPA_AUDIO_FORMAT_S24_32:
	case SPA_AUDIO_FORMAT_S24_32_OE:
	case SPA_AUDIO_FORMAT_S32:
	case SPA_AUDIO_FORMAT_S32_OE:
	case SPA_AUDIO_FORMAT_U32:
	case SPA_AUDIO_FORMAT_U32_OE:
	case SPA_AUDIO_FORMAT_F32:
	case SPA_AUDIO_FORMAT_F32_OE:
		return res * 4;
	case SPA_AUDIO_FORMAT_F64:
	case SPA_AUDIO_FORMAT_F64_OE:
		return res * 8;
	default:
		return 0;
	}
}

static void copy_props(struct impl *impl, struct pw_properties *props, const char *key)
{
	const char *str;
	if ((str = pw_properties_get(props, key)) != NULL) {
		if (pw_properties_get(impl->stream_props, key) == NULL)
			pw_properties_set(impl->stream_props, key, str);
	}
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props = NULL;
	struct impl *impl;
	const char *str, *media_class = NULL;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->fd = -1;

	pw_log_debug("module %p: new %s", impl, args);

	if (args == NULL)
		args = "";

	props = pw_properties_new_string(args);
	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}
	impl->props = props;

	impl->stream_props = pw_properties_new(NULL, NULL);
	if (impl->stream_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->module = module;
	impl->context = context;

	if ((str = pw_properties_get(props, "tunnel.mode")) == NULL)
		str = "playback";

	if (spa_streq(str, "capture")) {
		impl->mode = MODE_CAPTURE;
		impl->direction = PW_DIRECTION_INPUT;
	} else if (spa_streq(str, "playback")) {
		impl->mode = MODE_PLAYBACK;
		impl->direction = PW_DIRECTION_OUTPUT;
	}else if (spa_streq(str, "sink")) {
		impl->mode = MODE_SINK;
		impl->direction = PW_DIRECTION_INPUT;
		media_class = "Audio/Sink";
	} else if (spa_streq(str, "source")) {
		impl->mode = MODE_SOURCE;
		impl->direction = PW_DIRECTION_OUTPUT;
		media_class = "Audio/Source";
	} else {
		pw_log_error("invalid tunnel.mode '%s'", str);
		res = -EINVAL;
		goto error;
	}

	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_CLASS, media_class);

	if ((str = pw_properties_get(props, "stream.props")) != NULL)
		pw_properties_update_string(impl->stream_props, str, strlen(str));

	copy_props(impl, props, PW_KEY_AUDIO_FORMAT);
	copy_props(impl, props, PW_KEY_AUDIO_RATE);
	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_NAME);
	copy_props(impl, props, PW_KEY_NODE_DESCRIPTION);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LATENCY);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);
	copy_props(impl, props, PW_KEY_MEDIA_CLASS);
	copy_props(impl, props, PW_KEY_TARGET_OBJECT);
	copy_props(impl, props, "pipe.filename");

	parse_audio_info(impl->stream_props, &impl->info);

	impl->frame_size = calc_frame_size(&impl->info);
	if (impl->frame_size == 0) {
		pw_log_error("unsupported audio format:%d channels:%d",
				impl->info.format, impl->info.channels);
		res = -EINVAL;
		goto error;
	}
	if (impl->info.rate != 0 &&
	    pw_properties_get(props, PW_KEY_NODE_RATE) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_RATE,
				"1/%u", impl->info.rate),

	copy_props(impl, props, PW_KEY_NODE_RATE);

	impl->leftover = calloc(1, impl->frame_size);
	if (impl->leftover == NULL) {
		res = -errno;
		pw_log_error("can't alloc leftover buffer: %m");
		goto error;
	}

	impl->core = pw_context_get_object(impl->context, PW_TYPE_INTERFACE_Core);
	if (impl->core == NULL) {
		str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
		impl->core = pw_context_connect(impl->context,
				pw_properties_new(
					PW_KEY_REMOTE_NAME, str,
					NULL),
				0);
		impl->do_disconnect = true;
	}
	if (impl->core == NULL) {
		res = -errno;
		pw_log_error("can't connect: %m");
		goto error;
	}

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	if ((res = create_fifo(impl)) < 0)
		goto error;

	if ((res = create_stream(impl)) < 0)
		goto error;

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;

error:
	impl_destroy(impl);
	return res;
}
