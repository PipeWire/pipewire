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

#define NAME "filter-chain"

static const struct spa_dict_item module_props[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Create filter chain streams" },
	{ PW_KEY_MODULE_USAGE, " [ remote.name=<remote> ] "
				"[ node.latency=<latency as fraction> ] "
				"[ node.name=<name of the nodes> ] "
				"[ node.description=<description of the nodes> ] "
				"[ audio.rate=<sample rate> ] "
				"[ audio.channels=<number of channels> ] "
				"[ audio.position=<channel map> ] "
				"filter.graph = [ "
				"    nodes = [ "
				"        { "
				"          type = ladspa "
				"          name = <name> "
				"          plugin = <plugin> "
				"          label = <label> "
				"          control = { "
				"             <controlname> = <value> ... "
				"          } "
				"        } "
				"    ] "
				"    links = [ "
				"        { output = <portname> input = <portname> } ... "
				"    ] "
				"    inputs = [ <portname> ... ] "
				"    outputs = [ <portname> ... ] "
				"] "
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

#define MAX_NODES 1
#define MAX_LINKS 0
#define MAX_PORTS 64
#define MAX_CONTROLS 256
#define MAX_SAMPLES 8192

struct ladspa_handle {
	struct spa_list link;
	int ref;
	char path[PATH_MAX];
	void *handle;
	LADSPA_Descriptor_Function desc_func;
	struct spa_list descriptor_list;
};

struct ladspa_descriptor {
	struct spa_list link;
	int ref;
	struct ladspa_handle *handle;
	char label[256];
	const LADSPA_Descriptor *desc;
	struct spa_list node_list;

	uint32_t n_input;
	uint32_t n_output;
	uint32_t n_control;
	uint32_t n_notify;
	unsigned long input[MAX_PORTS];
	unsigned long output[MAX_PORTS];
	unsigned long control[MAX_PORTS];
	unsigned long notify[MAX_PORTS];
	LADSPA_Data default_control[MAX_PORTS];
};

struct node {
	struct spa_list link;
	struct ladspa_descriptor *desc;

	uint32_t pending;
	uint32_t required;

	char name[256];
	LADSPA_Data control_data[MAX_PORTS];
	LADSPA_Data notify_data[MAX_PORTS];

	uint32_t n_hndl;
	LADSPA_Handle hndl[MAX_PORTS];
};

struct link {
	struct spa_list link;
	uint32_t output_node;
	uint32_t output_port;
	uint32_t input_node;
	uint32_t input_port;
	LADSPA_Data control_data;
	LADSPA_Data audio_data[MAX_SAMPLES];
};

struct graph {
	struct impl *impl;

	struct spa_list node_list;
	struct spa_list link_list;

	uint32_t n_input;
	const LADSPA_Descriptor *in_desc[MAX_PORTS];
	LADSPA_Handle *in_hndl[MAX_PORTS];
	uint32_t in_port[MAX_PORTS];

	uint32_t n_output;
	const LADSPA_Descriptor *out_desc[MAX_PORTS];
	LADSPA_Handle *out_hndl[MAX_PORTS];
	uint32_t out_port[MAX_PORTS];

	uint32_t n_hndl;
	const LADSPA_Descriptor *desc[MAX_PORTS];
	LADSPA_Handle *hndl[MAX_PORTS];

	uint32_t n_control;
	struct node *control_node[MAX_CONTROLS];
	uint32_t control_index[MAX_CONTROLS];
};

struct impl {
	struct pw_context *context;

	struct pw_impl_module *module;
	struct pw_work_queue *work;

	struct spa_hook module_listener;

	struct pw_core *core;
	struct spa_hook core_proxy_listener;
	struct spa_hook core_listener;

	struct spa_list ladspa_handle_list;

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

	struct graph graph;
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
	struct graph *graph = &impl->graph;
	uint32_t i, size = 0, n_hndl = graph->n_hndl;
	int32_t stride = 0;

	if ((in = pw_stream_dequeue_buffer(impl->capture)) == NULL)
		pw_log_warn("out of capture buffers: %m");

	if ((out = pw_stream_dequeue_buffer(impl->playback)) == NULL)
		pw_log_warn("out of playback buffers: %m");

	if (in == NULL || out == NULL)
		goto done;

	for (i = 0; i < in->buffer->n_datas; i++) {
		struct spa_data *ds = &in->buffer->datas[i];
		graph->in_desc[i]->connect_port(graph->in_hndl[i],
				graph->in_port[i],
				SPA_MEMBER(ds->data, ds->chunk->offset, void));
		size = SPA_MAX(size, ds->chunk->size);
		stride = SPA_MAX(stride, ds->chunk->stride);
	}
	for (i = 0; i < out->buffer->n_datas; i++) {
		struct spa_data *dd = &out->buffer->datas[i];
		graph->out_desc[i]->connect_port(graph->out_hndl[i],
				graph->out_port[i], dd->data);
		dd->chunk->offset = 0;
		dd->chunk->size = size;
		dd->chunk->stride = stride;
	}
	for (i = 0; i < n_hndl; i++)
		graph->desc[i]->run(graph->hndl[i], size / sizeof(float));

done:
	if (in != NULL)
		pw_stream_queue_buffer(impl->capture, in);
	if (out != NULL)
		pw_stream_queue_buffer(impl->playback, out);
}

static float get_default(struct impl *impl, struct ladspa_descriptor *desc, uint32_t p)
{
	const LADSPA_Descriptor *d = desc->desc;
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
		def = lower;
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
		if (upper == lower)
			def = upper;
		else
			def = SPA_CLAMP(0.5 * upper, lower, upper);
		break;
	}
	if (LADSPA_IS_HINT_INTEGER(hint))
		def = roundf(def);
	return def;
}

static struct spa_pod *get_prop_info(struct graph *graph, struct spa_pod_builder *b, uint32_t idx)
{
	struct spa_pod_frame f[2];
	struct impl *impl = graph->impl;
	struct node *node = graph->control_node[idx];
	struct ladspa_descriptor *desc = node->desc;
	uint32_t i = graph->control_index[idx];
	uint32_t p = desc->control[i];
	const LADSPA_Descriptor *d = desc->desc;
	LADSPA_PortRangeHintDescriptor hint = d->PortRangeHints[p].HintDescriptor;
	float def, upper, lower;

	def = get_default(impl, desc, p);
	lower = d->PortRangeHints[p].LowerBound;
	upper = d->PortRangeHints[p].UpperBound;

	if (LADSPA_IS_HINT_SAMPLE_RATE(hint)) {
		lower *= (LADSPA_Data) impl->rate;
		upper *= (LADSPA_Data) impl->rate;
	}

	spa_pod_builder_push_object(b, &f[0],
			SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
	spa_pod_builder_add (b,
			SPA_PROP_INFO_id, SPA_POD_Id(SPA_PROP_START_CUSTOM + idx),
			SPA_PROP_INFO_name, SPA_POD_String(d->PortNames[p]),
			0);
	spa_pod_builder_prop(b, SPA_PROP_INFO_type, 0);
	if (lower == upper) {
		spa_pod_builder_float(b, def);
	} else {
		spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Range, 0);
		spa_pod_builder_float(b, def);
		spa_pod_builder_float(b, lower);
		spa_pod_builder_float(b, upper);
		spa_pod_builder_pop(b, &f[1]);
	}
	return spa_pod_builder_pop(b, &f[0]);
}

static struct spa_pod *get_props_param(struct graph *graph, struct spa_pod_builder *b)
{
	struct spa_pod_frame f[2];
	uint32_t i;

	spa_pod_builder_push_object(b, &f[0],
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	for (i = 0; i < graph->n_control; i++) {
		struct node *node = graph->control_node[i];
		uint32_t idx = graph->control_index[i];
		spa_pod_builder_prop(b, SPA_PROP_START_CUSTOM + i, 0);
		spa_pod_builder_float(b, node->control_data[idx]);
	}
	return spa_pod_builder_pop(b, &f[0]);
}

static int set_control_value(struct node *node, const char *name, float *value)
{
	uint32_t i;
	struct ladspa_descriptor *desc = node->desc;

	for (i = 0; i < desc->n_control; i++) {
		uint32_t p = desc->control[i];
		if (strcmp(desc->desc->PortNames[p], name) == 0) {
			float old = node->control_data[i];
			node->control_data[i] = value ? *value : desc->default_control[i];
			pw_log_info("control %d ('%s') to %f", i, name, node->control_data[i]);
			return old == node->control_data[i] ? 0 : 1;
		}
	}
	return 0;
}

static void param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
	struct impl *impl = data;
	const struct spa_pod_prop *prop;
	struct spa_pod_object *obj = (struct spa_pod_object *) param;
	struct graph *graph = &impl->graph;
	int changed = 0;

	if (id != SPA_PARAM_Props)
		return;

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		uint32_t idx;
		float value;
		struct node *node;

		if (prop->key < SPA_PROP_START_CUSTOM)
			continue;
		idx = prop->key - SPA_PROP_START_CUSTOM;
		if (idx >= graph->n_control)
			continue;

		if (spa_pod_get_float(&prop->value, &value) < 0)
			continue;

		node = graph->control_node[idx];
		idx = graph->control_index[idx];

		if (node->control_data[idx] != value) {
			node->control_data[idx] = value;
			changed++;
			pw_log_info("control %d to %f", idx, node->control_data[idx]);
		}
	}
	if (changed > 0) {
		uint8_t buffer[1024];
		struct spa_pod_builder b;
		const struct spa_pod *params[1];

		spa_pod_builder_init(&b, buffer, sizeof(buffer));
		params[0] = get_props_param(graph, &b);

		pw_stream_update_params(impl->playback, params, 1);
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

static int builder_overflow(void *data, uint32_t size)
{
	struct spa_pod_builder *b = data;
	b->size = SPA_ROUND_UP_N(size, 4096);
	if ((b->data = realloc(b->data, b->size)) == NULL)
		return -errno;
        return 0;
}

static const struct spa_pod_builder_callbacks builder_callbacks = {
	SPA_VERSION_POD_BUILDER_CALLBACKS,
	.overflow = builder_overflow
};

static int setup_streams(struct impl *impl)
{
	int res;
	uint32_t i, n_params;
	const struct spa_pod *params[256];
	struct spa_pod_builder b;
	struct graph *graph = &impl->graph;

	impl->capture = pw_stream_new(impl->core,
			"filter capture", impl->capture_props);
	impl->capture_props = NULL;
	if (impl->capture == NULL)
		return -errno;

	pw_stream_add_listener(impl->capture,
			&impl->capture_listener,
			&in_stream_events, impl);

	impl->playback = pw_stream_new(impl->core,
			"filter playback", impl->playback_props);
	impl->playback_props = NULL;
	if (impl->playback == NULL)
		return -errno;

	pw_stream_add_listener(impl->playback,
			&impl->playback_listener,
			&out_stream_events, impl);

	n_params = 0;
	spa_pod_builder_init(&b, NULL, 0);
	spa_pod_builder_set_callbacks(&b, &builder_callbacks, &b);

	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &impl->capture_info);

	for (i = 0; i < graph->n_control; i++)
		params[n_params++] = get_prop_info(graph, &b, i);

	params[n_params++] = get_props_param(graph, &b);

	res = pw_stream_connect(impl->capture,
			PW_DIRECTION_INPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params);
	free(b.data);
	if (res < 0)
		return res;

	n_params = 0;
	spa_pod_builder_init(&b, NULL, 0);
	spa_pod_builder_set_callbacks(&b, &builder_callbacks, &b);
	params[n_params++] = spa_format_audio_raw_build(&b,
			SPA_PARAM_EnumFormat, &impl->playback_info);

	res = pw_stream_connect(impl->playback,
			PW_DIRECTION_OUTPUT,
			PW_ID_ANY,
			PW_STREAM_FLAG_AUTOCONNECT |
			PW_STREAM_FLAG_MAP_BUFFERS |
			PW_STREAM_FLAG_RT_PROCESS,
			params, n_params);
	free(b.data);

	if (res < 0)
		return res;


	return 0;
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

static struct node *find_node(struct graph *graph, const char *name)
{
	struct node *node;
	spa_list_for_each(node, &graph->node_list, link) {
		if (strcmp(node->name, name) == 0)
			return node;
	}
	return NULL;
}

static uint32_t find_port(struct node *node, const char *name, int mask)
{
	uint32_t p;
	const LADSPA_Descriptor *d = node->desc->desc;
	for (p = 0; p < d->PortCount; p++) {
		if ((d->PortDescriptors[p] & mask) != mask)
			continue;
		if (strcmp(d->PortNames[p], name) == 0)
			return p;
	}
	return SPA_ID_INVALID;
}

static uint32_t count_array(struct spa_json *json)
{
	struct spa_json it = *json;
	char v[256];
	uint32_t count = 0;
	while (spa_json_get_string(&it, v, sizeof(v)) > 0)
		count++;
	return count;
}

static void ladspa_handle_unref(struct ladspa_handle *hndl)
{
	if (--hndl->ref > 0)
		return;

	dlclose(hndl->handle);
	free(hndl);
}

static struct ladspa_handle *ladspa_handle_load(struct impl *impl, const char *plugin)
{
	struct ladspa_handle *hndl;
	char path[PATH_MAX];
	const char *e;
	int res;

	if ((e = getenv("LADSPA_PATH")) == NULL)
		e = "/usr/lib64/ladspa";

	snprintf(path, sizeof(path), "%s/%s.so", e, plugin);

	spa_list_for_each(hndl, &impl->ladspa_handle_list, link) {
		if (strcmp(hndl->path, path) == 0) {
			hndl->ref++;
			return hndl;
		}
	}

	hndl = calloc(1, sizeof(*hndl));
	hndl->ref = 1;
	snprintf(hndl->path, sizeof(hndl->path), "%s", path);

	hndl->handle = dlopen(path, RTLD_NOW);
	if (hndl->handle == NULL) {
		pw_log_error("plugin dlopen failed %s: %s", path, dlerror());
		res = -ENOENT;
		goto exit;
	}

	hndl->desc_func = (LADSPA_Descriptor_Function)dlsym(hndl->handle,
                                                      "ladspa_descriptor");

	if (hndl->desc_func == NULL) {
		pw_log_error("cannot find descriptor function from %s: %s",
                       path, dlerror());
		res = -ENOSYS;
		goto exit;
	}
	spa_list_init(&hndl->descriptor_list);

	return hndl;

exit:
	if (hndl->handle != NULL)
		dlclose(hndl->handle);
	free(hndl);
	errno = -res;
	return NULL;
}

static void ladspa_descriptor_unref(struct impl *impl, struct ladspa_descriptor *desc)
{
	if (--desc->ref > 0)
		return;

	spa_list_remove(&desc->link);
	ladspa_handle_unref(desc->handle);
	free(desc);
}

static struct ladspa_descriptor *ladspa_descriptor_load(struct impl *impl,
		const char *plugin, const char *label)
{
	struct ladspa_handle *hndl;
	struct ladspa_descriptor *desc;
	const LADSPA_Descriptor *d;
	uint32_t i;
	unsigned long p;
	int res;

	if ((hndl = ladspa_handle_load(impl, plugin)) == NULL)
		return NULL;

	spa_list_for_each(desc, &hndl->descriptor_list, link) {
		if (strcmp(desc->label, label) == 0) {
			desc->ref++;
			return desc;
		}
	}

	desc = calloc(1, sizeof(*desc));
	desc->ref = 1;
	desc->handle = hndl;

	if ((d = find_descriptor(hndl->desc_func, label)) == NULL) {
		pw_log_error("cannot find label %s", label);
		res = -ENOENT;
		goto exit;
	}
	desc->desc = d;
	snprintf(desc->label, sizeof(desc->label), "%s", label);
	spa_list_init(&desc->node_list);

	for (p = 0; p < d->PortCount; p++) {
		if (LADSPA_IS_PORT_AUDIO(d->PortDescriptors[p])) {
			if (LADSPA_IS_PORT_INPUT(d->PortDescriptors[p])) {
				pw_log_info("using port %lu ('%s') as input %d", p,
						d->PortNames[p], desc->n_input);
				desc->input[desc->n_input++] = p;
			}
			else if (LADSPA_IS_PORT_OUTPUT(d->PortDescriptors[p])) {
				pw_log_info("using port %lu ('%s') as output %d", p,
						d->PortNames[p], desc->n_output);
				desc->output[desc->n_output++] = p;
			}
		} else if (LADSPA_IS_PORT_CONTROL(d->PortDescriptors[p])) {
			if (LADSPA_IS_PORT_INPUT(d->PortDescriptors[p])) {
				pw_log_info("using port %lu ('%s') as control %d", p,
						d->PortNames[p], desc->n_control);
				desc->control[desc->n_control++] = p;
			}
			else if (LADSPA_IS_PORT_OUTPUT(d->PortDescriptors[p])) {
				pw_log_info("using port %lu ('%s') as notify %d", p,
						d->PortNames[p], desc->n_notify);
				desc->notify[desc->n_notify++] = p;
			}
		}
	}
	if (desc->n_input == 0 || desc->n_output == 0) {
		pw_log_error("plugin has no input or no output ports");
		res = -ENOTSUP;
		goto exit;
	}
	for (i = 0; i < desc->n_control; i++) {
		p = desc->control[i];
		desc->default_control[i] = get_default(impl, desc, p);
		pw_log_info("control %d ('%s') default to %f", i,
				d->PortNames[p], desc->default_control[i]);
	}
	spa_list_append(&hndl->descriptor_list, &desc->link);

	return desc;

exit:
	if (hndl != NULL)
		ladspa_handle_unref(hndl);
	free(desc);
	errno = -res;
	return NULL;
}

/**
 * {
 *   "Reverb tail" = 2.0
 *   ...
 * }
 */
static int parse_control(struct node *node, struct spa_json *control)
{
	struct spa_json it[1];
	char key[256];

        if (spa_json_enter_object(control, &it[0]) <= 0)
		return -EINVAL;

	while (spa_json_get_string(&it[0], key, sizeof(key)) > 0) {
		float fl;
		if (spa_json_get_float(&it[0], &fl) <= 0)
			break;
		set_control_value(node, key, &fl);
	}
	return 0;
}

/**
 * type = ladspa
 * name = rev
 * plugin = g2reverb
 * label = G2reverb
 * control = [
 *     ...
 * ]
 */
static int load_node(struct graph *graph, struct spa_json *json)
{
	struct spa_json it[1];
	struct ladspa_descriptor *desc;
	struct node *node;
	const char *val;
	char key[256];
	char type[256] = "";
	char name[256] = "";
	char plugin[256] = "";
	char label[256] = "";
	bool have_control = false;
	uint32_t i;

	while (spa_json_get_string(json, key, sizeof(key)) > 0) {
		if (strcmp("type", key) == 0) {
			if (spa_json_get_string(json, type, sizeof(type)) <= 0) {
				pw_log_error("type expects a string");
				return -EINVAL;
			}
			if (strcmp(type, "ladspa") != 0)
				return -ENOTSUP;
		} else if (strcmp("name", key) == 0) {
			if (spa_json_get_string(json, name, sizeof(name)) <= 0) {
				pw_log_error("name expects a string");
				return -EINVAL;
			}
		} else if (strcmp("plugin", key) == 0) {
			if (spa_json_get_string(json, plugin, sizeof(plugin)) <= 0) {
				pw_log_error("plugin expects a string");
				return -EINVAL;
			}
		} else if (strcmp("label", key) == 0) {
			if (spa_json_get_string(json, label, sizeof(label)) <= 0) {
				pw_log_error("label expects a string");
				return -EINVAL;
			}
		} else if (strcmp("control", key) == 0) {
			it[0] = *json;
			have_control = true;
		} else if (spa_json_next(json, &val) < 0)
			break;
	}

	pw_log_info("loading %s %s", plugin, label);

	if ((desc = ladspa_descriptor_load(graph->impl, plugin, label)) == NULL)
		return -errno;

	node = calloc(1, sizeof(*node));
	if (node == NULL)
		return -errno;

	node->desc = desc;
	snprintf(node->name, sizeof(node->name), "%s", name);

	for (i = 0; i < desc->n_control; i++)
		node->control_data[i] = desc->default_control[i];

	if (have_control)
		parse_control(node, &it[0]);

	spa_list_append(&graph->node_list, &node->link);

	return 0;
}

static int setup_graph(struct graph *graph, struct spa_json *inputs, struct spa_json *outputs)
{
	struct impl *impl = graph->impl;
	struct node *node, *first, *last;
	uint32_t i, j, n_input, n_output, n_hndl;
	int res;
	unsigned long p;
	struct ladspa_descriptor *desc;
	const LADSPA_Descriptor *d;
	char v[256], *col, *node_name, *port_name;

	graph->n_input = 0;
	graph->n_output = 0;
	graph->n_control = 0;

	first = spa_list_first(&graph->node_list, struct node, link);
	last = spa_list_last(&graph->node_list, struct node, link);

	if (inputs != NULL) {
		n_input = count_array(inputs);
	} else {
		n_input = first->desc->n_input;
	}
	if (outputs != NULL) {
		n_output = count_array(outputs);
	} else {
		n_output = last->desc->n_output;
	}

	if (impl->capture_info.channels == 0)
		impl->capture_info.channels = n_input;
	if (impl->playback_info.channels == 0)
		impl->playback_info.channels = n_output;

	n_hndl = impl->capture_info.channels / n_input;
	if (n_hndl != impl->playback_info.channels / n_output) {
		pw_log_error("invalid channels");
		res = -EINVAL;
		goto error;
	}
	pw_log_info("using %d instances %d %d", n_hndl, n_input, n_output);

	spa_list_for_each(node, &graph->node_list, link) {
		desc = node->desc;
		d = desc->desc;
		for (i = 0; i < n_hndl; i++) {
			if ((node->hndl[i] = d->instantiate(d, impl->rate)) == NULL) {
				pw_log_error("cannot create plugin instance");
				res = -ENOMEM;
				goto error;
			}
			node->n_hndl = i;

			graph->hndl[graph->n_hndl] = node->hndl[i];
			graph->desc[graph->n_hndl] = d;
			graph->n_hndl++;

			for (j = 0; j < desc->n_control; j++) {
				p = desc->control[j];
				d->connect_port(node->hndl[i], p, &node->control_data[j]);
			}
			for (j = 0; j < desc->n_notify; j++) {
				p = desc->notify[j];
				d->connect_port(node->hndl[i], p, &node->notify_data[j]);
			}
			if (d->activate)
				d->activate(node->hndl[i]);
		}
		for (j = 0; j < desc->n_control; j++) {
			graph->control_node[graph->n_control] = node;
			graph->control_index[graph->n_control] = j;
			graph->n_control++;
		}
	}
	for (i = 0; i < n_hndl; i++) {
		if (inputs == NULL) {
			desc = first->desc;
			d = desc->desc;
			for (j = 0; j < desc->n_input; j++) {
				p = desc->input[j];
				graph->in_desc[graph->n_input] = d;
				graph->in_hndl[graph->n_input] = first->hndl[i];
				graph->in_port[graph->n_input] = p;
				graph->n_input++;
			}
		} else {
			struct spa_json it = *inputs;
			while (spa_json_get_string(&it, v, sizeof(v)) > 0) {
				col = strchr(v, ':');
				if (col != NULL) {
					node_name = v;
					port_name = col + 1;
					*col = '\0';
					node = find_node(graph, node_name);
				} else {
					node = first;
					node_name = first->name;
					port_name = v;
				}
				if (node == NULL) {
					pw_log_error("input node %s not found", node_name);
					res = -EINVAL;
					goto error;
				}
				p = find_port(node, port_name, LADSPA_PORT_INPUT);
				if (p == SPA_ID_INVALID) {
					pw_log_error("input port %s:%s not found", node_name, port_name);
					res = -EINVAL;
					goto error;
				}
				desc = node->desc;
				d = desc->desc;

				graph->in_desc[graph->n_input] = d;
				graph->in_hndl[graph->n_input] = node->hndl[i];
				graph->in_port[graph->n_input] = p;
				graph->n_input++;
			}
		}
		if (outputs == NULL) {
			desc = first->desc;
			d = desc->desc;
			for (j = 0; j < desc->n_output; j++) {
				p = desc->output[j];
				graph->out_desc[graph->n_output] = d;
				graph->out_hndl[graph->n_output] = last->hndl[i];
				graph->out_port[graph->n_output] = p;
				graph->n_output++;
			}
		} else {
			struct spa_json it = *outputs;
			while (spa_json_get_string(&it, v, sizeof(v)) > 0) {
				col = strchr(v, ':');
				if (col != NULL) {
					node_name = v;
					port_name = col + 1;
					*col = '\0';
					node = find_node(graph, node_name);
				} else {
					node = first;
					node_name = first->name;
					port_name = v;
				}
				if (node == NULL) {
					pw_log_error("output node %s not found", node_name);
					res = -EINVAL;
					goto error;
				}
				p = find_port(node, port_name, LADSPA_PORT_OUTPUT);
				if (p == SPA_ID_INVALID) {
					pw_log_error("output port %s:%s not found", node_name, port_name);
					res = -EINVAL;
					goto error;
				}
				desc = node->desc;
				d = desc->desc;

				graph->out_desc[graph->n_output] = d;
				graph->out_hndl[graph->n_output] = node->hndl[i];
				graph->out_port[graph->n_output] = p;
				graph->n_output++;
			}
		}
	}

	return 0;

error:
	spa_list_for_each(node, &graph->node_list, link) {
		for (i = 0; i < n_hndl; i++) {
			if (node->hndl[i] != NULL)
				node->desc->desc->cleanup(node->hndl[i]);
			node->hndl[i] = NULL;
		}
	}
	return res;
}

/**
 * filter.graph = {
 *     nodes = [
 *         { ... } ...
 *     ]
 *     links = [
 *         { ... } ...
 *     ]
 *     inputs = [ ]
 *     outputs = [ ]
 * }
 */
static int load_graph(struct graph *graph, struct pw_properties *props)
{
	struct spa_json it[4];
	struct spa_json inputs, outputs, *pinputs = NULL, *poutputs = NULL;
	const char *json, *val;
	char key[256];
	int res;

	spa_list_init(&graph->node_list);
	spa_list_init(&graph->link_list);

	if ((json = pw_properties_get(props, "filter.graph")) == NULL) {
		pw_log_error("missing filter.graph property");
		return -EINVAL;
	}

	spa_json_init(&it[0], json, strlen(json));
        if (spa_json_enter_object(&it[0], &it[1]) <= 0)
		spa_json_init(&it[1], json, strlen(json));

	while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
		if (strcmp("nodes", key) == 0) {
			if (spa_json_enter_array(&it[1], &it[2]) <= 0) {
				pw_log_error("nodes expect and array");
				return -EINVAL;
			}

			while (spa_json_enter_object(&it[2], &it[3]) > 0) {
				if ((res = load_node(graph, &it[3])) < 0)
					return res;
			}
		}
		else if (strcmp("links", key) == 0) {
			if (spa_json_enter_array(&it[1], &it[2]) <= 0)
				return -EINVAL;

			while (spa_json_enter_object(&it[2], &it[3]) > 0) {
				return -ENOTSUP;
			}
		}
		else if (strcmp("inputs", key) == 0) {
			if (spa_json_enter_array(&it[1], &inputs) <= 0)
				return -EINVAL;
			pinputs = &inputs;
		}
		else if (strcmp("outputs", key) == 0) {
			if (spa_json_enter_array(&it[1], &outputs) <= 0)
				return -EINVAL;
			poutputs = &outputs;
		} else if (spa_json_next(&it[1], &val) < 0)
			break;
	}
	return setup_graph(graph, pinputs, poutputs);
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
	impl->graph.impl = impl;
	spa_list_init(&impl->ladspa_handle_list);


	if (pw_properties_get(props, PW_KEY_NODE_GROUP) == NULL)
		pw_properties_setf(props, PW_KEY_NODE_GROUP, "filter-chain-%u", id);
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

	if ((res = load_graph(&impl->graph, props)) < 0) {
		pw_log_error("can't load graph: %s", spa_strerror(res));
		goto error;
	}

	if (pw_properties_get(impl->capture_props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(impl->capture_props, PW_KEY_MEDIA_NAME, "filter input %u",
				id);
	if (pw_properties_get(impl->playback_props, PW_KEY_MEDIA_NAME) == NULL)
		pw_properties_setf(impl->playback_props, PW_KEY_MEDIA_NAME, "filter output %u",
				id);

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
