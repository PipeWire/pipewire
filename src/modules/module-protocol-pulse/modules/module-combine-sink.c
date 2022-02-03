/* PipeWire
 *
 * Copyright © 2021 Wim Taymans <wim.taymans@gmail.com>
 * Copyright © 2021 Arun Raghavan <arun@asymptotic.io>
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

#include <spa/param/audio/format-utils.h>

#include <pipewire/pipewire.h>
#include <pipewire/utils.h>

#include "../manager.h"
#include "../module.h"
#include "registry.h"

#define NAME "combine-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define MAX_SINKS 64 /* ... good enough for anyone */

#define TIMEOUT_SINKS_MSEC	2000

static const struct spa_dict_item module_combine_sink_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Arun Raghavan <arun@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Combine multiple sinks into a single sink" },
	{ PW_KEY_MODULE_USAGE, "sink_name=<name of the sink> "
				"sink_properties=<properties for the sink> "
				/* not a great name, but for backwards compatibility... */
				"slaves=<sinks to combine> "
				"rate=<sample rate> "
				"channels=<number of channels> "
				"channel_map=<channel map> " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct module_combine_sink_data;

/* This goes to the stream event listener to be able to identify the stream on
 * which the event occurred and to have a link to the module data */
struct combine_stream {
	struct pw_stream *stream;
	struct spa_hook stream_listener;
	struct module_combine_sink_data *data;
	bool cleanup;
	bool started;
};

struct module_combine_sink_data {
	struct module *module;

	struct pw_core *core;
	struct pw_manager *manager;

	struct pw_stream *sink;

	struct spa_hook core_listener;
	struct spa_hook manager_listener;
	struct spa_hook sink_listener;

	char *sink_name;
	char **sink_names;
	struct combine_stream streams[MAX_SINKS];

	struct spa_source *cleanup;
	struct spa_source *sinks_timeout;

	struct spa_audio_info_raw info;

	unsigned int sinks_pending;
	unsigned int source_started:1;
	unsigned int load_emitted:1;
	unsigned int start_error:1;
};

/* Core connection: mainly to unload the module if the connection errors out */
static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct module_combine_sink_data *d = data;
	struct module *module = d->module;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		module_schedule_unload(module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = on_core_error,
};

/* Input stream: the "combine sink" */
static void capture_process(void *d)
{
	struct module_combine_sink_data *data = d;
	struct pw_buffer *in;
	int i;

	if ((in = pw_stream_dequeue_buffer(data->sink)) == NULL) {
		pw_log_warn("out of capture buffers: %m");
		return;
	}

	for (i = 0; i < MAX_SINKS; i++) {
		struct pw_buffer *out;
		uint32_t j, size = 0;
		int32_t stride = 0;

		if (data->streams[i].stream == NULL || data->streams[i].cleanup)
			continue;

		if ((out = pw_stream_dequeue_buffer(data->streams[i].stream)) == NULL) {
			pw_log_warn("out of playback buffers: %m");
			continue;
		}

		if (in->buffer->n_datas != out->buffer->n_datas) {
			pw_log_error("incompatible buffer planes");
			continue;
		}

		for (j = 0; j < out->buffer->n_datas; j++) {
			struct spa_data *ds, *dd;

			dd = &out->buffer->datas[j];

			if (j < in->buffer->n_datas) {
				ds = &in->buffer->datas[j];

				memcpy(dd->data,
					SPA_PTROFF(ds->data, ds->chunk->offset, void),
					ds->chunk->size);

				size = SPA_MAX(size, ds->chunk->size);
				stride = SPA_MAX(stride, ds->chunk->stride);
			} else {
				memset(dd->data, 0, size);
			}
			dd->chunk->offset = 0;
			dd->chunk->size = size;
			dd->chunk->stride = stride;
		}

		pw_stream_queue_buffer(data->streams[i].stream, out);
	}

	if (in != NULL)
		pw_stream_queue_buffer(data->sink, in);
}

static void check_initialized(struct module_combine_sink_data *data)
{
	struct module *module = data->module;

	if (data->load_emitted)
		return;

	if (data->start_error) {
		pw_log_debug("module load error");
		data->load_emitted = true;
		module_emit_loaded(module, -EIO);
	} else if (data->sinks_pending == 0 && data->source_started) {
		pw_log_debug("module loaded");
		data->load_emitted = true;
		module_emit_loaded(module, 0);
	}
}

static void on_in_stream_state_changed(void *d, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct module_combine_sink_data *data = d;
	struct module *module = data->module;
	uint32_t i;

	if (!data->source_started && state != PW_STREAM_STATE_CONNECTING) {
		/* Input stream appears on server */
		data->source_started = true;
		if (state < PW_STREAM_STATE_PAUSED)
			data->start_error = true;
		check_initialized(data);
	}

	switch (state) {
	case PW_STREAM_STATE_PAUSED:
		pw_stream_flush(data->sink, false);
		for (i = 0; i < MAX_SINKS; i++) {
			struct combine_stream *s = &data->streams[i];
			if (s->stream == NULL || s->cleanup)
				continue;
			pw_stream_flush(s->stream, false);
		}
		break;
	case PW_STREAM_STATE_UNCONNECTED:
		pw_log_info("stream disconnected, unloading");
		module_schedule_unload(module);
		break;
	default:
		break;
	}
}

static const struct pw_stream_events in_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_in_stream_state_changed,
	.process = capture_process
};

