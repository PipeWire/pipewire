/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-FileCopyrightText: Copyright © 2021 Sanchayan Maity <sanchayan@asymptotic.io> */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/hook.h>
#include <pipewire/pipewire.h>

#include "../defs.h"
#include "../module.h"

/** \page page_pulse_module_roc_source ROC Source
 *
 * ## Module Name
 *
 * `module-roc-source`
 *
 * ## Module Options
 *
 * @pulse_module_options@
 *
 * ## See Also
 *
 * \ref page_module_roc_source "libpipewire-module-roc-source"
 */

static const char *const pulse_module_options =
	"source_name=<name for the source> "
	"source_properties=<properties for the source> "
	"resampler_profile=<empty>|high|medium|low "
	"fec_code=<empty>|disable|rs8m|ldpc "
	"sess_latency_msec=<target network latency in milliseconds> "
	"local_ip=<local receiver ip> "
	"local_source_port=<local receiver port for source packets> "
	"local_repair_port=<local receiver port for repair packets> "
	"local_control_port=<local receiver port for control packets> "
	;

#define NAME "roc-source"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_roc_source_data {
	struct module *module;

	struct spa_hook mod_listener;
	struct pw_impl_module *mod;

	struct pw_properties *source_props;
	struct pw_properties *roc_props;
};

static void module_destroy(void *data)
{
	struct module_roc_source_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_roc_source_load(struct module *module)
{
	struct module_roc_source_data *data = module->user_data;
	FILE *f;
	char *args;
	size_t size;

	pw_properties_setf(data->source_props, "pulse.module.id",
			"%u", module->index);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &data->roc_props->dict, 0);
	fprintf(f, " source.props = {");
	pw_properties_serialize_dict(f, &data->source_props->dict, 0);
	fprintf(f, " } }");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-roc-source",
			args, NULL);

	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);

	return 0;
}

static int module_roc_source_unload(struct module *module)
{
	struct module_roc_source_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}

	pw_properties_free(d->roc_props);
	pw_properties_free(d->source_props);

	return 0;
}

static const char* const valid_args[] = {
	"source_name",
	"source_properties",
	"resampler_profile",
	"fec_code",
	"sess_latency_msec",
	"local_ip",
	"local_source_port",
	"local_repair_port",
	"local_control_port",
	NULL
};

static const struct spa_dict_item module_roc_source_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "roc source" },
	{ PW_KEY_MODULE_USAGE, pulse_module_options },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_roc_source_prepare(struct module * const module)
{
	struct module_roc_source_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *source_props = NULL, *roc_props = NULL;
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	source_props = pw_properties_new(NULL, NULL);
	roc_props = pw_properties_new(NULL, NULL);
	if (!source_props || !roc_props) {
		res = -errno;
		goto out;
	}

	if ((str = pw_properties_get(props, "source_name")) != NULL) {
		pw_properties_set(source_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "source_name", NULL);
	}
	if ((str = pw_properties_get(props, "source_properties")) != NULL) {
		module_args_add_props(source_props, str);
		pw_properties_set(props, "source_properties", NULL);
	}

	if ((str = pw_properties_get(props, PW_KEY_MEDIA_CLASS)) == NULL) {
		pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Source");
		pw_properties_set(source_props, PW_KEY_MEDIA_CLASS, "Audio/Source");
	}

	if ((str = pw_properties_get(props, "local_ip")) != NULL) {
		pw_properties_set(roc_props, "local.ip", str);
		pw_properties_set(props, "local_ip", NULL);
	}

	if ((str = pw_properties_get(props, "local_source_port")) != NULL) {
		pw_properties_set(roc_props, "local.source.port", str);
		pw_properties_set(props, "local_source_port", NULL);
	}

	if ((str = pw_properties_get(props, "local_repair_port")) != NULL) {
		pw_properties_set(roc_props, "local.repair.port", str);
		pw_properties_set(props, "local_repair_port", NULL);
	}

	if ((str = pw_properties_get(props, "local_control_port")) != NULL) {
		pw_properties_set(roc_props, "local.control.port", str);
		pw_properties_set(props, "local_control_port", NULL);
	}

	if ((str = pw_properties_get(props, "sess_latency_msec")) != NULL) {
		pw_properties_set(roc_props, "sess.latency.msec", str);
		pw_properties_set(props, "sess_latency_msec", NULL);
	}

	if ((str = pw_properties_get(props, "resampler_profile")) != NULL) {
		pw_properties_set(roc_props, "resampler.profile", str);
		pw_properties_set(props, "resampler_profile", NULL);
	}

	if ((str = pw_properties_get(props, "fec_code")) != NULL) {
		pw_properties_set(roc_props, "fec.code", str);
		pw_properties_set(props, "fec_code", NULL);
	}

	d->module = module;
	d->source_props = source_props;
	d->roc_props = roc_props;

	return 0;
out:
	pw_properties_free(source_props);
	pw_properties_free(roc_props);

	return res;
}

DEFINE_MODULE_INFO(module_roc_source) = {
	.name = "module-roc-source",
	.valid_args = valid_args,
	.prepare = module_roc_source_prepare,
	.load = module_roc_source_load,
	.unload = module_roc_source_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_roc_source_info),
	.data_size = sizeof(struct module_roc_source_data),
};
