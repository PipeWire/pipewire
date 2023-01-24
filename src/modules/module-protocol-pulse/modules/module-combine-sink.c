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
#include <spa/utils/json.h>

#include <pipewire/pipewire.h>
#include <pipewire/utils.h>

#include "../manager.h"
#include "../module.h"

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
				"channel_map=<channel map> "
				"remix=<remix channels> " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct module_combine_sink_data;

struct module_combine_sink_data {
	struct module *module;

	struct pw_core *core;
	struct spa_hook core_listener;
	struct pw_manager *manager;
	struct spa_hook manager_listener;

	struct pw_impl_module *mod;
	struct spa_hook mod_listener;

	char *sink_name;
	char **sink_names;
	struct pw_properties *combine_props;

	struct spa_source *sinks_timeout;

	struct spa_audio_info_raw info;

	unsigned int sinks_pending;
	unsigned int remix:1;
	unsigned int load_emitted:1;
	unsigned int start_error:1;
};

static void check_initialized(struct module_combine_sink_data *data)
{
	struct module *module = data->module;

	if (data->load_emitted)
		return;

	if (data->start_error) {
		pw_log_debug("module load error");
		data->load_emitted = true;
		module_emit_loaded(module, -EIO);
	} else if (data->sinks_pending == 0) {
		pw_log_debug("module loaded");
		data->load_emitted = true;
		module_emit_loaded(module, 0);
	}
}

static void manager_added(void *d, struct pw_manager_object *o)
{
	struct module_combine_sink_data *data = d;
	const char *str;
	uint32_t val = 0;
	struct pw_node_info *info;

	if (!spa_streq(o->type, PW_TYPE_INTERFACE_Node) ||
	    (info = o->info) == NULL || info->props == NULL)
		return;

	str = spa_dict_lookup(info->props, "pulse.module.id");
	if (str == NULL || !spa_atou32(str, &val, 0) || val != data->module->index)
		return;

	pw_log_info("found our %s, pending:%d",
			pw_properties_get(o->props, PW_KEY_NODE_NAME),
			data->sinks_pending);

	if (!pw_manager_object_is_sink(o)) {
		if (data->sinks_pending > 0)
			data->sinks_pending--;
	}
	check_initialized(data);
	return;
}

static const struct pw_manager_events manager_events = {
	PW_VERSION_MANAGER_EVENTS,
	.added = manager_added,
};

static void on_sinks_timeout(void *d, uint64_t count)
{
	struct module_combine_sink_data *data = d;

	if (data->load_emitted)
		return;

	data->start_error = true;
	check_initialized(data);
}

static void module_destroy(void *data)
{
	struct module_combine_sink_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_combine_sink_load(struct module *module)
{
	struct module_combine_sink_data *data = module->user_data;
	uint32_t i;
	FILE *f;
	char *args;
	size_t size;

	data->core = pw_context_connect(module->impl->context, NULL, 0);
	if (data->core == NULL)
		return -errno;

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	fprintf(f, " node.name = %s", data->sink_name);
	fprintf(f, " node.description = %s", data->sink_name);
	if (data->info.rate != 0)
		fprintf(f, " audio.rate = %u", data->info.rate);
	if (data->info.channels != 0) {
		fprintf(f, " audio.channels = %u", data->info.channels);
		if (!(data->info.flags & SPA_AUDIO_FLAG_UNPOSITIONED)) {
			fprintf(f, " audio.position = [ ");
			for (i = 0; i < data->info.channels; i++)
				fprintf(f, "%s%s", i == 0 ? "" : ",",
					channel_id2name(data->info.position[i]));
			fprintf(f, " ]");
		}
	}
	fprintf(f, " combine.props = {");
	fprintf(f, " pulse.module.id = %u", module->index);
	pw_properties_serialize_dict(f, &data->combine_props->dict, 0);
	fprintf(f, " } stream.props = {");
	if (!data->remix)
		fprintf(f, "   "PW_KEY_STREAM_DONT_REMIX" = true");
	fprintf(f, "   pulse.module.id = %u", module->index);
	fprintf(f, " } stream.rules = [");
	if (data->sink_names == NULL) {
		fprintf(f, "  { matches = [ { media.class = \"Audio/Sink\" } ]");
		fprintf(f, "    actions = { create-stream = { } } }");
	} else {
		for (i = 0; data->sink_names[i] != NULL; i++) {
			char name[1024];
			spa_json_encode_string(name, sizeof(name)-1, data->sink_names[i]);
			fprintf(f, "  { matches = [ { media.class = \"Audio/Sink\" ");
			fprintf(f, " node.name = %s } ]", name);
			fprintf(f, "    actions = { create-stream = { } } }");
		}
	}
	fprintf(f, " ]");
	fprintf(f, "}");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-combine-stream",
			args, NULL);
	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);

	data->manager = pw_manager_new(data->core);
	if (data->manager == NULL)
		return -errno;

	pw_manager_add_listener(data->manager, &data->manager_listener,
			&manager_events, data);

	data->sinks_timeout = pw_loop_add_timer(module->impl->loop, on_sinks_timeout, data);
	if (data->sinks_timeout) {
		struct timespec timeout = {0};
		timeout.tv_sec = TIMEOUT_SINKS_MSEC / 1000;
		timeout.tv_nsec = (TIMEOUT_SINKS_MSEC % 1000) * SPA_NSEC_PER_MSEC;
		pw_loop_update_timer(module->impl->loop, data->sinks_timeout, &timeout, NULL, false);
	}
	return data->load_emitted ? 0 : SPA_RESULT_RETURN_ASYNC(0);
}

