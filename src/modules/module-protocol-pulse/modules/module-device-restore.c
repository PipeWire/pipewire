/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <pipewire/pipewire.h>

#include "../module.h"

/** \page page_pulse_module_device_restore Device restore extension
 *
 * ## Module Name
 *
 * `module-device-restore`
 *
 * ## Module Options
 *
 * @pulse_module_options@
 */

#define EXT_DEVICE_RESTORE_VERSION	1

enum {
    SUBCOMMAND_TEST,
    SUBCOMMAND_SUBSCRIBE,
    SUBCOMMAND_EVENT,
    SUBCOMMAND_READ_FORMATS_ALL,
    SUBCOMMAND_READ_FORMATS,
    SUBCOMMAND_SAVE_FORMATS
};

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/pod/builder.h>
#include <spa/utils/defs.h>
#include <spa/utils/dict.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <spa/param/audio/format.h>
#include <spa/param/props.h>

#include <pipewire/log.h>
#include <pipewire/properties.h>

#include "../client.h"
#include "../collect.h"
#include "../commands.h"
#include "../defs.h"
#include "../extension.h"
#include "../format.h"
#include "../manager.h"
#include "../message.h"
#include "../operation.h"
#include "../reply.h"
#include "../volume.h"

static const char *const pulse_module_options =
	"restore_port=<Save/restore port?> "
	"restore_volume=<Save/restore volumes?> "
	"restore_muted=<Save/restore muted states?> "
	"restore_formats=<Save/restore saved formats?>";

#define NAME "device-restore"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_device_restore_data {
	struct module *module;

	struct spa_list subscribed;
};

static const struct spa_dict_item module_device_restore_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Automatically restore the volume/mute state of devices" },
	{ PW_KEY_MODULE_USAGE, pulse_module_options },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#define DEVICE_TYPE_SINK	0
#define DEVICE_TYPE_SOURCE	1

static int do_extension_device_restore_test(struct module *module, struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct message *reply;

	reply = reply_new(client, tag);
	message_put(reply,
			TAG_U32, EXT_DEVICE_RESTORE_VERSION,
			TAG_INVALID);

	return client_queue_message(client, reply);
}

struct subscribe {
	struct spa_list link;
	struct module_device_restore_data *data;

	struct client *client;
	struct spa_hook listener;

	struct spa_hook manager_listener;
};

static void remove_subscribe(struct subscribe *s)
{
	spa_list_remove(&s->link);
	spa_hook_remove(&s->listener);
	spa_hook_remove(&s->manager_listener);
	free(s);
}

static void emit_event(struct subscribe *s, uint32_t type, uint32_t idx)
{
	struct client *client = s->client;
	struct message *msg = message_alloc(client->impl, -1, 0);

	pw_log_info("[%s] EVENT index:%u name:%s %d/%d", client->name,
			s->data->module->index, s->data->module->info->name, type, idx);

	message_put(msg,
		TAG_U32, COMMAND_EXTENSION,
		TAG_U32, 0,
		TAG_U32, s->data->module->index,
		TAG_STRING, s->data->module->info->name,
		TAG_U32, SUBCOMMAND_EVENT,
		TAG_U32, type,
		TAG_U32, idx,
		TAG_INVALID);

	client_queue_message(client, msg);
}

static void module_client_disconnect(void *data)
{
	struct subscribe *s = data;
	remove_subscribe(s);
}

static const struct client_events module_client_events = {
	VERSION_CLIENT_EVENTS,
	.disconnect = module_client_disconnect,
};

static void manager_updated(void *data, struct pw_manager_object *object)
{
	struct subscribe *s = data;
	uint32_t i;

	if (!pw_manager_object_is_sink(object))
		return;

	for (i = 0; i < object->n_params; i++) {
		if (object->params[i].id == SPA_PARAM_EnumFormat &&
		    object->params[i].user != 0)
			emit_event(s, DEVICE_TYPE_SINK, object->index);
	}
}

struct pw_manager_events manager_events = {
	PW_VERSION_MANAGER_EVENTS,
	.added = manager_updated,
	.updated = manager_updated,
};

static struct subscribe *add_subscribe(struct module_device_restore_data *data, struct client *c)
{
	struct subscribe *s;
	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return NULL;
	s->data = data;
	s->client = c;
	client_add_listener(c, &s->listener, &module_client_events, s);
	spa_list_append(&data->subscribed, &s->link);

	pw_manager_add_listener(c->manager, &s->manager_listener,
			&manager_events, s);
	return s;
}

static struct subscribe *find_subscribe(struct module_device_restore_data *data, struct client *c)
{
	struct subscribe *s;
	spa_list_for_each(s, &data->subscribed, link) {
		if (s->client == c)
			return s;
	}
	return NULL;
}

