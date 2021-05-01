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
#include <dlfcn.h>
#include <unistd.h>

#include "config.h"

#include <ladspa.h>

#include <spa/utils/result.h>
#include <spa/utils/json.h>
#include <spa/param/profiler.h>
#include <spa/debug/pod.h>

#include <pipewire/private.h>
#include <pipewire/impl.h>
#include <extensions/profiler.h>

#define NAME "ladspa-filter"

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create ladspa filter streams" },
	{ PW_KEY_MODULE_USAGE, " [ remote.name=<remote> ] "
				"[ node.latency=<latency as fraction> ] "
				"[ node.name=<name of the nodes> ] "
				"[ node.description=<description of the nodes> ] "
				"[ audio.rate=<sample rate> ] "
				"[ audio.channels=<number of channels> ] "
				"[ audio.position=<channel map> ] "
				"ladspa.plugin=<plugin name> "
				"ladspa.label=<label name> "
				"ladspa.control = [ { name="" value=0.0 } ,... ] "
				"ladspa.inputs = [ <name>... ] "
				"ladspa.outputs = [ <name>... ] "
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

#define MAX_PORTS 64

struct impl {
	struct pw_context *context;

	struct pw_impl_module *module;
	struct pw_work_queue *work;

	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct pw_properties *capture_props;
	struct pw_stream *capture;
	struct spa_hook capture_listener;
	struct spa_audio_info_raw capture_info;

	struct pw_properties *playback_props;
	struct pw_stream *playback;
	struct spa_hook playback_listener;
	struct spa_audio_info_raw playback_info;

	unsigned int do_disconnect:1;
	unsigned int unloading:1;

	uint32_t rate;

	void *handle;
	uint32_t n_input;
	uint32_t n_output;
	uint32_t n_control;
	uint32_t n_notify;
	unsigned long input[MAX_PORTS];
	unsigned long output[MAX_PORTS];
	unsigned long control[MAX_PORTS];
	unsigned long notify[MAX_PORTS];
	const LADSPA_Descriptor *desc;
	uint32_t n_hndl;
	LADSPA_Handle hndl[MAX_PORTS];
	LADSPA_Data control_data[MAX_PORTS];
	LADSPA_Data notify_data[MAX_PORTS];
};

static void do_unload_module(void *obj, void *data, int res, uint32_t id)
{
	struct impl *impl = data;
	pw_impl_module_destroy(impl->module);
}
static void unload_module(struct impl *impl)
{
	if (!impl->unloading) {
		impl->unloading = true;
		pw_work_queue_add(impl->work, impl, 0, do_unload_module, impl);
	}
}

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
	uint32_t i, size = 0, n_hndl = impl->n_hndl;
	int32_t stride = 0;
	const LADSPA_Descriptor *desc = impl->desc;

	if ((in = pw_stream_dequeue_buffer(impl->capture)) == NULL)
		pw_log_warn("out of capture buffers: %m");

	if ((out = pw_stream_dequeue_buffer(impl->playback)) == NULL)
		pw_log_warn("out of playback buffers: %m");

	if (in == NULL || out == NULL)
		goto done;

	for (i = 0; i < in->buffer->n_datas; i++) {
		struct spa_data *ds = &in->buffer->datas[i];
		desc->connect_port(impl->hndl[i % n_hndl],
				impl->input[i % impl->n_input],
				SPA_MEMBER(ds->data, ds->chunk->offset, void));
		size = SPA_MAX(size, ds->chunk->size);
		stride = SPA_MAX(stride, ds->chunk->stride);
	}
	for (i = 0; i < out->buffer->n_datas; i++) {
		struct spa_data *dd = &out->buffer->datas[i];
		desc->connect_port(impl->hndl[i % n_hndl],
				impl->output[i % impl->n_output], dd->data);
		dd->chunk->offset = 0;
		dd->chunk->size = size;
		dd->chunk->stride = stride;
	}

	for (i = 0; i < n_hndl; i++)
		desc->run(impl->hndl[i], size / sizeof(float));

done:
	if (in != NULL)
		pw_stream_queue_buffer(impl->capture, in);
	if (out != NULL)
		pw_stream_queue_buffer(impl->playback, out);
}

static void set_control_value(struct impl *impl, const char *name, float value)
{
	uint32_t i;
	for (i = 0; i < impl->n_control; i++) {
		uint32_t p = impl->control[i];
		if (strcmp(impl->desc->PortNames[p], name) == 0) {
			pw_log_info("set '%s' to %f", name, value);
			impl->control_data[i] = value;
			return;
		}
	}
}

