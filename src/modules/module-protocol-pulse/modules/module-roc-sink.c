/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-FileCopyrightText: Copyright © 2021 Sanchayan Maity <sanchayan@asymptotic.io> */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/hook.h>
#include <pipewire/pipewire.h>

#include "../defs.h"
#include "../module.h"

#define NAME "roc-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_roc_sink_data {
	struct module *module;

	struct spa_hook mod_listener;
	struct pw_impl_module *mod;

	struct pw_properties *sink_props;
	struct pw_properties *roc_props;
};

static void module_destroy(void *data)
{
	struct module_roc_sink_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_roc_sink_load(struct module *module)
{
	struct module_roc_sink_data *data = module->user_data;
	FILE *f;
	char *args;
	size_t size;

	pw_properties_setf(data->sink_props, "pulse.module.id",
			"%u", module->index);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &data->roc_props->dict, 0);
	fprintf(f, " sink.props = {");
	pw_properties_serialize_dict(f, &data->sink_props->dict, 0);
	fprintf(f, " } }");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-roc-sink",
			args, NULL);

	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);

	return 0;
}

static int module_roc_sink_unload(struct module *module)
{
	struct module_roc_sink_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}

	pw_properties_free(d->roc_props);
	pw_properties_free(d->sink_props);

	return 0;
}

static const struct spa_dict_item module_roc_sink_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "roc sink" },
	{ PW_KEY_MODULE_USAGE, "sink_name=<name for the sink> "
				"sink_properties=<properties for the sink> "
				"fec_code=<empty>|disable|rs8m|ldpc "
				"remote_ip=<remote receiver ip> "
				"remote_source_port=<remote receiver port for source packets> "
				"remote_repair_port=<remote receiver port for repair packets> " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_roc_sink_prepare(struct module * const module)
{
	struct module_roc_sink_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *sink_props = NULL, *roc_props = NULL;
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	sink_props = pw_properties_new(NULL, NULL);
	roc_props = pw_properties_new(NULL, NULL);
	if (!sink_props || !roc_props) {
		res = -errno;
		goto out;
	}

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_properties_set(sink_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "sink_name", NULL);
	}
	if ((str = pw_properties_get(props, "sink_properties")) != NULL) {
		module_args_add_props(sink_props, str);
		pw_properties_set(props, "sink_properties", NULL);
	}

	if ((str = pw_properties_get(props, PW_KEY_MEDIA_CLASS)) == NULL) {
		pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
		pw_properties_set(sink_props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
	}

	if ((str = pw_properties_get(props, "remote_ip")) != NULL) {
		pw_properties_set(roc_props, "remote.ip", str);
		pw_properties_set(props, "remote_ip", NULL);
	} else {
		pw_log_error("Remote IP not specified");
		res = -EINVAL;
		goto out;
	}

	if ((str = pw_properties_get(props, "remote_source_port")) != NULL) {
		pw_properties_set(roc_props, "remote.source.port", str);
		pw_properties_set(props, "remote_source_port", NULL);
	}

	if ((str = pw_properties_get(props, "remote_repair_port")) != NULL) {
		pw_properties_set(roc_props, "remote.repair.port", str);
		pw_properties_set(props, "remote_repair_port", NULL);
	}
	if ((str = pw_properties_get(props, "fec_code")) != NULL) {
		pw_properties_set(roc_props, "fec.code", str);
		pw_properties_set(props, "fec_code", NULL);
	}

	d->module = module;
	d->sink_props = sink_props;
	d->roc_props = roc_props;

	return 0;
out:
	pw_properties_free(sink_props);
	pw_properties_free(roc_props);

	return res;
}

DEFINE_MODULE_INFO(module_roc_sink) = {
	.name = "module-roc-sink",
	.prepare = module_roc_sink_prepare,
	.load = module_roc_sink_load,
	.unload = module_roc_sink_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_roc_sink_info),
	.data_size = sizeof(struct module_roc_sink_data),
};
