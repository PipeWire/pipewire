/* PipeWire
 *
 * Copyright © 2022 Wim Taymans <wim.taymans@gmail.com>
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

#define NAME "rtp-send"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_rtp_send_data {
	struct module *module;

	struct spa_hook mod_listener;
	struct pw_impl_module *mod;

	struct pw_properties *stream_props;
	struct pw_properties *global_props;
	struct spa_audio_info_raw info;
};

static void module_destroy(void *data)
{
	struct module_rtp_send_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_rtp_send_load(struct module *module)
{
	struct module_rtp_send_data *data = module->user_data;
	FILE *f;
	char *args;
	size_t size;
	uint32_t i;

	pw_properties_setf(data->stream_props, "pulse.module.id",
			"%u", module->index);

	if ((f = open_memstream(&args, &size)) == NULL)
		return -errno;

	fprintf(f, "{");
	pw_properties_serialize_dict(f, &data->global_props->dict, 0);
	if (data->info.format != 0)
		fprintf(f, " \"audio.format\": \"%s\"", format_id2name(data->info.format));
	if (data->info.rate != 0)
		fprintf(f, " \"audio.rate\": %u,", data->info.rate);
	if (data->info.channels != 0) {
		fprintf(f, " \"audio.channels\": %u,", data->info.channels);
		if (!(data->info.flags & SPA_AUDIO_FLAG_UNPOSITIONED)) {
			fprintf(f, " \"audio.position\": [ ");
			for (i = 0; i < data->info.channels; i++)
				fprintf(f, "%s\"%s\"", i == 0 ? "" : ",",
					channel_id2name(data->info.position[i]));
			fprintf(f, " ],");
		}
	}
	fprintf(f, " stream.props = {");
	pw_properties_serialize_dict(f, &data->stream_props->dict, 0);
	fprintf(f, " } }");
	fclose(f);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-rtp-sink",
			args, NULL);

	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);

	return 0;
}

static int module_rtp_send_unload(struct module *module)
{
	struct module_rtp_send_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}

	pw_properties_free(d->global_props);
	pw_properties_free(d->stream_props);

	return 0;
}

static const struct spa_dict_item module_rtp_send_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Read data from source and send it to the network via RTP/SAP/SDP" },
	{ PW_KEY_MODULE_USAGE,	"source=<name of the source> "
				"format=<sample format> "
				"channels=<number of channels> "
				"rate=<sample rate> "
				"destination_ip=<destination IP address> "
				"source_ip=<source IP address> "
				"port=<port number> "
				"mtu=<maximum transfer unit> "
				"loop=<loopback to local host?> "
				"ttl=<ttl value> "
				"inhibit_auto_suspend=<always|never|only_with_non_monitor_sources> "
				"stream_name=<name of the stream> "
				"enable_opus=<enable OPUS codec>" },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_rtp_send_prepare(struct module * const module)
{
	struct module_rtp_send_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *stream_props = NULL, *global_props = NULL;
	struct spa_audio_info_raw info = { 0 };
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	stream_props = pw_properties_new(NULL, NULL);
	global_props = pw_properties_new(NULL, NULL);
	if (!stream_props || !global_props) {
		res = -errno;
		goto out;
	}

	if ((str = pw_properties_get(props, "source")) != NULL) {
		pw_properties_set(stream_props, PW_KEY_NODE_TARGET, str);
		if (spa_strendswith(str, ".monitor")) {
			pw_properties_setf(stream_props, PW_KEY_NODE_TARGET,
					"%.*s", (int)strlen(str)-8, str);
			pw_properties_set(stream_props, PW_KEY_STREAM_CAPTURE_SINK,
					"true");
		} else {
			pw_properties_set(stream_props, PW_KEY_NODE_TARGET, str);
		}
	}
	if (module_args_to_audioinfo(module->impl, props, &info) < 0) {
		res = -EINVAL;
		goto out;
	}
	info.format = 0;
	if ((str = pw_properties_get(props, "format")) != NULL) {
		if ((info.format = format_paname2id(str, strlen(str))) ==
				SPA_AUDIO_FORMAT_UNKNOWN) {
			pw_log_error("unknown format %s", str);
			res = -EINVAL;
			goto out;
		}
	}

	if ((str = pw_properties_get(props, "destination_ip")) != NULL)
		pw_properties_set(global_props, "destination.ip", str);
	if ((str = pw_properties_get(props, "source_ip")) != NULL)
		pw_properties_set(global_props, "source.ip", str);
	if ((str = pw_properties_get(props, "port")) != NULL)
		pw_properties_set(global_props, "destination.port", str);
	if ((str = pw_properties_get(props, "mtu")) != NULL)
		pw_properties_set(global_props, "net.mtu", str);
	if ((str = pw_properties_get(props, "loop")) != NULL)
		pw_properties_set(global_props, "net.loop",
				module_args_parse_bool(str) ? "true" : "false");
	if ((str = pw_properties_get(props, "ttl")) != NULL)
		pw_properties_set(global_props, "net.ttl", str);
	if ((str = pw_properties_get(props, "stream_name")) != NULL)
		pw_properties_set(global_props, "sess.name", str);

	d->module = module;
	d->stream_props = stream_props;
	d->global_props = global_props;
	d->info = info;

	return 0;
out:
	pw_properties_free(stream_props);
	pw_properties_free(global_props);

	return res;
}

DEFINE_MODULE_INFO(module_rtp_send) = {
	.name = "module-rtp-send",
	.prepare = module_rtp_send_prepare,
	.load = module_rtp_send_load,
	.unload = module_rtp_send_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_rtp_send_info),
	.data_size = sizeof(struct module_rtp_send_data),
};