static void param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct impl *impl = data;
	const struct spa_pod_prop *prop;
	struct spa_pod_parser prs;
	struct spa_pod_frame f;

	if (id != SPA_PARAM_Props)
		return;

	if ((prop = spa_pod_find_prop(param, NULL, SPA_PROP_paramStruct)) == NULL ||
	    !spa_pod_is_struct(&prop->value))
		return;

	spa_pod_parser_pod(&prs, &prop->value);
	if (spa_pod_parser_push_struct(&prs, &f) < 0)
		return;

	while (true) {
		const char *name;
		float value;

		if (spa_pod_parser_get_string(&prs, &name) < 0)
			break;
		if (spa_pod_parser_get_float(&prs, &value) < 0)
			break;

		set_control_value(impl, name, value);
	}
}

static const struct pw_stream_events in_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = capture_destroy,
	.process = capture_process,
	.param_changed = param_changed
};

static void playback_destroy(void *d)
{
	struct impl *impl = d;
	spa_hook_remove(&impl->playback_listener);
	impl->playback = NULL;
}

static const struct pw_stream_events out_stream_events = {
	PW_VERSION_STREAM_EVENTS,
	.destroy = playback_destroy,
	.param_changed = param_changed
};

static int setup_streams(struct impl *impl)
{
	int res;
	uint32_t n_params;
	const struct spa_pod *params[1];
	uint8_t buffer[1024];
	struct spa_pod_builder b;

	impl->capture = pw_stream_new(impl->core,
			"ladspa capture", impl->capture_props);
	impl->capture_props = NULL;
	if (impl->capture == NULL)
		return -errno;

	pw_stream_add_listener(impl->capture,
			&impl->capture_listener,
			&in_stream_events, impl);

	impl->playback = pw_stream_new(impl->core,
			"ladspa playback", impl->playback_props);
	impl->playback_props = NULL;
	if (impl->playback == NULL)
		return -errno;

	pw_stream_add_listener(impl->playback,
			&impl->playback_listener,
			&out_stream_events, impl);

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&impl->capture_info);

	if ((res = pw_stream_connect(impl->capture,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params)) < 0)
		return res;

	n_params = 0;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));
	params[n_params++] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat,
			&impl->playback_info);

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

static float get_default(struct impl *impl, uint32_t p)
{
	const LADSPA_Descriptor *d = impl->desc;
	LADSPA_PortRangeHintDescriptor hint = d->PortRangeHints[p].HintDescriptor;
	LADSPA_Data lower, upper, def;

	lower = d->PortRangeHints[p].LowerBound;
	upper = d->PortRangeHints[p].UpperBound;

	if (LADSPA_IS_HINT_SAMPLE_RATE(hint)) {
		lower *= (LADSPA_Data) impl->rate;
		upper *= (LADSPA_Data) impl->rate;
	}

	switch (hint & LADSPA_HINT_DEFAULT_MASK) {
	case LADSPA_HINT_DEFAULT_MINIMUM:
		def =  lower;
		break;
	case LADSPA_HINT_DEFAULT_MAXIMUM:
		def = upper;
		break;
	case LADSPA_HINT_DEFAULT_LOW:
		if (LADSPA_IS_HINT_LOGARITHMIC(hint))
			def = (LADSPA_Data) exp(log(lower) * 0.75 + log(upper) * 0.25);
		else
			def = (LADSPA_Data) (lower * 0.75 + upper * 0.25);
		break;
	case LADSPA_HINT_DEFAULT_MIDDLE:
		if (LADSPA_IS_HINT_LOGARITHMIC(hint))
			def = (LADSPA_Data) exp(log(lower) * 0.5 + log(upper) * 0.5);
		else
			def = (LADSPA_Data) (lower * 0.5 + upper * 0.5);
		break;
	case LADSPA_HINT_DEFAULT_HIGH:
		if (LADSPA_IS_HINT_LOGARITHMIC(hint))
			def = (LADSPA_Data) exp(log(lower) * 0.25 + log(upper) * 0.75);
		else
			def = (LADSPA_Data) (lower * 0.25 + upper * 0.75);
		break;
	case LADSPA_HINT_DEFAULT_0:
		def = 0;
		break;
	case LADSPA_HINT_DEFAULT_1:
		def = 1;
		break;
	case LADSPA_HINT_DEFAULT_100:
		def = 100;
		break;
	case LADSPA_HINT_DEFAULT_440:
		def = 440;
		break;
	default:
		def = 0.5 * upper;
		break;
	}
	if (LADSPA_IS_HINT_INTEGER(hint))
		def = roundf(def);
	return def;
}

