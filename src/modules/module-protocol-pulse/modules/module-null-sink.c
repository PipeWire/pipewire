/* PipeWire
 *
 * Copyright © 2021 Georges Basile Stavracas Neto
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

#include <pipewire/pipewire.h>

#include "../manager.h"
#include "../module.h"
#include "registry.h"

#define NAME "null-sink"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_null_sink_data {
	struct pw_core *core;
	struct spa_hook core_listener;

	struct pw_proxy *proxy;
	struct spa_hook proxy_listener;
};

static void module_null_sink_proxy_removed(void *data)
{
	struct module *module = data;
	struct module_null_sink_data *d = module->user_data;
	pw_proxy_destroy(d->proxy);
}

static void module_null_sink_proxy_destroy(void *data)
{
	struct module *module = data;
	struct module_null_sink_data *d = module->user_data;

	pw_log_info("proxy %p destroy", d->proxy);

	spa_hook_remove(&d->proxy_listener);
	d->proxy = NULL;

	module_schedule_unload(module);
}

static void module_null_sink_proxy_bound(void *data, uint32_t global_id)
{
	struct module *module = data;
	struct module_null_sink_data *d = module->user_data;

	pw_log_info("proxy %p bound", d->proxy);

	module_emit_loaded(module, 0);
}

static void module_null_sink_proxy_error(void *data, int seq, int res, const char *message)
{
	struct module *module = data;
	struct module_null_sink_data *d = module->user_data;

	pw_log_info("proxy %p error %d", d->proxy, res);

	pw_proxy_destroy(d->proxy);
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = module_null_sink_proxy_removed,
	.bound = module_null_sink_proxy_bound,
	.error = module_null_sink_proxy_error,
	.destroy = module_null_sink_proxy_destroy,
};

static void module_null_sink_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	struct module *module = data;

	pw_log_warn("error id:%u seq:%d res:%d (%s): %s",
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE && res == -EPIPE)
		module_schedule_unload(module);
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.error = module_null_sink_core_error,
};

static int module_null_sink_load(struct client *client, struct module *module)
{
	struct module_null_sink_data *d = module->user_data;

	d->core = pw_context_connect(module->impl->context, pw_properties_copy(client->props), 0);
	if (d->core == NULL)
		return -errno;

	pw_core_add_listener(d->core, &d->core_listener, &core_events, module);

	pw_properties_setf(module->props, "pulse.module.id", "%u", module->index);

	d->proxy = pw_core_create_object(d->core,
					 "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE,
					 module->props ? &module->props->dict : NULL, 0);
	if (d->proxy == NULL)
		return -errno;

	pw_proxy_add_listener(d->proxy, &d->proxy_listener, &proxy_events, module);

	return SPA_RESULT_RETURN_ASYNC(0);
}

static int module_null_sink_unload(struct module *module)
{
	struct module_null_sink_data *d = module->user_data;

	if (d->proxy != NULL) {
		spa_hook_remove(&d->proxy_listener);
		pw_proxy_destroy(d->proxy);
		d->proxy = NULL;
	}

	if (d->core != NULL) {
		spa_hook_remove(&d->core_listener);
		pw_core_disconnect(d->core);
		d->core = NULL;
	}

	return 0;
}

static const struct module_methods module_null_sink_methods = {
	VERSION_MODULE_METHODS,
	.load = module_null_sink_load,
	.unload = module_null_sink_unload,
};

static const struct spa_dict_item module_null_sink_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "A NULL sink" },
	{ PW_KEY_MODULE_USAGE,  "sink_name=<name of sink> "
				"sink_properties=<properties for the sink> "
				"format=<sample format> "
				"rate=<sample rate> "
				"channels=<number of channels> "
				"channel_map=<channel map>" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct module *create_module_null_sink(struct impl *impl, const char *argument)
{
	struct module *module;
	struct pw_properties *props = NULL;
	const char *str;
	struct spa_audio_info_raw info = { 0 };
	uint32_t i;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	props = pw_properties_new_dict(&SPA_DICT_INIT_ARRAY(module_null_sink_info));
	if (props == NULL) {
		res = -EINVAL;
		goto out;
	}
	if (argument)
		module_args_add_props(props, argument);

	if ((str = pw_properties_get(props, "sink_name")) != NULL) {
		pw_properties_set(props, PW_KEY_NODE_NAME, str);
		pw_properties_set(props, "sink_name", NULL);
	}
	else {
		pw_properties_set(props, PW_KEY_NODE_NAME, "null-sink");
	}

	if ((str = pw_properties_get(props, "sink_properties")) != NULL) {
		module_args_add_props(props, str);
		pw_properties_set(props, "sink_properties", NULL);
	}

	if (module_args_to_audioinfo(impl, props, &info) < 0) {
		res = -EINVAL;
		goto out;
	}

	if (info.rate)
		pw_properties_setf(props, SPA_KEY_AUDIO_RATE, "%u", info.rate);
	if (info.channels) {
		char *s, *p;

		pw_properties_setf(props, SPA_KEY_AUDIO_CHANNELS, "%u", info.channels);

		p = s = alloca(info.channels * 8);
		for (i = 0; i < info.channels; i++)
			p += spa_scnprintf(p, 8, "%s%s", i == 0 ? "" : ",",
					channel_id2name(info.position[i]));
		pw_properties_set(props, SPA_KEY_AUDIO_POSITION, s);
	}

	if (pw_properties_get(props, PW_KEY_MEDIA_CLASS) == NULL)
		pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");

	if ((str = pw_properties_get(props, PW_KEY_NODE_DESCRIPTION)) == NULL) {
		const char *name, *class;

		name = pw_properties_get(props, PW_KEY_NODE_NAME);
		class = pw_properties_get(props, PW_KEY_MEDIA_CLASS);
		pw_properties_setf(props, PW_KEY_NODE_DESCRIPTION,
						"%s%s%s%ssink",
						name, (name[0] == '\0') ? "" : " ",
						class ? class : "", (class && class[0] != '\0') ? " " : "");
	}
	pw_properties_set(props, PW_KEY_FACTORY_NAME, "support.null-audio-sink");

	if (pw_properties_get(props, "monitor.channel-volumes") == NULL)
		pw_properties_set(props, "monitor.channel-volumes", "true");

	module = module_new(impl, &module_null_sink_methods, sizeof(struct module_null_sink_data));
	if (module == NULL) {
		res = -errno;
		goto out;
	}
	module->props = props;

	return module;
out:
	pw_properties_free(props);
	errno = -res;
	return NULL;
}
