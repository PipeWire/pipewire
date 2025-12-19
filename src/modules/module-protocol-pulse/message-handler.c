/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2021 Pauli Virtanen */
/* SPDX-License-Identifier: MIT */

#include <stdint.h>
#include <stdio.h>

#include <regex.h>
#include <malloc.h>

#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/pod.h>
#include <spa/utils/defs.h>
#include <spa/utils/json.h>
#include <spa/utils/string.h>

#include <pipewire/pipewire.h>

#include "client.h"
#include "collect.h"
#include "defs.h"
#include "log.h"
#include "manager.h"
#include "module.h"
#include "message-handler.h"

static int bluez_card_object_message_handler(struct client *client, struct pw_manager_object *o, const char *message, const char *params, FILE *response)
{
	struct transport_codec_info codecs[64];
	uint32_t n_codecs, active;

	pw_log_debug(": bluez-card %p object message:'%s' params:'%s'", o, message, params);

	n_codecs = collect_transport_codec_info(o, codecs, SPA_N_ELEMENTS(codecs), &active);

	if (n_codecs == 0)
		return -EINVAL;

	if (spa_streq(message, "switch-codec")) {
		char codec[256];
		struct spa_json it;
		char buf[1024];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
		struct spa_pod_frame f[1];
		struct spa_pod *param;
		uint32_t codec_id = SPA_ID_INVALID;

		/* Parse args */
		if (params == NULL)
			return -EINVAL;

		spa_json_init(&it, params, strlen(params));
		if (spa_json_get_string(&it, codec, sizeof(codec)) <= 0)
			return -EINVAL;

		codec_id = atoi(codec);

		/* Switch codec */
		spa_pod_builder_push_object(&b, &f[0],
				SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
		spa_pod_builder_add(&b,
				SPA_PROP_bluetoothAudioCodec, SPA_POD_Id(codec_id), 0);
		param = spa_pod_builder_pop(&b, &f[0]);

		pw_device_set_param((struct pw_device *)o->proxy,
				SPA_PARAM_Props, 0, param);
	} else if (spa_streq(message, "list-codecs")) {
		uint32_t i;
		bool first = true;

		fputc('[', response);
		for (i = 0; i < n_codecs; ++i) {
			const char *desc = codecs[i].description;
			fprintf(response, "%s{\"name\":\"%d\",\"description\":\"%s\"}",
					first ? "" : ",",
					(int)codecs[i].id, desc ? desc : "Unknown");
			first = false;
		}
		fputc(']', response);
	} else if (spa_streq(message, "get-codec")) {
		if (active == SPA_ID_INVALID)
			fputs("null", response);
		else
			fprintf(response, "\"%d\"", (int) codecs[active].id);
	} else {
		return -ENOSYS;
	}

	return 0;
}


static int core_object_force_mono_output(struct client *client, const char *params, FILE *response)
{
	if (!client->have_force_mono_audio) {
		/* Not supported, return a null value to indicate that */
		fprintf(response, "null");
		return 0;
	}

	if (!params || params[0] == '\0') {
		/* No parameter => query the current value */
		fprintf(response, "%s", client->force_mono_audio ? "true" : "false");
		return 0;
	} else {
		/* The caller is trying to set a value or clear with a null */
		int ret;

		if (spa_streq(params, "true")) {
			ret = pw_manager_set_metadata(client->manager, client->metadata_sm_settings, PW_ID_CORE,
					METADATA_FEATURES_AUDIO_MONO, "Spa:String:JSON", "true");
		} else if (spa_streq(params, "false")) {
			ret = pw_manager_set_metadata(client->manager, client->metadata_sm_settings, PW_ID_CORE,
					METADATA_FEATURES_AUDIO_MONO, "Spa:String:JSON", "false");
		} else if (spa_streq(params, "null")) {
			ret = pw_manager_set_metadata(client->manager, client->metadata_sm_settings, PW_ID_CORE,
					METADATA_FEATURES_AUDIO_MONO, NULL, NULL);
		} else {
			fprintf(response, "Value must be true, false, or clear");
			return -EINVAL;
		}

		if (ret < 0)
			fprintf(response, "Could not set metadata: %s", spa_strerror(ret));
		else
			fprintf(response, "%s", params);

		return ret;
	}
}

static int core_object_message_handler(struct client *client, struct pw_manager_object *o, const char *message, const char *params, FILE *response)
{
	pw_log_debug(": core %p object message:'%s' params:'%s'", o, message, params);

	if (spa_streq(message, "list-handlers")) {
		bool first = true;

		fputc('[', response);
		spa_list_for_each(o, &client->manager->object_list, link) {
			if (o->message_object_path) {
				fprintf(response, "%s{\"name\":\"%s\",\"description\":\"%s\"}",
						first ? "" : ",",
						o->message_object_path, o->type);
				first = false;
			}
		}
		fputc(']', response);
#ifdef HAVE_MALLOC_INFO
	} else if (spa_streq(message, "pipewire-pulse:malloc-info")) {
		malloc_info(0, response);
#endif
#ifdef HAVE_MALLOC_TRIM
	} else if (spa_streq(message, "pipewire-pulse:malloc-trim")) {
		int res = malloc_trim(0);
		fprintf(response, "%d", res);
#endif
	} else if (spa_streq(message, "pipewire-pulse:log-level")) {
		int res = pw_log_set_level_string(params);
		fprintf(response, "%d", res);
	} else if (spa_streq(message, "pipewire-pulse:describe-module")) {
		const struct module_info *i = module_info_find(client->impl, params);

		if (i != NULL) {
			fprintf(response, "Name: %s\n", i->name);
		        if (i->properties == NULL) {
				fprintf(response, "No module information available\n");
			} else {
				const char *s;
				if ((s = spa_dict_lookup(i->properties, PW_KEY_MODULE_VERSION)))
					fprintf(response, "Version: %s\n", s);
				if ((s = spa_dict_lookup(i->properties, PW_KEY_MODULE_DESCRIPTION)))
					fprintf(response, "Description: %s\n", s);
				if ((s = spa_dict_lookup(i->properties, PW_KEY_MODULE_AUTHOR)))
					fprintf(response, "Author: %s\n", s);
				if ((s = spa_dict_lookup(i->properties, PW_KEY_MODULE_USAGE)))
					fprintf(response, "Usage: %s\n", s);
				fprintf(response, "Load Once: %s\n", i->load_once ? "Yes": "No");
				if ((s = spa_dict_lookup(i->properties, PW_KEY_MODULE_DEPRECATED)))
					fprintf(response, "Warning, deprecated: %s\n", s);
			}
		} else {
			fprintf(response, "Failed to open module.\n");
		}
	} else if (spa_streq(message, "pipewire-pulse:force-mono-output")) {
		return core_object_force_mono_output(client, params, response);
	} else {
		return -ENOSYS;
	}

	return 0;
}

void register_object_message_handlers(struct pw_manager_object *o)
{
	const char *str;

	if (o->id == PW_ID_CORE) {
		free(o->message_object_path);
		o->message_object_path = strdup("/core");
		o->message_handler = core_object_message_handler;
		return;
	}

	if (pw_manager_object_is_card(o) && o->props != NULL &&
	    (str = pw_properties_get(o->props, PW_KEY_DEVICE_API)) != NULL &&
	    spa_streq(str, "bluez5")) {
		str = pw_properties_get(o->props, PW_KEY_DEVICE_NAME);
		if (str) {
			free(o->message_object_path);
			o->message_object_path = spa_aprintf("/card/%s/bluez", str);
			o->message_handler = bluez_card_object_message_handler;
		}
		return;
	}
}