static int do_extension_device_restore_subscribe(struct module *module, struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct module_device_restore_data * const d = module->user_data;
	int res;
	bool enabled;
	struct subscribe *s;

	if ((res = message_get(m,
			TAG_BOOLEAN, &enabled,
			TAG_INVALID)) < 0)
		return -EPROTO;

	s = find_subscribe(d, client);
	if (enabled) {
		if (s == NULL)
			s = add_subscribe(d, client);
		if (s == NULL)
			return -errno;
	} else {
		if (s != NULL)
			remove_subscribe(s);
	}
	return reply_simple_ack(client, tag);
}

struct format_data {
        struct client *client;
        struct message *reply;
};

static int do_sink_read_format(void *data, struct pw_manager_object *o)
{
	struct format_data *d = data;
	struct pw_manager_param *p;
	struct format_info info[32];
	uint32_t i, n_info = 0;

	if (!pw_manager_object_is_sink(o))
		return 0;

	spa_list_for_each(p, &o->param_list, link) {
		uint32_t index = 0;

		if (p->id != SPA_PARAM_EnumFormat)
			continue;

		while (n_info < SPA_N_ELEMENTS(info)) {
			spa_zero(info[n_info]);
			if (format_info_from_param(&info[n_info], p->param, index++) < 0)
				break;
			if (info[n_info].encoding == ENCODING_ANY) {
				format_info_clear(&info[n_info]);
				continue;
			}
			n_info++;
		}
	}
	message_put(d->reply,
		TAG_U32, DEVICE_TYPE_SINK,
		TAG_U32, o->index,			/* sink index */
		TAG_U8, n_info,				/* n_formats */
		TAG_INVALID);
	for (i = 0; i < n_info; i++) {
		message_put(d->reply,
			TAG_FORMAT_INFO, &info[i],
			TAG_INVALID);
		format_info_clear(&info[i]);
	}
	return 0;
}

static int do_extension_device_restore_read_formats_all(struct module *module, struct client *client,
		uint32_t command, uint32_t tag, struct message *m)
{
	struct pw_manager *manager = client->manager;
	struct format_data data;

	spa_zero(data);
	data.client = client;
	data.reply = reply_new(client, tag);

	pw_manager_for_each_object(manager, do_sink_read_format, &data);

	return client_queue_message(client, data.reply);
}

static int do_extension_device_restore_read_formats(struct module *module, struct client *client,
		uint32_t command, uint32_t tag, struct message *m)
{
	struct pw_manager *manager = client->manager;
	struct format_data data;
	uint32_t type, sink_index;
	struct selector sel;
	struct pw_manager_object *o;
	int res;

	if ((res = message_get(m,
			TAG_U32, &type,
			TAG_U32, &sink_index,
			TAG_INVALID)) < 0)
		return -EPROTO;

	if (type != DEVICE_TYPE_SINK) {
		pw_log_info("Device format reading is only supported on sinks");
		return -ENOTSUP;
	}

	spa_zero(sel);
	sel.index = sink_index;
	sel.type = pw_manager_object_is_sink;

	o = select_object(manager, &sel);
        if (o == NULL)
		return -ENOENT;

	spa_zero(data);
	data.client = client;
	data.reply = reply_new(client, tag);

	do_sink_read_format(&data, o);

	return client_queue_message(client, data.reply);
}

static int set_card_codecs(struct pw_manager_object *o, uint32_t port_index,
		uint32_t device_id, uint32_t n_codecs, uint32_t *codecs)
{
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[2];
	struct spa_pod *param;

	if (!SPA_FLAG_IS_SET(o->permissions, PW_PERM_W | PW_PERM_X))
		return -EACCES;

	if (o->proxy == NULL)
		return -ENOENT;

	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
	spa_pod_builder_add(&b,
			SPA_PARAM_ROUTE_index, SPA_POD_Int(port_index),
			SPA_PARAM_ROUTE_device, SPA_POD_Int(device_id),
			0);
	spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
	spa_pod_builder_push_object(&b, &f[1],
			SPA_TYPE_OBJECT_Props,  SPA_PARAM_Props);
	spa_pod_builder_add(&b,
			SPA_PROP_iec958Codecs, SPA_POD_Array(sizeof(uint32_t),
				SPA_TYPE_Id, n_codecs, codecs), 0);
	spa_pod_builder_pop(&b, &f[1]);
	spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_save, 0);
	spa_pod_builder_bool(&b, true);
	param = spa_pod_builder_pop(&b, &f[0]);

	pw_device_set_param((struct pw_device*)o->proxy,
			SPA_PARAM_Route, 0, param);
	return 0;
}

