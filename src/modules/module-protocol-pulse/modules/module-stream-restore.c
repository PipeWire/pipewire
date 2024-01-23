/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2024 Wim Taymans <wim.taymans@gmail.com> */
/* SPDX-License-Identifier: MIT */

#include <pipewire/pipewire.h>

#include "../module.h"

/** \page page_pulse_module_stream_restore Stream restore extension
 *
 * ## Module Name
 *
 * `module-stream-restore`
 *
 * ## Module Options
 *
 * @pulse_module_options@
 */

static const char *const pulse_module_options =
	"restore_device=<Save/restore sinks/sources?> "
	"restore_volume=<Save/restore volumes?> "
	"restore_muted=<Save/restore muted states?> "
	"on_hotplug=<This argument is obsolete, please remove it from configuration> "
	"on_rescue=<This argument is obsolete, please remove it from configuration> "
	"fallback_table=<filename>";

#define NAME "stream-restore"

PW_LOG_TOPIC_STATIC(mod_topic, "mod." NAME);
#define PW_LOG_TOPIC_DEFAULT mod_topic

struct module_stream_restore_data {
	struct module *module;
};

static const struct spa_dict_item module_stream_restore_info[] = {
	{ PW_KEY_MODULE_AUTHOR, "Wim Taymans <wim.taymans@gmail.com>" },
	{ PW_KEY_MODULE_DESCRIPTION, "Automatically restore the volume/mute/device state of streams" },
	{ PW_KEY_MODULE_USAGE, pulse_module_options },
	{ PW_KEY_MODULE_VERSION, PACKAGE_VERSION },
};

#define EXT_STREAM_RESTORE_VERSION	1

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/utils/defs.h>
#include <spa/utils/dict.h>
#include <spa/utils/string.h>
#include <spa/utils/json.h>
#include <pipewire/log.h>
#include <pipewire/properties.h>

#include "../client.h"
#include "../defs.h"
#include "../extension.h"
#include "../format.h"
#include "../manager.h"
#include "../message.h"
#include "../remap.h"
#include "../reply.h"
#include "../volume.h"

PW_LOG_TOPIC_EXTERN(pulse_ext_stream_restore);
#undef PW_LOG_TOPIC_DEFAULT
#define PW_LOG_TOPIC_DEFAULT pulse_ext_stream_restore

static int do_extension_stream_restore_test(struct module *module, struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct message *reply;

	reply = reply_new(client, tag);
	message_put(reply,
			TAG_U32, EXT_STREAM_RESTORE_VERSION,
			TAG_INVALID);

	return client_queue_message(client, reply);
}

static int key_from_name(const char *name, char *key, size_t maxlen)
{
	const char *media_class, *select, *str;

	if (spa_strstartswith(name, "sink-input-"))
		media_class = "Output/Audio";
	else if (spa_strstartswith(name, "source-output-"))
		media_class = "Input/Audio";
	else
		return -1;

	if ((str = strstr(name, "-by-media-role:")) != NULL) {
		const struct str_map *map;
		str += strlen("-by-media-role:");
		map = str_map_find(media_role_map, NULL, str);
		str = map ? map->pw_str : str;
		select = "media.role";
	}
	else if ((str = strstr(name, "-by-application-id:")) != NULL) {
		str += strlen("-by-application-id:");
		select = "application.id";
	}
	else if ((str = strstr(name, "-by-application-name:")) != NULL) {
		str += strlen("-by-application-name:");
		select = "application.name";
	}
	else if ((str = strstr(name, "-by-media-name:")) != NULL) {
		str += strlen("-by-media-name:");
		select = "media.name";
	} else
		return -1;

	snprintf(key, maxlen, "restore.stream.%s.%s:%s",
				media_class, select, str);
	return 0;
}

static int key_to_name(const char *key, char *name, size_t maxlen)
{
	const char *type, *select, *str;

	if (spa_strstartswith(key, "restore.stream.Output/Audio."))
		type = "sink-input";
	else if (spa_strstartswith(key, "restore.stream.Input/Audio."))
		type = "source-output";
	else
		type = "stream";

	if ((str = strstr(key, ".media.role:")) != NULL) {
		const struct str_map *map;
		str += strlen(".media.role:");
		map = str_map_find(media_role_map, str, NULL);
		select = "media-role";
		str = map ? map->pa_str : str;
	}
	else if ((str = strstr(key, ".application.id:")) != NULL) {
		str += strlen(".application.id:");
		select = "application-id";
	}
	else if ((str = strstr(key, ".application.name:")) != NULL) {
		str += strlen(".application.name:");
		select = "application-name";
	}
	else if ((str = strstr(key, ".media.name:")) != NULL) {
		str += strlen(".media.name:");
		select = "media-name";
	}
	else
		return -1;

	snprintf(name, maxlen, "%s-by-%s:%s", type, select, str);
	return 0;

}

