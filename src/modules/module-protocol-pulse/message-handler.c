static int bluez_card_object_message_handler(struct pw_manager *m, struct pw_manager_object *o, const char *message, const char *params, char **response)
{
	struct card_info card_info = CARD_INFO_INIT;
	uint32_t n_profiles;
	struct profile_info *profile_info;
	const char *prefix;
	uint32_t prefix_len;

	/*
	 * XXX: We just list/switch profiles here, until codec is a separate
	 * XXX: device param.
	 */

	pw_log_debug(NAME "bluez-card %p object message:'%s' params:'%s'", o, message, params);

	collect_card_info(o, &card_info);
	profile_info = alloca(card_info.n_profiles * sizeof(*profile_info));
	n_profiles = collect_profile_info(o, &card_info, profile_info);

	if (card_info.active_profile_name && strstr(card_info.active_profile_name, "headset-head-unit") != NULL)
		prefix = "headset-head-unit-";
	else
		prefix = "a2dp-sink-";
	prefix_len = strlen(prefix);

	if (strcmp(message, "switch-codec") == 0) {
		uint32_t i;
		uint32_t profile_id = SPA_ID_INVALID;
		regex_t re;
		regmatch_t matches[2];
		const char *str;
		uint32_t str_len;

		/* Parse args */
		if (params == NULL)
			return -EINVAL;
		if (regcomp(&re, "[:space:]*{\\(.*\\)}[:space:]*", 0) != 0)
			return -EIO;
		if (regexec(&re, params, SPA_N_ELEMENTS(matches), matches, 0) != 0) {
			regfree(&re);
			return -EINVAL;
		}
		regfree(&re);

		str = params + matches[1].rm_so;
		str_len = matches[1].rm_eo - matches[1].rm_so;

		/* Find profile corresponding to selected codec */
		for (i = 0; i < n_profiles; ++i) {
			if (strncmp(profile_info[i].name, prefix, prefix_len) != 0)
				continue;
			if (strncmp(profile_info[i].name + prefix_len, str, str_len) == 0 &&
			    strlen(profile_info[i].name) == prefix_len + str_len) {
				profile_id = profile_info[i].id;
				goto found;
			}
		}

	found:
		if (profile_id != SPA_ID_INVALID) {
			char buf[1024];
			struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

			/* Switch profile */
			pw_device_set_param((struct pw_device*)o->proxy,
					SPA_PARAM_Profile, 0,
					spa_pod_builder_add_object(&b,
						SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
						SPA_PARAM_PROFILE_index, SPA_POD_Int(profile_id),
						SPA_PARAM_PROFILE_save, SPA_POD_Bool(true)));
			return 0;
		} else {
			return -EINVAL;
		}
	} else if (strcmp(message, "list-codecs") == 0) {
		uint32_t i;
		const char *p;
		FILE *r;
		size_t size;
		regex_t re;
		regmatch_t matches[2];
		bool found = false;

		r = open_memstream(response, &size);
		if (r == NULL)
			return -ENOMEM;

		if (regcomp(&re, "codec \\(.*\\))", 0) != 0)
			return -ENOMEM;

		fputc('{', r);

		for (i = 0; i < n_profiles; ++i) {
			if (strncmp(profile_info[i].name, prefix, prefix_len) != 0)
				continue;

			found = true;
			fputc('{', r);
			fprintf(r, "{%s}", profile_info[i].name + prefix_len);

			/* Parse codec name from description */
			p = profile_info[i].description;
			if (regexec(&re, p, SPA_N_ELEMENTS(matches), matches, 0) == 0) {
				fputc('{', r);
				fwrite(p + matches[1].rm_so, 1, matches[1].rm_eo - matches[1].rm_so, r);
				fputc('}', r);
			} else {
				fprintf(r, "{%s}", p);
			}

			fputc('}', r);
		}

		fputc('}', r);

		regfree(&re);

		if (!found) {
			fclose(r);
			free(*response);
			*response = NULL;
			return -ENOSYS;
		}

		return fclose(r) ? -errno : 0;
	} else if (strcmp(message, "get-codec") == 0) {
		const char *name = "none";
		FILE *r;
		size_t size;
		struct pw_manager_object *obj;

		r = open_memstream(response, &size);
		if (r == NULL)
			return -ENOMEM;

		/* Look up from nodes */
		spa_list_for_each(obj, &m->object_list, link) {
			const char *str;
			uint32_t card_id = SPA_ID_INVALID;
			struct pw_node_info *info;

			if (obj->creating || obj->removing)
				continue;
			if (!object_is_sink(obj) && !object_is_source_or_monitor(obj))
				continue;
			if ((info = obj->info) == NULL || info->props == NULL)
				continue;
			if ((str = spa_dict_lookup(info->props, PW_KEY_DEVICE_ID)) != NULL)
				card_id = (uint32_t)atoi(str);
			if (card_id != o->id)
				continue;
			str = spa_dict_lookup(info->props, "api.bluez5.codec");
			if (str) {
				name = str;
				break;
			}
		}

		fprintf(r, "{%s}", name);
		return fclose(r) ? -errno : 0;
	}

	return -ENOSYS;
}

static int core_object_message_handler(struct pw_manager *m, struct pw_manager_object *o, const char *message, const char *params, char **response)
{
	pw_log_debug(NAME "core %p object message:'%s' params:'%s'", o, message, params);

	if (strcmp(message, "list-handlers") == 0) {
		FILE *r;
		size_t size;

		r = open_memstream(response, &size);
		if (r == NULL)
			return -ENOMEM;

		fputc('{', r);
		spa_list_for_each(o, &m->object_list, link) {
			if (o->message_object_path)
				fprintf(r, "{{%s}{%s}}", o->message_object_path, o->type);
		}
		fputc('}', r);
		return fclose(r) ? -errno : 0;
	}

	return -ENOSYS;
}

static void register_object_message_handlers(struct pw_manager_object *o)
{
	const char *str;

	if (o->id == 0) {
		free(o->message_object_path);
		o->message_object_path = strdup("/core");
		o->message_handler = core_object_message_handler;
		return;
	}

	if (object_is_card(o) && o->props != NULL &&
	    (str = pw_properties_get(o->props, PW_KEY_DEVICE_API)) != NULL &&
	    strcmp(str, "bluez5") == 0) {
		str = pw_properties_get(o->props, PW_KEY_DEVICE_NAME);
		if (str) {
			free(o->message_object_path);
			o->message_object_path = spa_aprintf("/card/%s/bluez", str);
			o->message_handler = bluez_card_object_message_handler;
		}
		return;
	}
}
