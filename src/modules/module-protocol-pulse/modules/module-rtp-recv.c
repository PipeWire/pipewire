/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2022 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <spa/utils/hook.h>
#include <spa/utils/json-builder.h>
#include <pipewire/pipewire.h>

#include "../defs.h"
#include "../module.h"

/** \page page_pulse_module_rtp_recv RTP Receiver
 *
 * ## Module Name
 *
 * `module-rtp-recv`
 *
 * ## Module Options
 *
 * @pulse_module_options@
 *
 * ## See Also
 *
 * \ref page_module_rtp_sap "libpipewire-module-rtp-sap"
 */

static const char *const pulse_module_options =
	"sink=<name of the sink> "
	"sap_address=<multicast address to listen on> "
	"latency_msec=<latency in ms> "
	"stream_properties=<properties for the stream> ";

#define NAME "rtp-recv"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_rtp_recv_data {
	struct module *module;

	struct spa_hook mod_listener;
	struct pw_impl_module *mod;

	struct pw_properties *stream_props;
	struct pw_properties *global_props;
};

static void module_destroy(void *data)
{
	struct module_rtp_recv_data *d = data;
	spa_hook_remove(&d->mod_listener);
	d->mod = NULL;
	module_schedule_unload(d->module);
}

static const struct pw_impl_module_events module_events = {
	PW_VERSION_IMPL_MODULE_EVENTS,
	.destroy = module_destroy
};

static int module_rtp_recv_load(struct module *module)
{
	struct module_rtp_recv_data *data = module->user_data;
	struct spa_json_builder b;
	char *args;
	size_t size;
	int res;

	pw_properties_setf(data->stream_props, "pulse.module.id",
			"%u", module->index);

	if ((res = spa_json_builder_memstream(&b, &args, &size, 0)) < 0)
		return res;

	spa_json_builder_array_push(&b, "{");
	pw_properties_serialize_dict(b.f, &data->global_props->dict, 0);
	spa_json_builder_object_push(&b,  "stream.rules", "[");
	spa_json_builder_array_push(&b,     "{");
	spa_json_builder_object_push(&b,      "matches", "[");
	spa_json_builder_array_push(&b,         "{");
	spa_json_builder_object_string(&b,        "rtp.session", "~.*");
	spa_json_builder_pop(&b,                "}");
	spa_json_builder_pop(&b,              "]");
	spa_json_builder_object_push(&b,      "actions", "{");
	spa_json_builder_object_push(&b,        "create-stream", "{");
	pw_properties_serialize_dict(b.f, &data->stream_props->dict, 0);
	spa_json_builder_pop(&b,                "}");
	spa_json_builder_pop(&b,              "}");
	spa_json_builder_pop(&b,            "}");
	spa_json_builder_pop(&b,          "]");
	spa_json_builder_pop(&b,        "}");
	spa_json_builder_close(&b);

	data->mod = pw_context_load_module(module->impl->context,
			"libpipewire-module-rtp-sap",
			args, NULL);

	free(args);

	if (data->mod == NULL)
		return -errno;

	pw_impl_module_add_listener(data->mod,
			&data->mod_listener,
			&module_events, data);

	return 0;
}

static int module_rtp_recv_unload(struct module *module)
{
	struct module_rtp_recv_data *d = module->user_data;

	if (d->mod) {
		spa_hook_remove(&d->mod_listener);
		pw_impl_module_destroy(d->mod);
		d->mod = NULL;
	}

	pw_properties_free(d->global_props);
	pw_properties_free(d->stream_props);

	return 0;
}

static const struct spa_dict_item module_rtp_recv_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Receive data from a network via RTP/SAP/SDP" },
	{ PW_KEY_MODULE_USAGE, pulse_module_options },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

static int module_rtp_recv_prepare(struct module * const module)
{
	struct module_rtp_recv_data * const d = module->user_data;
	struct pw_properties * const props = module->props;
	struct pw_properties *stream_props = NULL, *global_props = NULL;
	const char *str;
	int res;

	PW_LOG_TOPIC_INIT(mod_topic);

	stream_props = pw_properties_new(NULL, NULL);
	global_props = pw_properties_new(NULL, NULL);
	if (!stream_props || !global_props) {
		res = -errno;
		goto out;
	}
	if ((str = pw_properties_get(props, "sap_address")) != NULL)
		pw_properties_set(global_props, "sap.ip", str);

	if ((str = pw_properties_get(props, "sink")) != NULL)
		pw_properties_set(stream_props, PW_KEY_TARGET_OBJECT, str);
	if ((str = pw_properties_get(props, "latency_msec")) != NULL)
		pw_properties_set(stream_props, "sess.latency.msec", str);
	if ((str = pw_properties_get(props, "stream_properties")) != NULL)
		module_args_add_props(stream_props, str);

	d->module = module;
	d->stream_props = stream_props;
	d->global_props = global_props;

	return 0;
out:
	pw_properties_free(stream_props);
	pw_properties_free(global_props);

	return res;
}

static const char* const valid_args[] = {
	"sink",
	"sap_address",
	"latency_msec",
	"stream_properties",
	NULL
};

DEFINE_MODULE_INFO(module_rtp_recv) = {
	.name = "module-rtp-recv",
	.valid_args = valid_args,
	.prepare = module_rtp_recv_prepare,
	.load = module_rtp_recv_load,
	.unload = module_rtp_recv_unload,
	.properties = &SPA_DICT_INIT_ARRAY(module_rtp_recv_info),
	.data_size = sizeof(struct module_rtp_recv_data),
};
