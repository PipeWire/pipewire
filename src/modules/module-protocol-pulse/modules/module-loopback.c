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
#include <spa/utils/hook.h>
#include <spa/utils/json.h>
#include <pipewire/pipewire.h>
#include <pipewire/private.h>

#include "../defs.h"
#include "../module.h"
#include "registry.h"

#define NAME "loopback"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_loopback_data {
	struct module *module;

	struct pw_impl_module *mod;
	struct spa_hook mod_listener;

	struct pw_properties *capture_props;
	struct pw_properties *playback_props;

	struct spa_audio_info_raw info;
};

static void module_destroy(void *data)
{
	struct module_loopback_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_loopback_load(struct client *client, struct module *module)
{
	struct module_loopback_data *data = module->user_data;
	FILE *f;
	char *args;
	size_t size, i;

	pw_properties_setf(data->capture_props, PW_KEY_NODE_GROUP, "loopback-%u", module->index);
	pw_properties_setf(data->playback_props, PW_KEY_NODE_GROUP, "loopback-%u", module->index);
	pw_properties_setf(data->capture_props, "pulse.module.id", "%u", module->index);
	pw_properties_setf(data->playback_props, "pulse.module.id", "%u", module->index);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
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
	fprintf(f, " capture.props = {");
	pw_properties_serialize_dict(f, &data->capture_props->dict, 0);
	fprintf(f, " } playback.props = {");
	pw_properties_serialize_dict(f, &data->playback_props->dict, 0);
	fprintf(f, " } }");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-loopback",
			args, NULL);
	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);

	return 0;
}

static int module_loopback_unload(struct module *module)
{
	struct module_loopback_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}

	pw_properties_free(d->capture_props);
	pw_properties_free(d->playback_props);

	return 0;
}

static const struct module_methods module_loopback_methods = {
	VERSION_MODULE_METHODS,
	.load = module_loopback_load,
	.unload = module_loopback_unload,
};

static const struct spa_dict_item module_loopback_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Arun Raghavan <arun@asymptotic.io>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Loopback from source to sink" },
	{ PW_KEY_MODULE_USAGE, "source=<source to connect to> "
				"sink=<sink to connect to> "
				"latency_msec=<latency in ms> "
				"rate=<sample rate> "
				"channels=<number of channels> "
				"channel_map=<channel map> "
				"sink_input_properties=<proplist> "
				"source_output_properties=<proplist> "
				"source_dont_move=<boolean> "
				"sink_dont_move=<boolean> "
				"remix=<remix channels?> " },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

struct module *create_module_loopback(struct impl *impl, const char *argument)
{
	struct module *module;
	struct module_loopback_data *d;
	struct pw_properties *props = NULL, *playback_props = NULL, *capture_props = NULL;
	const char *str;
	struct spa_audio_info_raw info = { 0 };
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	props = pw_properties_new_dict(&SPA_DICT_INIT_ARRAY(module_loopback_info));
	capture_props = pw_properties_new(NULL, NULL);
	playback_props = pw_properties_new(NULL, NULL);
	if (!props || !capture_props || !playback_props) {
		res = -EINVAL;
		goto out;
	}
	if (argument)
		module_args_add_props(props, argument);

	/* The following modargs are not implemented:
	 * adjust_time, max_latency_msec, fast_adjust_threshold_msec: these are just not relevant
	 */

	if ((str = pw_properties_get(props, "source")) != NULL) {
		if (spa_strendswith(str, ".monitor")) {
			pw_properties_setf(capture_props, PW_KEY_NODE_TARGET,
					"%.*s", (int)strlen(str)-8, str);
			pw_properties_set(capture_props, PW_KEY_STREAM_CAPTURE_SINK,
					"true");
		} else {
			pw_properties_set(capture_props, PW_KEY_NODE_TARGET, str);
		}
		pw_properties_set(props, "source", NULL);
	}

	if ((str = pw_properties_get(props, "sink")) != NULL) {
		pw_properties_set(playback_props, PW_KEY_NODE_TARGET, str);
		pw_properties_set(props, "sink", NULL);
	}

	if (module_args_to_audioinfo(impl, props, &info) < 0) {
		res = -EINVAL;
		goto out;
	}

	if ((str = pw_properties_get(props, "source_dont_move")) != NULL) {
		pw_properties_set(capture_props, PW_KEY_NODE_DONT_RECONNECT, str);
		pw_properties_set(props, "source_dont_move", NULL);
	}

	if ((str = pw_properties_get(props, "sink_dont_move")) != NULL) {
		pw_properties_set(playback_props, PW_KEY_NODE_DONT_RECONNECT, str);
		pw_properties_set(props, "sink_dont_move", NULL);
	}

	if ((str = pw_properties_get(props, "remix")) != NULL) {
		/* Note that the boolean is inverted */
		pw_properties_set(playback_props, PW_KEY_STREAM_DONT_REMIX,
				module_args_parse_bool(str) ? "false" : "true");
		pw_properties_set(props, "remix", NULL);
	}

	if ((str = pw_properties_get(props, "latency_msec")) != NULL) {
		/* Half the latency on each of the playback and capture streams */
		pw_properties_setf(capture_props, PW_KEY_NODE_LATENCY, "%s/2000", str);
		pw_properties_setf(playback_props, PW_KEY_NODE_LATENCY, "%s/2000", str);
		pw_properties_set(props, "latency_msec", NULL);
	} else {
		pw_properties_set(capture_props, PW_KEY_NODE_LATENCY, "100/1000");
		pw_properties_set(playback_props, PW_KEY_NODE_LATENCY, "100/1000");
	}

	if ((str = pw_properties_get(props, "sink_input_properties")) != NULL) {
		module_args_add_props(playback_props, str);
		pw_properties_set(props, "sink_input_properties", NULL);
	}

	if ((str = pw_properties_get(props, "source_output_properties")) != NULL) {
		module_args_add_props(capture_props, str);
		pw_properties_set(props, "source_output_properties", NULL);
	}

	module = module_new(impl, &module_loopback_methods, sizeof(*d));
	if (module == NULL) {
		res = -errno;
		goto out;
	}

	module->props = props;
	d = module->user_data;
	d->module = module;
	d->capture_props = capture_props;
	d->playback_props = playback_props;
	d->info = info;

	return module;
out:
	pw_properties_free(props);
	pw_properties_free(playback_props);
	pw_properties_free(capture_props);
	errno = -res;

	return NULL;
}
