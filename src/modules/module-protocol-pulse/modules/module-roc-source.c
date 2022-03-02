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

#include <spa/utils/hook.h>
#include <pipewire/pipewire.h>
#include <pipewire/private.h>

#include "../defs.h"
#include "../module.h"
#include "registry.h"

#define NAME "roc-source"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

#define ROC_DEFAULT_IP "0.0.0.0"
#define ROC_DEFAULT_SOURCE_PORT "10001"
#define ROC_DEFAULT_REPAIR_PORT "10002"

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

static int module_roc_source_load(struct client *client, struct module *module)
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
	fprintf(f, " } source.props = {");
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

static const struct module_methods module_roc_source_methods = {
	VERSION_MODULE_METHODS,
	.load = module_roc_source_load,
	.unload = module_roc_source_unload,
};

static const struct spa_dict_item module_roc_source_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Sanchayan Maity <sanchayan@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "roc source" },
	{ PW_KEY_MODULE_USAGE, "source_name=<name for the source> "
				"resampler_profile=<empty>|disable|high|medium|low "
				"sess_latency_msec=<target network latency in milliseconds> "
				"local_ip=<local receiver ip> "
				"local_source_port=<local receiver port for source packets> "
				"local_repair_port=<local receiver port for repair packets> " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct module *create_module_roc_source(struct impl *impl, const char *argument)
{
	struct module *module;
	struct module_roc_source_data *d;
	struct pw_properties *props = NULL, *source_props = NULL, *roc_props = NULL;
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	props = pw_properties_new_dict(&SPA_DICT_INIT_ARRAY(module_roc_source_info));
	source_props = pw_properties_new(NULL, NULL);
	roc_props = pw_properties_new(NULL, NULL);
	if (!props || !source_props || !roc_props) {
		res = -errno;
		goto out;
	}

	if (argument != NULL)
		module_args_add_props(props, argument);

	if ((str = pw_properties_get(props, "source_name")) != NULL) {
		pw_properties_set(source_props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "source_name", NULL);
	}

	if ((str = pw_properties_get(props, PW_KEY_MEDIA_CLASS)) == NULL) {
		pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Source");
		pw_properties_set(source_props, PW_KEY_MEDIA_CLASS, "Audio/Source");
	}

	if ((str = pw_properties_get(props, "local_ip")) != NULL) {
		pw_properties_set(roc_props, "local.ip", str);
		pw_properties_set(props, "local_ip", NULL);
	} else {
		pw_properties_set(roc_props, "local.ip", ROC_DEFAULT_IP);
	}

	if ((str = pw_properties_get(props, "local_source_port")) != NULL) {
		pw_properties_set(roc_props, "local.source.port", str);
		pw_properties_set(props, "local_source_port", NULL);
	} else {
		pw_properties_set(roc_props, "local.source.port", ROC_DEFAULT_SOURCE_PORT);
	}

	if ((str = pw_properties_get(props, "local_repair_port")) != NULL) {
		pw_properties_set(roc_props, "local.repair.port", str);
		pw_properties_set(props, "local_repair_port", NULL);
	} else {
		pw_properties_set(roc_props, "local.repair.port", ROC_DEFAULT_REPAIR_PORT);
	}

	if ((str = pw_properties_get(props, "sess_latency_msec")) != NULL) {
		pw_properties_set(roc_props, "sess.latency.msec", str);
		pw_properties_set(props, "sess_latency_msec", NULL);
	} else {
		pw_properties_set(roc_props, "sess.latency.msec", ROC_DEFAULT_REPAIR_PORT);
	}

	if ((str = pw_properties_get(props, "resampler_profile")) != NULL) {
		pw_properties_set(roc_props, "resampler.profile", str);
		pw_properties_set(props, "resampler_profile", NULL);
	} else {
		pw_properties_set(roc_props, "resampler.profile", ROC_DEFAULT_REPAIR_PORT);
	}

	module = module_new(impl, &module_roc_source_methods, sizeof(*d));
	if (module == NULL) {
		res = -errno;
		goto out;
	}

	module->props = props;
	d = module->user_data;
	d->module = module;
	d->source_props = source_props;
	d->roc_props = roc_props;

	return module;
out:
	pw_properties_free(props);
	pw_properties_free(source_props);
	pw_properties_free(roc_props);
	errno = -res;
	return NULL;
}