static int do_extension_stream_restore_read(struct module *module, struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	struct message *reply;
	const struct spa_dict_item *item;

	reply = reply_new(client, tag);

	spa_dict_for_each(item, &client->routes->dict) {
		struct spa_json it[3];
		const char *value;
		char name[1024], key[128];
		char device_name[1024] = "\0";
		bool mute = false;
		struct volume vol = VOLUME_INIT;
		struct channel_map map = CHANNEL_MAP_INIT;
		float volume = 0.0f;

		if (key_to_name(item->key, name, sizeof(name)) < 0)
			continue;

		pw_log_debug("%s -> %s: %s", item->key, name, item->value);

		spa_json_init(&it[0], item->value, strlen(item->value));
		if (spa_json_enter_object(&it[0], &it[1]) <= 0)
			continue;

		while (spa_json_get_string(&it[1], key, sizeof(key)) > 0) {
			if (spa_streq(key, "volume")) {
				if (spa_json_get_float(&it[1], &volume) <= 0)
					continue;
			}
			else if (spa_streq(key, "mute")) {
				if (spa_json_get_bool(&it[1], &mute) <= 0)
					continue;
			}
			else if (spa_streq(key, "volumes")) {
				vol = VOLUME_INIT;
				if (spa_json_enter_array(&it[1], &it[2]) <= 0)
					continue;

				for (vol.channels = 0; vol.channels < CHANNELS_MAX; vol.channels++) {
					if (spa_json_get_float(&it[2], &vol.values[vol.channels]) <= 0)
						break;
				}
			}
			else if (spa_streq(key, "channels")) {
				if (spa_json_enter_array(&it[1], &it[2]) <= 0)
					continue;

				for (map.channels = 0; map.channels < CHANNELS_MAX; map.channels++) {
					char chname[16];
					if (spa_json_get_string(&it[2], chname, sizeof(chname)) <= 0)
						break;
					map.map[map.channels] = channel_name2id(chname);
				}
			}
			else if (spa_streq(key, "target-node")) {
				if (spa_json_get_string(&it[1], device_name, sizeof(device_name)) <= 0)
					continue;
			}
			else if (spa_json_next(&it[1], &value) <= 0)
				break;
		}
		message_put(reply,
			TAG_STRING, name,
			TAG_CHANNEL_MAP, &map,
			TAG_CVOLUME, &vol,
			TAG_STRING, device_name[0] ? device_name : NULL,
			TAG_BOOLEAN, mute,
			TAG_INVALID);
	}

	return client_queue_message(client, reply);
}

static int do_extension_stream_restore_write(struct module *module, struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	int res;
	uint32_t mode;
	bool apply;

	if ((res = message_get(m,
			TAG_U32, &mode,
			TAG_BOOLEAN, &apply,
			TAG_INVALID)) < 0)
		return -EPROTO;

	while (m->offset < m->length) {
		const char *name, *device_name = NULL;
		struct channel_map map;
		struct volume vol;
		bool mute = false;
		uint32_t i;
		FILE *f;
		char *ptr;
		size_t size;
		char key[1024], buf[128];

		spa_zero(map);
		spa_zero(vol);

		if (message_get(m,
				TAG_STRING, &name,
				TAG_CHANNEL_MAP, &map,
				TAG_CVOLUME, &vol,
				TAG_STRING, &device_name,
				TAG_BOOLEAN, &mute,
				TAG_INVALID) < 0)
			return -EPROTO;

		if (name == NULL || name[0] == '\0')
			return -EPROTO;

		if ((f = open_memstream(&ptr, &size)) == NULL)
			return -errno;

		fprintf(f, "{");
		fprintf(f, " \"mute\": %s", mute ? "true" : "false");
		if (vol.channels > 0) {
			fprintf(f, ", \"volumes\": [");
			for (i = 0; i < vol.channels; i++)
				fprintf(f, "%s%s", (i == 0 ? " ":", "),
						spa_json_format_float(buf, sizeof(buf), vol.values[i]));
			fprintf(f, " ]");
		}
		if (map.channels > 0) {
			fprintf(f, ", \"channels\": [");
			for (i = 0; i < map.channels; i++)
				fprintf(f, "%s\"%s\"", (i == 0 ? " ":", "), channel_id2name(map.map[i]));
			fprintf(f, " ]");
		}
		if (device_name != NULL && device_name[0] &&
		    (client->default_source == NULL || !spa_streq(device_name, client->default_source)) &&
		    (client->default_sink == NULL || !spa_streq(device_name, client->default_sink)))
			fprintf(f, ", \"target-node\": \"%s\"", device_name);
		fprintf(f, " }");
		fclose(f);
		if (key_from_name(name, key, sizeof(key)) >= 0) {
			pw_log_debug("%s -> %s: %s", name, key, ptr);
			if ((res = pw_manager_set_metadata(client->manager,
							client->metadata_routes,
							PW_ID_CORE, key, "Spa:String:JSON", "%s", ptr)) < 0)
				pw_log_warn("failed to set metadata %s = %s, %s", key, ptr, strerror(-res));
		}
		free(ptr);
	}

	return reply_simple_ack(client, tag);
}

static int do_extension_stream_restore_delete(struct module *module, struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	return reply_simple_ack(client, tag);
}

static int do_extension_stream_restore_subscribe(struct module *module, struct client *client, uint32_t command, uint32_t tag, struct message *m)
{
	return reply_simple_ack(client, tag);
}

static const struct extension module_stream_restore_extension[] = {
	{ "TEST", 0, do_extension_stream_restore_test, },
	{ "READ", 1, do_extension_stream_restore_read, },
	{ "WRITE", 2, do_extension_stream_restore_write, },
	{ "DELETE", 3, do_extension_stream_restore_delete, },
	{ "SUBSCRIBE", 4, do_extension_stream_restore_subscribe, },
	{ "EVENT", 5, },
	{ NULL, },
};

static int module_stream_restore_prepare(struct module * const module)
{
	PW_LOG_TOPIC_INIT(mod_topic);

	struct module_stream_restore_data * const data = module->user_data;
	data->module = module;

	return 0;
}

static int module_stream_restore_load(struct module *module)
{
	return 0;
}

DEFINE_MODULE_INFO(module_stream_restore) = {
	.name = "module-stream-restore",
	.load_once = true,
	.prepare = module_stream_restore_prepare,
	.load = module_stream_restore_load,
	.extension = module_stream_restore_extension,
	.properties = &SPA_DICT_INIT_ARRAY(module_stream_restore_info),
	.data_size = sizeof(struct module_stream_restore_data),
};