static int set_node_codecs(struct pw_manager_object *o, uint32_t n_codecs, uint32_t *codecs)
{
	char buf[1024];
	struct spa_pod_builder b;
	struct spa_pod *param;

	if (!SPA_FLAG_IS_SET(o->permissions, PW_PERM_W | PW_PERM_X))
		return -EACCES;

	if (o->proxy == NULL)
		return -ENOENT;

	spa_pod_builder_init(&b, buf, sizeof(buf));
	param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
			SPA_PROP_iec958Codecs, SPA_POD_Array(sizeof(uint32_t),
				SPA_TYPE_Id, n_codecs, codecs));

	pw_node_set_param((struct pw_node*)o->proxy,
			SPA_PARAM_Props, 0, param);

	return 0;
}

static int do_extension_device_restore_save_formats(struct module *module, struct client *client,
		uint32_t command, uint32_t tag, struct message *m)
{
	struct pw_manager *manager = client->manager;
	struct selector sel;
	struct pw_manager_object *o, *card = NULL;
	struct pw_node_info *info;
	int res;
	uint32_t type, sink_index;
	uint8_t i, n_formats;
	uint32_t n_codecs = 0, codec, iec958codecs[32];
	struct device_info dev_info;

	if ((res = message_get(m,
			TAG_U32, &type,
			TAG_U32, &sink_index,
			TAG_U8, &n_formats,
			TAG_INVALID)) < 0)
		return -EPROTO;
	if (n_formats < 1)
		return -EPROTO;

	if (type != DEVICE_TYPE_SINK)
		return -ENOTSUP;

	for (i = 0; i < n_formats; ++i) {
		struct format_info format;
		spa_zero(format);
		if (message_get(m,
				TAG_FORMAT_INFO, &format,
				TAG_INVALID) < 0)
			return -EPROTO;

		codec = format_encoding2id(format.encoding);
		if (codec != SPA_ID_INVALID && n_codecs < SPA_N_ELEMENTS(iec958codecs))
			iec958codecs[n_codecs++] = codec;

		format_info_clear(&format);
	}
	if (n_codecs == 0)
		return -ENOTSUP;

	spa_zero(sel);
	sel.index = sink_index;
	sel.type = pw_manager_object_is_sink;

	o = select_object(manager, &sel);
	if (o == NULL || (info = o->info) == NULL || info->props == NULL)
		return -ENOENT;

	get_device_info(o, &dev_info, SPA_DIRECTION_INPUT, false);

	if (dev_info.card_id != SPA_ID_INVALID) {
		struct selector sel = { .id = dev_info.card_id, .type = pw_manager_object_is_card, };
		card = select_object(manager, &sel);
	}
	if (card != NULL && dev_info.active_port != SPA_ID_INVALID) {
		res = set_card_codecs(card, dev_info.active_port,
				dev_info.device, n_codecs, iec958codecs);
	} else {
		res = set_node_codecs(o, n_codecs, iec958codecs);
	}
	if (res < 0)
		return res;

	return reply_simple_ack(client, tag);
}

static const struct extension module_device_restore_extension[] = {
	{ "TEST", SUBCOMMAND_TEST, do_extension_device_restore_test, },
	{ "SUBSCRIBE", SUBCOMMAND_SUBSCRIBE, do_extension_device_restore_subscribe, },
	{ "EVENT", SUBCOMMAND_EVENT, },
	{ "READ_FORMATS_ALL", SUBCOMMAND_READ_FORMATS_ALL, do_extension_device_restore_read_formats_all, },
	{ "READ_FORMATS", SUBCOMMAND_READ_FORMATS, do_extension_device_restore_read_formats, },
	{ "SAVE_FORMATS", SUBCOMMAND_SAVE_FORMATS, do_extension_device_restore_save_formats, },
	{ NULL },
};

static int module_device_restore_prepare(struct module * const module)
{
	PW_LOG_TOPIC_INIT(mod_topic);

	struct module_device_restore_data * const data = module->user_data;
	data->module = module;

	return 0;
}

static int module_device_restore_load(struct module *module)
{
	struct module_device_restore_data * const data = module->user_data;
	spa_list_init(&data->subscribed);
	return 0;
}
static int module_device_restore_unload(struct module *module)
{
	struct module_device_restore_data * const data = module->user_data;
	struct subscribe *s;

	spa_list_consume(s, &data->subscribed, link)
		remove_subscribe(s);
	return 0;
}

DEFINE_MODULE_INFO(module_device_restore) = {
	.name = "module-device-restore",
	.load_once = true,
	.prepare = module_device_restore_prepare,
	.load = module_device_restore_load,
	.unload = module_device_restore_unload,
	.extension = module_device_restore_extension,
	.properties = &SPA_DICT_INIT_ARRAY(module_device_restore_info),
	.data_size = sizeof(struct module_device_restore_data),
};
