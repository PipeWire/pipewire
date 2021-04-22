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

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include "config.h"

#include <spa/utils/result.h>
#include <spa/utils/json.h>
#include <spa/param/profiler.h>
#include <spa/debug/pod.h>

#include <pipewire/private.h>
#include <pipewire/impl.h>
#include <extensions/profiler.h>

#define NAME "loopback"

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create loopback streams" },
	{ PW_KEY_MODULE_USAGE, " [ remote.name=<remote> ] "
				"[ node.latency=<latency as fraction> ] "
                                "[ audio.rate=<sample rate> ] "
                                "[ audio.channels=<number of channels> ] "
                                "[ audio.position=<channel map> ] "
                                "[ capture.props=<properties> ] "
                                "[ playback.props=<properties> ] " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <getopt.h>
#include <limits.h>
#include <math.h>

#include <spa/utils/result.h>
#include <spa/pod/builder.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

#include <pipewire/pipewire.h>

struct impl {
	struct pw_context *context;

	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct spa_audio_info_raw info;

	struct pw_properties *capture_props;
	struct pw_stream *capture;
	struct spa_hook capture_listener;

	struct pw_properties *playback_props;
	struct pw_stream *playback;
	struct spa_hook playback_listener;
};

static void capture_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->capture_listener);
	impl->capture = NULL;
}

static void capture_process(void *d)
{
	struct impl *impl = d;
	struct pw_buffer *in, *out;
	uint32_t i;

	if ((in = pw_stream_dequeue_buffer(impl->capture)) == NULL)
		pw_log_warn("out of capture buffers: %m");

	if ((out = pw_stream_dequeue_buffer(impl->playback)) == NULL)
		pw_log_warn("out of playback buffers: %m");

	if (in != NULL && out != NULL) {
		for (i = 0; i < in->buffer->n_datas; i++) {
			struct spa_data *ds, *dd;

			ds = &in->buffer->datas[i];
			dd = &out->buffer->datas[i];

			memcpy(dd->data,
				SPA_MEMBER(ds->data, ds->chunk->offset, void),
				ds->chunk->size);

			dd->chunk->offset = 0;
			dd->chunk->size = ds->chunk->size;
			dd->chunk->stride = ds->chunk->stride;
		}
	}

	if (in != NULL)
		pw_stream_queue_buffer(impl->capture, in);
	if (out != NULL)
		pw_stream_queue_buffer(impl->playback, out);
}

static const struct pw_stream_events in_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = capture_destroy,
	.process = capture_process
};

static void playback_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->playback_listener);
	impl->playback = NULL;
}

static const struct pw_stream_events out_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = playback_destroy
};

static int setup_streams(struct impl *impl)
{
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

	impl->capture = pw_stream_new(impl->core,
			"loopback capture", impl->capture_props);
	impl->capture_props = NULL;
	if (impl->capture == NULL)
		return -errno;

	pw_stream_add_listener(impl->capture,
			&impl->capture_listener,
			&in_stream_events, impl);

	impl->playback = pw_stream_new(impl->core,
			"loopback playback", impl->playback_props);
	impl->playback_props = NULL;
	if (impl->playback == NULL)
		return -errno;

	pw_stream_add_listener(impl->playback,
			&impl->playback_listener,
			&out_stream_events, impl);

	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&impl->info);

	if ((res = pw_stream_connect(impl->capture,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	if ((res = pw_stream_connect(impl->playback,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	return 0;
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);
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
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void module_destroy(void *data)
{
	struct impl *impl = data;

	spa_hook_remove(&impl->module_listener);

	if (impl->capture)
		pw_stream_destroy(impl->capture);
	if (impl->playback)
		pw_stream_destroy(impl->playback);
	if (impl->core)
		pw_core_disconnect(impl->core);
	if (impl->capture_props)
		pw_properties_free(impl->capture_props);
	if (impl->playback_props)
		pw_properties_free(impl->playback_props);
	free(impl);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy,
};

static uint32_t channel_from_name(const char *name)
{
	int i;
	for (i = 0; spa_type_audio_channel[i].name; i++) {
		if (strcmp(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)) == 0)
			return spa_type_audio_channel[i].type;
	}
	return SPA_AUDIO_CHANNEL_UNKNOWN;
}

static inline void parse_position(struct impl *impl, const char *val, size_t len)
{
	struct spa_json it[2];
	char v[256];

	spa_json_init(&it[0], val, len);
        if (spa_json_enter_array(&it[0], &it[1]) <= 0)
                spa_json_init(&it[1], val, len);

	impl->info.channels = 0;
	while (spa_json_get_string(&it[1], v, sizeof(v)) > 0 &&
	    impl->info.channels < SPA_AUDIO_MAX_CHANNELS) {
		impl->info.position[impl->info.channels++] = channel_from_name(v);
	}
}

SPA_EXPORT
int pipewire__module_init(struct pw_impl_module *module, const char *args)
{
	struct pw_context *context = pw_impl_module_get_context(module);
	struct pw_properties *props;
	struct impl *impl;
	uint32_t id = pw_global_get_id(pw_impl_module_get_global(module));
	const char *str;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);

	impl->info = SPA_AUDIO_INFO_RAW_INIT(
				.format = SPA_AUDIO_FORMAT_F32P);
	if (args)
		props = pw_properties_new_string(args);
	else
		props = pw_properties_new(NULL, NULL);

	impl->capture_props = pw_properties_new(NULL, NULL);
	impl->playback_props = pw_properties_new(NULL, NULL);
	if (impl->capture_props == NULL || impl->playback_props == NULL) {
		pw_log_error( "can't create properties: %m");
		return -errno;
	}

	impl->context = context;

	if ((str = pw_properties_get(props, PW_KEY_AUDIO_RATE)) != NULL)
		impl->info.rate = atoi(str);
	if ((str = pw_properties_get(props, PW_KEY_AUDIO_CHANNELS)) != NULL)
		impl->info.channels = atoi(str);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(impl, str, strlen(str));

	if ((str = pw_properties_get(props, "capture.props")) != NULL)
		pw_properties_update_string(impl->capture_props, str, strlen(str));
	if ((str = pw_properties_get(props, "playback.props")) != NULL)
		pw_properties_update_string(impl->playback_props, str, strlen(str));

	if (pw_properties_get(impl->capture_props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(impl->capture_props, PW_KEY_NODE_GROUP, "loopback-%u", id);
	if (pw_properties_get(impl->playback_props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(impl->playback_props, PW_KEY_NODE_GROUP, "loopback-%u", id);

	str = pw_properties_get(props, PW_KEY_REMOTE_NAME);
	impl->core = pw_context_connect(impl->context,
			pw_properties_new(
				PW_KEY_REMOTE_NAME, str,
				NULL),
			0);
	if (impl->core == NULL) {
		pw_log_error("can't connect: %m");
		return -errno;
	}

	pw_proxy_add_listener((struct pw_proxy*)impl->core,
			&impl->core_proxy_listener,
			&core_proxy_events, impl);
	pw_core_add_listener(impl->core,
			&impl->core_listener,
			&core_events, impl);

	setup_streams(impl);

	pw_impl_module_add_listener(module, &impl->module_listener, &module_events, impl);

	pw_impl_module_update_properties(module, &SPA_DICT_INIT_ARRAY(module_props));

	return 0;
}