/* Output streams: one per sink we have combined output to */
static void on_out_stream_state_changed(void *data, enum pw_stream_state old,
		enum pw_stream_state state, const char *error)
{
	struct combine_stream *s = data;

	if (!s->started && state != PW_STREAM_STATE_CONNECTING) {
		/* Output stream appears on server */
		s->started = true;
		if (s->data->sinks_pending > 0)
			--s->data->sinks_pending;
		if (state < PW_STREAM_STATE_PAUSED)
			s->data->start_error = true;
		check_initialized(s->data);
	}

	if (state == PW_STREAM_STATE_UNCONNECTED) {
		s->cleanup = true;
		pw_loop_signal_event(s->data->module->impl->loop, s->data->cleanup);
	}
}

static const struct pw_stream_events out_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.state_changed = on_out_stream_state_changed,
};

static void manager_added(void *d, struct pw_manager_object *o)
{
	struct module_combine_sink_data *data = d;
	struct combine_stream *cstream;
	struct pw_properties *props;
	const struct spa_pod *params[1];
	const char *sink_name;
	struct spa_pod_builder b;
	uint32_t n_params;
	char buffer[1024];
	int i, res;

	if (!pw_manager_object_is_sink(o))
		return;

	sink_name = pw_properties_get(o->props, PW_KEY_NODE_NAME);

	if (strcmp(sink_name, data->sink_name) == 0) {
		/* That's the sink we created */
		return;
	}

	if (data->sink_names) {
		int i;

		for (i = 0; data->sink_names[i] != NULL; i++) {
			if (strcmp(data->sink_names[i], sink_name) == 0) {
				i = -1;
				break;
			}
		}

		/* This sink isn't in our list */
		if (i > -1)
			return;
	}

	pw_log_info("Adding %s to combine outputs", sink_name);

	for (i = 0; i < MAX_SINKS; i++)
		if (data->streams[i].stream == NULL)
			break;

	if (i == MAX_SINKS) {
		pw_log_error("Cannot combine more than %u sinks", MAX_SINKS);
		return;
	} else {
		cstream = &data->streams[i];
	}

	props = pw_properties_new(NULL, NULL);
	pw_properties_setf(props, PW_KEY_NODE_NAME,
			"combine_output.sink-%u.%s", data->module->index, sink_name);
	pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, data->sink_name);
	pw_properties_set(props, PW_KEY_NODE_TARGET, sink_name);
	pw_properties_setf(props, PW_KEY_NODE_GROUP, "combine_sink-%u", data->module->index);
	pw_properties_setf(props, PW_KEY_NODE_LINK_GROUP, "combine_sink-%u", data->module->index);
	pw_properties_set(props, PW_KEY_NODE_DONT_RECONNECT, "true");
	pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	pw_properties_set(props, PW_KEY_NODE_PASSIVE, "true");
	pw_properties_setf(props, "pulse.module.id", "%u", data->module->index);

	cstream->data = data;
	cstream->stream = pw_stream_new(data->core, NULL, props);
	if (cstream->stream == NULL) {
		pw_log_error("Could not create stream");
		goto error;
	}

	pw_stream_add_listener(cstream->stream,
			&cstream->stream_listener,
			&out_stream_events, cstream);

	n_params = 0;
	b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&data->info);

	if ((res = pw_stream_connect(cstream->stream,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0) {
		pw_log_error("Could not connect to sink '%s'", sink_name);
		goto error;
	}

	return;

error:
	data->start_error = true;
	check_initialized(data);
}

static const struct pw_manager_events manager_events = {
	PW_VERSION_MANAGER_EVENTS,
	.added = manager_added,
};

static void cleanup_stream(struct combine_stream *s)
{
	spa_hook_remove(&s->stream_listener);
	pw_stream_destroy(s->stream);

	s->stream = NULL;
	s->data = NULL;
	s->cleanup = false;
}

static void on_cleanup(void *d, uint64_t count)
{
	struct module_combine_sink_data *data = d;
	int i;

	for (i = 0; i < MAX_SINKS; i++) {
		if (data->streams[i].cleanup)
			cleanup_stream(&data->streams[i]);
	}
}

static void on_sinks_timeout(void *d, uint64_t count)
{
	struct module_combine_sink_data *data = d;

	if (data->load_emitted)
		return;

	data->start_error = true;
	check_initialized(data);
}

static int module_combine_sink_load(struct client *client, struct module *module)
{
	struct module_combine_sink_data *data = module->user_data;
	struct pw_properties *props;
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	const char *str;

	data->core = pw_context_connect(module->impl->context,
			pw_properties_copy(client->props),
			0);
	if (data->core == NULL)
		return -errno;

	pw_core_add_listener(data->core,
			&data->core_listener,
			&core_events, data);

	props = pw_properties_new(NULL, NULL);

	pw_properties_set(props, PW_KEY_NODE_NAME, data->sink_name);
	pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, data->sink_name);
	pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
	pw_properties_setf(props, PW_KEY_NODE_GROUP, "combine_sink-%u", data->module->index);
	pw_properties_setf(props, PW_KEY_NODE_LINK_GROUP, "combine_sink-%u", data->module->index);
	pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
	pw_properties_setf(props, "pulse.module.id", "%u", module->index);

	if ((str = pw_properties_get(module->props, "sink_properties")) != NULL)
		module_args_add_props(props, str);

	data->sink = pw_stream_new(data->core, data->sink_name, props);
	if (data->sink == NULL)
		return -errno;

	pw_stream_add_listener(data->sink,
			&data->sink_listener,
			&in_stream_events, data);

	n_params = 0;
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&data->info);

	if ((res = pw_stream_connect(data->sink,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	data->manager = pw_manager_new(data->core);
	if (data->manager == NULL)
		return -errno;

	pw_manager_add_listener(data->manager, &data->manager_listener,
			&manager_events, data);

	data->cleanup = pw_loop_add_event(module->impl->loop, on_cleanup, data);

	data->sinks_timeout = pw_loop_add_timer(module->impl->loop, on_sinks_timeout, data);
	if (data->sinks_timeout) {
		struct timespec timeout = {0}, interval = {0};
		timeout.tv_sec = TIMEOUT_SINKS_MSEC / 1000;
		timeout.tv_nsec = (TIMEOUT_SINKS_MSEC % 1000) * SPA_NSEC_PER_MSEC;
		pw_loop_update_timer(module->impl->loop, data->sinks_timeout, &timeout, &interval, false);
	}

	return data->load_emitted ? 0 : SPA_RESULT_RETURN_ASYNC(0);
}

static int module_combine_sink_unload(struct module *module)
{
	struct module_combine_sink_data *d = module->user_data;
	int i;

	if (d->cleanup != NULL)
		pw_loop_destroy_source(module->impl->loop, d->cleanup);

	if (d->sinks_timeout != NULL)
		pw_loop_destroy_source(module->impl->loop, d->sinks_timeout);

	/* Note that we explicitly disconnect the hooks to avoid having the
	 * cleanup triggered again in those callbacks */
	if (d->sink != NULL) {
		spa_hook_remove(&d->sink_listener);
		pw_stream_destroy(d->sink);
	}

	for (i = 0; i < MAX_SINKS; i++) {
		if (d->streams[i].stream)
			cleanup_stream(&d->streams[i]);
	}

	if (d->manager != NULL) {
		spa_hook_remove(&d->manager_listener);
		pw_manager_destroy(d->manager);
	}

	if (d->core != NULL) {
		spa_hook_remove(&d->core_listener);
		pw_core_disconnect(d->core);
	}

	pw_free_strv(d->sink_names);
	free(d->sink_name);

	return 0;
}

static const struct module_methods module_combine_sink_methods = {
	VERSION_MODULE_METHODS,
	.load = module_combine_sink_load,
	.unload = module_combine_sink_unload,
};

struct module *create_module_combine_sink(struct impl *impl, const char *argument)
{
	struct module *module;
	struct module_combine_sink_data *d;
	struct pw_properties *props = NULL;
	const char *str;
	char *sink_name = NULL, **sink_names = NULL;
	struct spa_audio_info_raw info = { 0 };
	int i, res;
	int num_sinks = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	props = pw_properties_new_dict(&SPA_DICT_INIT_ARRAY(module_combine_sink_info));
	if (!props) {
		res = -EINVAL;
		goto out;
	}
	if (argument)
		module_args_add_props(props, argument);

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		sink_name = strdup(str);
		pw_properties_set(props, "sink_name", NULL);
	} else {
		sink_name = strdup("combined");
	}

	if ((str = pw_properties_get(props, "slaves")) != NULL) {
		sink_names = pw_split_strv(str, ",", MAX_SINKS, &num_sinks);
		pw_properties_set(props, "slaves", NULL);
	}

	if ((str = pw_properties_get(props, "adjust_time")) != NULL) {
		pw_log_info("The `adjust_time` modarg is ignored");
		pw_properties_set(props, "adjust_time", NULL);
	}

	if ((str = pw_properties_get(props, "resample_method")) != NULL) {
		pw_log_info("The `resample_method` modarg is ignored");
		pw_properties_set(props, "resample_method", NULL);
	}

	if (module_args_to_audioinfo(impl, props, &info) < 0) {
		res = -EINVAL;
		goto out;
	}

	module = module_new(impl, &module_combine_sink_methods, sizeof(*d));
	if (module == NULL) {
		res = -errno;
		goto out;
	}

	module->props = props;
	d = module->user_data;
	d->module = module;
	d->info = info;
	d->sink_name = sink_name;
	d->sink_names = sink_names;
	d->sinks_pending = (sink_names == NULL) ? 0 : num_sinks;
	for (i = 0; i < MAX_SINKS; i++) {
		d->streams[i].stream = NULL;
		d->streams[i].cleanup = false;
	}

	return module;
out:
	pw_properties_free(props);
	free(sink_name);
	pw_free_strv(sink_names);
	errno = -res;
	return NULL;
}