static const LADSPA_Descriptor *find_descriptor(LADSPA_Descriptor_Function desc_func,
		const char *label)
{
	unsigned long i;

	for (i = 0; ;i++) {
		const LADSPA_Descriptor *desc = desc_func(i);
		if (desc == NULL)
			break;
		if (strcmp(desc->Label, label) == 0)
			return desc;
	}
	return NULL;
}

static int load_ladspa(struct impl *impl, struct pw_properties *props)
{
	char path[PATH_MAX];
	const char *e, *plugin, *label;
	LADSPA_Descriptor_Function desc_func;
	const LADSPA_Descriptor *d;
	uint32_t i, j, p;
	int res;

	if ((e = getenv("LADSPA_PATH")) == NULL)
		e = "/usr/lib64/ladspa";

	if ((plugin = pw_properties_get(props, "ladspa.plugin")) == NULL)
		return -EINVAL;
	if ((label = pw_properties_get(props, "ladspa.label")) == NULL)
		return -EINVAL;

	snprintf(path, sizeof(path), "%s/%s.so", e, plugin);

	impl->handle = dlopen(path, RTLD_NOW);
	if (impl->handle == NULL) {
		pw_log_error("plugin dlopen failed %s: %s", path, dlerror());
		res = -ENOENT;
		goto exit;
	}

	desc_func = (LADSPA_Descriptor_Function)dlsym(impl->handle,
                                                      "ladspa_descriptor");

	if (desc_func == NULL) {
		pw_log_error("cannot find descriptor function from %s: %s",
                       path, dlerror());
		res = -ENOSYS;
		goto exit;
	}
	if ((d = find_descriptor(desc_func, label)) == NULL) {
		pw_log_error("cannot find label %s", label);
		res = -ENOENT;
		goto exit;
	}
	impl->desc = d;

	pw_properties_setf(props, "ladspa.unique-id", "%lu", impl->desc->UniqueID);
	pw_properties_setf(props, "ladspa.name", "%s", impl->desc->Name);
	pw_properties_setf(props, "ladspa.maker", "%s", impl->desc->Maker);
	pw_properties_setf(props, "ladspa.copyright", "%s", impl->desc->Copyright);

	for (p = 0; p < d->PortCount; p++) {
		if (LADSPA_IS_PORT_AUDIO(d->PortDescriptors[p])) {
			if (LADSPA_IS_PORT_INPUT(d->PortDescriptors[p]))
				impl->input[impl->n_input++] = p;
			else if (LADSPA_IS_PORT_OUTPUT(d->PortDescriptors[p]))
				impl->output[impl->n_output++] = p;
		} else if (LADSPA_IS_PORT_CONTROL(d->PortDescriptors[p])) {
			if (LADSPA_IS_PORT_INPUT(d->PortDescriptors[p]))
				impl->control[impl->n_control++] = p;
			else if (LADSPA_IS_PORT_OUTPUT(d->PortDescriptors[p]))
				impl->notify[impl->n_notify++] = p;
		}
	}
	if (impl->n_input == 0 || impl->n_output == 0) {
		pw_log_error("plugin has 0 input or 0 output ports");
		res = -ENOTSUP;
		goto exit;
	}
	for (j = 0; j < impl->n_control; j++) {
		p = impl->control[j];
		impl->control_data[j] = get_default(impl, p);
		pw_log_info("control (%s) %d set to %f", d->PortNames[p], p, impl->control_data[j]);
	}

	if (impl->capture_info.channels == 0)
		impl->capture_info.channels = impl->n_input;
	if (impl->playback_info.channels == 0)
		impl->playback_info.channels = impl->n_output;

	impl->n_hndl = impl->capture_info.channels / impl->n_input;
	if (impl->n_hndl != impl->playback_info.channels / impl->n_output) {
		pw_log_error("invalid channels");
		res = -EINVAL;
		goto exit;
	}
	pw_log_info("using %d instances", impl->n_hndl);

	for (i = 0; i < impl->n_hndl;i++) {
		if ((impl->hndl[i] = d->instantiate(d, impl->rate)) == NULL) {
			pw_log_error("cannot create plugin instance");
			res = -ENOMEM;
			goto exit;
		}

		for (j = 0; j < impl->n_control; j++) {
			p = impl->control[j];
			d->connect_port(impl->hndl[i], p, &impl->control_data[j]);
		}
		for (j = 0; j < impl->n_notify; j++) {
			p = impl->notify[j];
			d->connect_port(impl->hndl[i], p, &impl->notify_data[j]);
		}
		if (d->activate)
			d->activate(impl->hndl[i]);
	}
	return 0;

exit:
	if (impl->handle != NULL)
		dlclose(impl->handle);
	impl->handle = NULL;
	return res;
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct impl *impl = data;

	pw_log_error("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		unload_module(impl);
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
	unload_module(impl);
}

static const struct pw_proxy_events core_proxy_events = {
	.destroy = core_destroy,
};

static void impl_destroy(struct impl *impl)
{
	if (impl->capture)
		pw_stream_destroy(impl->capture);
	if (impl->playback)
		pw_stream_destroy(impl->playback);
	if (impl->core && impl->do_disconnect)
		pw_core_disconnect(impl->core);
	if (impl->capture_props)
		pw_properties_free(impl->capture_props);
	if (impl->playback_props)
		pw_properties_free(impl->playback_props);
	pw_work_queue_cancel(impl->work, impl, SPA_ID_INVALID);
	free(impl);
}

static void module_destroy(void *data)
{
	struct impl *impl = data;
	impl->unloading = true;
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
		if (strcmp(name, spa_debug_type_short_name(spa_type_audio_channel[i].name)) == 0)
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

static void parse_audio_info(struct pw_properties *props, struct spa_audio_info_raw *info)
{
	const char *str;

	*info = SPA_AUDIO_INFO_RAW_INIT(
			.format = SPA_AUDIO_FORMAT_F32P);
	if ((str = pw_properties_get(props, PW_KEY_AUDIO_RATE)) != NULL)
		info->rate = atoi(str);
	if ((str = pw_properties_get(props, PW_KEY_AUDIO_CHANNELS)) != NULL)
		info->channels = atoi(str);
	if ((str = pw_properties_get(props, SPA_KEY_AUDIO_POSITION)) != NULL)
		parse_position(info, str, strlen(str));
}

static void copy_props(struct impl *impl, struct pw_properties *props, const char *key)
{
	const char *str;
	if ((str = pw_properties_get(props, key)) != NULL) {
		if (pw_properties_get(impl->capture_props, key) == NULL)
			pw_properties_set(impl->capture_props, key, str);
		if (pw_properties_get(impl->playback_props, key) == NULL)
			pw_properties_set(impl->playback_props, key, str);
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
	int res;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	pw_log_debug("module %p: new %s", impl, args);

	if (args)
		props = pw_properties_new_string(args);
	else
		props = pw_properties_new(NULL, NULL);

	if (props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->capture_props = pw_properties_new(NULL, NULL);
	impl->playback_props = pw_properties_new(NULL, NULL);
	if (impl->capture_props == NULL || impl->playback_props == NULL) {
		res = -errno;
		pw_log_error( "can't create properties: %m");
		goto error;
	}

	impl->module = module;
	impl->context = context;
	impl->work = pw_context_get_work_queue(context);
	impl->rate = 48000;

	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_GROUP, "ladspa-filter-%u", id);
	if (pw_properties_get(props, PW_KEY_NODE_VIRTUAL) == NULL)
		pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");

	if ((str = pw_properties_get(props, "capture.props")) != NULL)
		pw_properties_update_string(impl->capture_props, str, strlen(str));
	if ((str = pw_properties_get(props, "playback.props")) != NULL)
		pw_properties_update_string(impl->playback_props, str, strlen(str));

	copy_props(impl, props, PW_KEY_AUDIO_RATE);
	copy_props(impl, props, PW_KEY_AUDIO_CHANNELS);
	copy_props(impl, props, SPA_KEY_AUDIO_POSITION);
	copy_props(impl, props, PW_KEY_NODE_NAME);
	copy_props(impl, props, PW_KEY_NODE_DESCRIPTION);
	copy_props(impl, props, PW_KEY_NODE_GROUP);
	copy_props(impl, props, PW_KEY_NODE_LATENCY);
	copy_props(impl, props, PW_KEY_NODE_VIRTUAL);

	parse_audio_info(impl->capture_props, &impl->capture_info);
	parse_audio_info(impl->playback_props, &impl->playback_info);

	if ((res = load_ladspa(impl, props)) < 0) {
		pw_log_error("can't load ladspa: %s", spa_strerror(res));
		goto error;
	}
	copy_props(impl, props, "ladspa.unique-id");
	copy_props(impl, props, "ladspa.name");
	copy_props(impl, props, "ladspa.maker");
	copy_props(impl, props, "ladspa.copyright");

	if (pw_properties_get(impl->capture_props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(impl->capture_props, PW_KEY_MEDIA_NAME, "%s input",
				impl->desc->Name);
	if (pw_properties_get(impl->playback_props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(impl->playback_props, PW_KEY_MEDIA_NAME, "%s output",
				impl->desc->Name);

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

	pw_properties_free(props);

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

error:
	if (props)
		pw_properties_free(props);
	impl_destroy(impl);
	return res;
}