static int module_combine_sink_unload(struct module *module)
{
	struct module_combine_sink_data *d = module->user_data;

	if (d->sinks_timeout != NULL)
		pw_loop_destroy_source(module->impl->loop, d->sinks_timeout);

	if (d->mod != NULL) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
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
	pw_properties_free(d->combine_props);
	return 0;
}

static int module_combine_sink_prepare(struct module * const module)
{
	struct module_combine_sink_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *combine_props = NULL;
	const char *str;
	char *sink_name = NULL, **sink_names = NULL;
	struct spa_audio_info_raw info = { 0 };
	int res;
	int num_sinks = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	combine_props = pw_properties_new(NULL, NULL);

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		sink_name = strdup(str);
		pw_properties_set(props, "sink_name", NULL);
	} else {
		sink_name = strdup("combined");
	}

	if ((str = pw_properties_get(module->props, "sink_properties")) != NULL)
		module_args_add_props(combine_props, str);

	if ((str = pw_properties_get(props, "slaves")) != NULL) {
		sink_names = pw_split_strv(str, ",", MAX_SINKS, &num_sinks);
		pw_properties_set(props, "slaves", NULL);
	}
	d->remix = true;
	if ((str = pw_properties_get(props, "remix")) != NULL) {
		d->remix = pw_properties_parse_bool(str);
		pw_properties_set(props, "remix", NULL);
	}

	if ((str = pw_properties_get(props, "adjust_time")) != NULL) {
		pw_log_info("The `adjust_time` modarg is ignored");
		pw_properties_set(props, "adjust_time", NULL);
	}

	if ((str = pw_properties_get(props, "resample_method")) != NULL) {
		pw_log_info("The `resample_method` modarg is ignored");
		pw_properties_set(props, "resample_method", NULL);
	}

	if (module_args_to_audioinfo(module->impl, props, &info) < 0) {
		res = -EINVAL;
		goto out;
	}

	d->module = module;
	d->info = info;
	d->sink_name = sink_name;
	d->sink_names = sink_names;
	d->sinks_pending = (sink_names == NULL) ? 0 : num_sinks;
	d->combine_props = combine_props;

	return 0;
out:
	free(sink_name);
	pw_free_strv(sink_names);
	pw_properties_free(combine_props);

	return res;
}

DEFINE_MODULE_INFO(module_combine_sink) = {
	.name = "module-combine-sink",
	.prepare = module_combine_sink_prepare,
	.load = module_combine_sink_load,
	.unload = module_combine_sink_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_combine_sink_info),
	.data_size = sizeof(struct module_combine_sink_data),
};
