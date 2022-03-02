#include <stdint.h>

#include <regex.h>

#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/pod.h>
#include <spa/utils/defs.h>
#include <spa/utils/json.h>
#include <spa/utils/string.h>

#include <pipewire/pipewire.h>

#include "collect.h"
#include "log.h"
#include "manager.h"
#include "message-handler.h"

static int bluez_card_object_message_handler(struct pw_manager *m, struct pw_manager_object *o, const char *message, const char *params, char **response)
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
		return 0;
	} else if (spa_streq(message, "list-codecs")) {
		uint32_t i;
		FILE *r;
		size_t size;
		bool first = true;

		r = open_memstream(response, &size);
		if (r == NULL)
			return -errno;

		fputc('[', r);
		for (i = 0; i < n_codecs; ++i) {
			const char *desc = codecs[i].description;
			fprintf(r, "%s{\"name\":\"%d\",\"description\":\"%s\"}",
					first ? "" : ",",
					(int)codecs[i].id, desc ? desc : "Unknown");
			first = false;
		}
		fputc(']', r);

		return fclose(r) ? -errno : 0;
	} else if (spa_streq(message, "get-codec")) {
		if (active == SPA_ID_INVALID)
			*response = strdup("null");
		else
			*response = spa_aprintf("\"%d\"", (int)codecs[active].id);
		return *response ? 0 : -ENOMEM;
	}

	return -ENOSYS;
}

static int core_object_message_handler(struct pw_manager *m, struct pw_manager_object *o, const char *message, const char *params, char **response)
{
	pw_log_debug(": core %p object message:'%s' params:'%s'", o, message, params);

	if (spa_streq(message, "list-handlers")) {
		FILE *r;
		size_t size;
		bool first = true;

		r = open_memstream(response, &size);
		if (r == NULL)
			return -errno;

		fputc('[', r);
		spa_list_for_each(o, &m->object_list, link) {
			if (o->message_object_path) {
				fprintf(r, "%s{\"name\":\"%s\",\"description\":\"%s\"}",
						first ? "" : ",",
						o->message_object_path, o->type);
				first = false;
			}
		}
		fputc(']', r);
		return fclose(r) ? -errno : 0;
	}

	return -ENOSYS;
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
