/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-FileCopyrightText: Copyright © 2021 Arun Raghavan <arun@asymptotic.io> */
/* SPDX-License-Identifier: MIT */

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
				"remix=<remix channels> "
				"latency_compensate=<bool> " },
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

	char **sink_names;
	struct pw_properties *props;
	struct pw_properties *combine_props;
	struct pw_properties *stream_props;

	struct spa_source *sinks_timeout;

	unsigned int sinks_pending;
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

	pw_properties_setf(data->combine_props, "pulse.module.id", "%u",
			module->index);
	pw_properties_setf(data->stream_props, "pulse.module.id", "%u",
			module->index);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &data->props->dict, 0);
	fprintf(f, " combine.props = {");
	pw_properties_serialize_dict(f, &data->combine_props->dict, 0);
	fprintf(f, " } stream.props = {");
	pw_properties_serialize_dict(f, &data->stream_props->dict, 0);
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
	pw_properties_free(d->stream_props);
	pw_properties_free(d->combine_props);
	pw_properties_free(d->props);
	return 0;
}

static int module_combine_sink_prepare(struct module * const module)
{
	struct module_combine_sink_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *combine_props = NULL, *global_props = NULL, *stream_props = NULL;
	const char *str;
	char **sink_names = NULL;
	struct spa_audio_info_raw info = { 0 };
	int res;
	int num_sinks = 0;

	PW_LOG_TOPIC_INIT(mod_topic);

	global_props = pw_properties_new(NULL, NULL);
	combine_props = pw_properties_new(NULL, NULL);
	stream_props = pw_properties_new(NULL, NULL);
	if (global_props == NULL || combine_props == NULL || stream_props == NULL) {
		res = -ENOMEM;
		goto out;
	}

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_properties_set(global_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(global_props, PW_KEY_NODE_DESCRIPTION, str);
		pw_properties_set(props, "sink_name", NULL);
	} else {
		str = "combined";
		pw_properties_set(global_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(global_props, PW_KEY_NODE_DESCRIPTION, str);
	}

	if ((str = pw_properties_get(props, "sink_properties")) != NULL)
		module_args_add_props(combine_props, str);

	if ((str = pw_properties_get(props, "slaves")) != NULL) {
		sink_names = pw_split_strv(str, ",", MAX_SINKS, &num_sinks);
		pw_properties_set(props, "slaves", NULL);
	}
	if ((str = pw_properties_get(props, "remix")) != NULL) {
		pw_properties_set(stream_props, PW_KEY_STREAM_DONT_REMIX,
				module_args_parse_bool(str) ? "false" : "true");
		pw_properties_set(props, "remix", NULL);
	}

	if ((str = pw_properties_get(props, "latency_compensate")) != NULL) {
		pw_properties_set(global_props, "combine.latency-compensate",
				module_args_parse_bool(str) ? "true" : "false");
		pw_properties_set(props, "latency_compensate", NULL);
	}

	if ((str = pw_properties_get(props, "adjust_time")) != NULL) {
		pw_log_info("The `adjust_time` modarg is ignored");
		pw_properties_set(props, "adjust_time", NULL);
	}

	if ((str = pw_properties_get(props, "resample_method")) != NULL) {
		pw_log_info("The `resample_method` modarg is ignored");
		pw_properties_set(props, "resample_method", NULL);
	}

	if (module_args_to_audioinfo_keys(module->impl, props,
			NULL, "rate", "channels", "channel_map", &info) < 0) {
		res = -EINVAL;
		goto out;
	}
	audioinfo_to_properties(&info, global_props);

	d->module = module;
	d->sink_names = sink_names;
	d->sinks_pending = (sink_names == NULL) ? 0 : num_sinks;
	d->stream_props = stream_props;
	d->combine_props = combine_props;
	d->props = global_props;

	return 0;
out:
	pw_free_strv(sink_names);
	pw_properties_free(stream_props);
	pw_properties_free(combine_props);
	pw_properties_free(global_props);

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
