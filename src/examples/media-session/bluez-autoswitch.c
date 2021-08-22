/* PipeWire
 *
 * Copyright © 2021 Pauli Virtanen
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

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include "config.h"

#include <spa/utils/hook.h>
#include <spa/utils/result.h>
#include <spa/utils/json.h>
#include <spa/utils/string.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>

#include "pipewire/pipewire.h"
#include "pipewire/extensions/metadata.h"

#include "media-session.h"

/** \page page_media_session_module_bluez_autoswitch Media Session Module: BlueZ Auto-Switch
 *
 * Switch profiles of Bluetooth devices trying to enable an input route,
 * if input streams are active while default output is directed to
 * the device. Profiles are restored once there are no active input streams.
 *
 * Not all input streams are considered, with behavior depending on
 * configuration file settings.
 */

#define NAME		"bluez-autoswitch"
#define SESSION_KEY	"bluez-autoswitch"

#define RESTORE_DELAY_SEC		3

#define DEFAULT_AUDIO_SINK_KEY		"default.audio.sink"

struct impl {
	struct sm_media_session *session;
	struct spa_hook listener;

	struct spa_hook meta_listener;

	unsigned int record_count;
	unsigned int communication_count;

	struct pw_context *context;
	struct spa_source *restore_timeout;

	char *default_sink;

	struct pw_properties *properties;
	bool switched;
};

struct node {
	struct sm_node *obj;
	struct impl *impl;
	struct spa_hook listener;
	unsigned char active:1;
	unsigned char communication:1;
};

struct find_data {
	const char *type;
	const char *name;
	uint32_t id;
	struct sm_object *obj;
};

static int find_check(void *data, struct sm_object *object)
{
	struct find_data *d = data;

	if (!spa_streq(object->type, d->type) || !object->props)
		return 0;

	if (d->id != SPA_ID_INVALID && d->id == object->id) {
		d->obj = object;
		return 1;
	}

	if (d->name != NULL &&
			spa_streq(pw_properties_get(object->props, PW_KEY_NODE_NAME), d->name)) {
		d->obj = object;
		return 1;
	}

	return 0;
}

static struct sm_object *find_by_name(struct impl *impl, const char *type, const char *name)
{
	struct find_data d = { type, name, SPA_ID_INVALID, NULL };
	if (name != NULL)
		sm_media_session_for_each_object(impl->session, find_check, &d);
	return d.obj;
}

static struct sm_object *find_by_id(struct impl *impl, const char *type, uint32_t id)
{
	struct find_data d = { type, NULL, id, NULL };
	if (id != SPA_ID_INVALID)
		sm_media_session_for_each_object(impl->session, find_check, &d);
	return d.obj;
}

static struct sm_device *find_default_output_device(struct impl *impl)
{
	struct sm_object *obj;
	const char *str;
	uint32_t device_id;

	if ((obj = find_by_name(impl, PW_TYPE_INTERFACE_Node, impl->default_sink)) == NULL ||
			!obj->props)
		return NULL;

	if ((str = pw_properties_get(obj->props, PW_KEY_DEVICE_ID)) == NULL)
		return NULL;

	if (!spa_atou32(str, &device_id, 10) ||
			(obj = find_by_id(impl, PW_TYPE_INTERFACE_Device, device_id)) == NULL)
		return NULL;

	if (!spa_streq(obj->type, PW_TYPE_INTERFACE_Device) || !obj->props)
		return NULL;

	return SPA_CONTAINER_OF(obj, struct sm_device, obj);
}

static int find_profile(struct sm_device *dev, int32_t index, const char *name,
		int32_t *out_index, const char **out_name, int32_t *out_priority)
{
       struct sm_param *p;

       spa_list_for_each(p, &dev->param_list, link) {
	       int32_t idx;
	       int32_t prio = 0;
	       const char *str;

	       if (p->id != SPA_PARAM_EnumProfile || !p->param)
		       continue;

	       if (spa_pod_parse_object(p->param,
					       SPA_TYPE_OBJECT_ParamProfile, NULL,
					       SPA_PARAM_PROFILE_index, SPA_POD_Int(&idx),
					       SPA_PARAM_PROFILE_name, SPA_POD_String(&str),
					       SPA_PARAM_PROFILE_priority, SPA_POD_OPT_Int(&prio)) < 0)
		       continue;

	       if ((index < 0 || idx == index) && (name == NULL || spa_streq(str, name))) {
		       if (out_index)
			       *out_index = idx;
		       if (out_name)
			       *out_name = str;
		       if (out_priority)
			       *out_priority = prio;
		       return 0;
	       }
       }

       return -ENOENT;
}

static int set_profile(struct sm_device *dev, const char *profile_name)
{
       char buf[1024];
       struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
       int32_t index = -1;
       int ret;

       if (!profile_name)
	       return -EINVAL;

       if (!dev->obj.proxy)
	       return -ENOENT;

       if ((ret = find_profile(dev, -1, profile_name, &index, NULL, NULL)) < 0)
	       return ret;

       pw_log_info(NAME ": switching device %d to profile %s", dev->obj.id, profile_name);

       return pw_device_set_param((struct pw_device *)dev->obj.proxy,
		       SPA_PARAM_Profile, 0,
		       spa_pod_builder_add_object(&b,
				       SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
				       SPA_PARAM_PROFILE_index,	  SPA_POD_Int(index)));
}

const char *get_saved_profile(struct impl *impl, const char *dev_name)
{
	char saved_profile_key[512];
	spa_scnprintf(saved_profile_key, sizeof(saved_profile_key), "%s:profile", dev_name);
	return pw_properties_get(impl->properties, saved_profile_key);
}

void set_saved_profile(struct impl *impl, const char *dev_name, const char *profile_name)
{
	char saved_profile_key[512];
	spa_scnprintf(saved_profile_key, sizeof(saved_profile_key), "%s:profile", dev_name);
	pw_properties_set(impl->properties, saved_profile_key, profile_name);
}

bool get_pending_save(struct impl *impl, const char *dev_name)
{
	char saved_profile_key[512];
	spa_scnprintf(saved_profile_key, sizeof(saved_profile_key), "%s:pending-save", dev_name);
	return spa_atob(pw_properties_get(impl->properties, saved_profile_key));
}

void set_pending_save(struct impl *impl, const char *dev_name, bool pending)
{
	char saved_profile_key[512];
	spa_scnprintf(saved_profile_key, sizeof(saved_profile_key), "%s:pending-save", dev_name);
	pw_properties_set(impl->properties, saved_profile_key, pending ? "true" : NULL);
}

const char *get_saved_headset_profile(struct impl *impl, const char *dev_name)
{
	char saved_profile_key[512];
	spa_scnprintf(saved_profile_key, sizeof(saved_profile_key), "%s:headset-profile", dev_name);
	return pw_properties_get(impl->properties, saved_profile_key);
}

void set_saved_headset_profile(struct impl *impl, const char *dev_name, const char *profile_name)
{
	char saved_profile_key[512];
	spa_scnprintf(saved_profile_key, sizeof(saved_profile_key), "%s:headset-profile", dev_name);
	pw_properties_set(impl->properties, saved_profile_key, profile_name);
}

static int do_restore_profile(void *data, struct sm_object *obj)
{
	struct impl *impl = data;
	struct sm_device *dev;
	const char *dev_name;
	const char *profile_name;
	struct sm_param *p;

	/* Find old profile and restore it */

	if (!spa_streq(obj->type, PW_TYPE_INTERFACE_Device) || !obj->props)
		goto next;
	if ((dev_name = pw_properties_get(obj->props, PW_KEY_DEVICE_NAME)) == NULL)
		goto next;
	if ((profile_name = get_saved_profile(impl, dev_name)) == NULL)
		goto next;

	dev = SPA_CONTAINER_OF(obj, struct sm_device, obj);

	/* Save user-selected headset profile */
	if (get_pending_save(impl, dev_name)) {
		spa_list_for_each(p, &dev->param_list, link) {
			const char *name;
			if (p->id != SPA_PARAM_Profile || !p->param)
				continue;
			if (spa_pod_parse_object(p->param,
							SPA_TYPE_OBJECT_ParamProfile, NULL,
							SPA_PARAM_PROFILE_name, SPA_POD_String(&name)) < 0)
				continue;
			set_saved_headset_profile(impl, dev_name, name);
			set_pending_save(impl, dev_name, false);
			break;
		}
	}

	/* Restore previous profile */
	set_profile(dev, profile_name);
	set_saved_profile(impl, dev_name, NULL);

next:
	return 0;
}

static void remove_restore_timeout(struct impl *impl)
{
	struct pw_loop *main_loop = pw_context_get_main_loop(impl->context);
	if (impl->restore_timeout) {
		pw_loop_destroy_source(main_loop, impl->restore_timeout);
		impl->restore_timeout = NULL;
	}
}

static void restore_timeout(void *data, uint64_t expirations)
{
	struct impl *impl = data;
	int res;

	remove_restore_timeout(impl);

	/*
	 * Switching profiles may make applications remove existing input streams
	 * and create new ones. To avoid getting into a rapidly spinning loop,
	 * restoring profiles has to be done with a timeout.
	 */

	/* Restore previous profiles to devices */
	sm_media_session_for_each_object(impl->session, do_restore_profile, impl);
	if ((res = sm_media_session_save_state(impl->session, SESSION_KEY, impl->properties)) < 0)
		pw_log_error("can't save "SESSION_KEY" state: %s", spa_strerror(res));

	impl->switched = false;
}

static void add_restore_timeout(struct impl *impl)
{
	struct timespec value;
	struct pw_loop *main_loop = pw_context_get_main_loop(impl->context);

	if (!impl->switched)
		return;

	if (impl->restore_timeout == NULL)
		impl->restore_timeout = pw_loop_add_timer(main_loop, restore_timeout, impl);

	value.tv_sec = RESTORE_DELAY_SEC;
	value.tv_nsec = 0;
	pw_loop_update_timer(main_loop, impl->restore_timeout, &value, NULL, false);
}

static void switch_profile_if_needed(struct impl *impl)
{
	struct sm_device *dev;
	struct sm_param *p;
	int headset_profile_priority = -1;
	const char *current_profile_name = NULL;
	const char *headset_profile_name = NULL;
	enum spa_direction direction;
	const char *dev_name;
	const char *saved_headset_profile = NULL;
	const char *str;
	int res;

	if (impl->record_count == 0)
		goto inactive;

	pw_log_debug(NAME ": considering switching device profiles");

	if ((dev = find_default_output_device(impl)) == NULL)
		goto inactive;

	/* Handle only bluez devices */
	if (!spa_streq(pw_properties_get(dev->obj.props, PW_KEY_DEVICE_API), "bluez5"))
		goto inactive;
	if ((dev_name = pw_properties_get(dev->obj.props, PW_KEY_DEVICE_NAME)) == NULL)
		goto inactive;

	/* Check autoswitch setting (default: role) */
	if ((str = pw_properties_get(dev->obj.props, "bluez5.autoswitch-profile")) == NULL)
		str = "role";
	if (spa_atob(str)) {
		/* ok */
	} else if (spa_streq(str, "role")) {
		if (impl->communication_count == 0)
			goto inactive;
	} else {
		goto inactive;
	}

	/* BT microphone is wanted */

	remove_restore_timeout(impl);

	if (get_saved_profile(impl, dev_name)) {
		/* We already switched this device */
		return;
	}

	/* Find saved headset profile, if any */
	saved_headset_profile = get_saved_headset_profile(impl, dev_name);

	/* Find current profile, and highest-priority profile with input route */
	spa_list_for_each(p, &dev->param_list, link) {
		const char *name;
		int32_t idx;
		struct spa_pod *profiles = NULL;

		if (!p->param)
			continue;

		switch (p->id) {
		case SPA_PARAM_Route:
		case SPA_PARAM_EnumRoute:
			if (spa_pod_parse_object(p->param,
							SPA_TYPE_OBJECT_ParamRoute, NULL,
							SPA_PARAM_ROUTE_direction, SPA_POD_Id(&direction),
							SPA_PARAM_ROUTE_profiles, SPA_POD_OPT_Pod(&profiles)) < 0)
				continue;
			if (direction != SPA_DIRECTION_INPUT)
				continue;

			if (p->id == SPA_PARAM_Route) {
				/* There's already an input route, no need to switch */
				return;
			} else if (profiles) {
				/* Take highest-priority (or first) profile in the input route */
				uint32_t *vals, n_vals, n;
				vals = spa_pod_get_array(profiles, &n_vals);
				if (vals == NULL)
					continue;
				for (n = 0; n < n_vals; ++n) {
					int32_t i = vals[n];
					int32_t prio = -1;
					const char *name = NULL;
					if (find_profile(dev, i, NULL, NULL, &name, &prio) < 0)
						continue;
					if (headset_profile_priority < prio) {
						headset_profile_priority = prio;
						headset_profile_name = name;
					}
				}
			}
			break;
		case SPA_PARAM_Profile:
		case SPA_PARAM_EnumProfile:
			if (spa_pod_parse_object(p->param,
							SPA_TYPE_OBJECT_ParamProfile, NULL,
							SPA_PARAM_PROFILE_index, SPA_POD_Int(&idx),
							SPA_PARAM_PROFILE_name, SPA_POD_String(&name)) < 0)
				continue;
			if (p->id == SPA_PARAM_Profile) {
				current_profile_name = name;
			} else if (spa_streq(name, saved_headset_profile)) {
				/* Saved headset profile takes priority */
				headset_profile_priority = INT32_MAX;
				headset_profile_name = name;
			}
			break;
		}
	}

	if (set_profile(dev, headset_profile_name) < 0)
		return;

	set_saved_profile(impl, dev_name, current_profile_name);
	set_pending_save(impl, dev_name, true);

	if ((res = sm_media_session_save_state(impl->session, SESSION_KEY, impl->properties)) < 0)
		pw_log_error("can't save "SESSION_KEY" state: %s", spa_strerror(res));

	impl->switched = true;
	return;

inactive:
	add_restore_timeout(impl);
	return;
}

static void change_node_state(struct node *node, bool active, bool communication)
{
	bool need_switch = false;
	struct impl *impl = node->impl;

	if (node->active != active) {
		impl->record_count += active ? 1 : -1;
		node->active = active;
		need_switch = true;
	}

	if (node->communication != communication) {
		impl->communication_count += communication ? 1 : -1;
		node->communication = communication;
		need_switch = true;
	}

	if (need_switch)
		switch_profile_if_needed(impl);
}

static void check_node(struct node *node)
{
	const char *str;
	bool communication = false;

	if (!node->obj || !node->obj->obj.props || !node->obj->info || !node->obj->info->props)
		goto inactive;

	if (!spa_streq(pw_properties_get(node->obj->obj.props, PW_KEY_MEDIA_CLASS), "Stream/Input/Audio"))
		goto inactive;

	if ((str = spa_dict_lookup(node->obj->info->props, PW_KEY_NODE_AUTOCONNECT)) == NULL ||
			!spa_atob(str))
		goto inactive;

	if ((str = spa_dict_lookup(node->obj->info->props, PW_KEY_STREAM_MONITOR)) != NULL &&
			spa_atob(str))
		goto inactive;

	if (spa_streq(pw_properties_get(node->obj->obj.props, PW_KEY_MEDIA_ROLE), "Communication"))
		communication = true;

	change_node_state(node, true, communication);
	return;

inactive:
	change_node_state(node, false, false);
}

static void object_update(void *data)
{
	struct node *node = data;
	if (node->obj->obj.avail & (SM_NODE_CHANGE_MASK_PARAMS | SM_NODE_CHANGE_MASK_INFO))
		check_node(node);
}

static const struct sm_object_events object_events = {
	SM_VERSION_OBJECT_EVENTS,
	.update = object_update
};

static void session_create(void *data, struct sm_object *object)
{
	struct impl *impl = data;
	struct node *node;

	if (spa_streq(object->type, PW_TYPE_INTERFACE_Device) && object->props) {
		const char *str;

		if ((str = pw_properties_get(object->props, PW_KEY_DEVICE_NAME)) != NULL)
			set_pending_save(impl, str, false);

		impl->switched = true;
		add_restore_timeout(impl);
		return;
	}

	if (!spa_streq(object->type, PW_TYPE_INTERFACE_Node) || !object->props)
		return;

	if (!spa_streq(pw_properties_get(object->props, PW_KEY_MEDIA_CLASS), "Stream/Input/Audio"))
		return;

	pw_log_debug(NAME ": input stream %d added", object->id);

	node = sm_object_add_data(object, SESSION_KEY, sizeof(struct node));
	if (!node->obj) {
		node->obj = (struct sm_node *)object;
		node->impl = impl;
		sm_object_add_listener(&node->obj->obj, &node->listener, &object_events, node);
	}

	check_node(node);
}

static void session_remove(void *data, struct sm_object *object)
{
	struct node *node;

	if (!spa_streq(object->type, PW_TYPE_INTERFACE_Node))
		return;

	if ((node = sm_object_get_data(object, SESSION_KEY)) == NULL)
		return;

	change_node_state(node, false, false);

	if (node->obj) {
		pw_log_debug(NAME ": input stream %d removed", object->id);
		spa_hook_remove(&node->listener);
		node->obj = NULL;
	}
}

static void session_destroy(void *data)
{
	struct impl *impl = data;
	remove_restore_timeout(impl);
	spa_hook_remove(&impl->listener);
	if (impl->session->metadata)
		spa_hook_remove(&impl->meta_listener);
	pw_properties_free(impl->properties);
	free(impl->default_sink);
	free(impl);
}

static const struct sm_media_session_events session_events = {
	SM_VERSION_MEDIA_SESSION_EVENTS,
	.create = session_create,
	.remove = session_remove,
	.destroy = session_destroy,
};

static int json_object_find(const char *obj, const char *key, char *value, size_t len)
{
	struct spa_json it[2];
	const char *v;
	char k[128];

	spa_json_init(&it[0], obj, strlen(obj));
	if (spa_json_enter_object(&it[0], &it[1]) <= 0)
		return -EINVAL;

	while (spa_json_get_string(&it[1], k, sizeof(k)-1) > 0) {
		if (spa_streq(k, key)) {
			if (spa_json_get_string(&it[1], value, len) <= 0)
				continue;
			return 0;
		} else {
			if (spa_json_next(&it[1], &v) <= 0)
				break;
		}
	}
	return -ENOENT;
}

static int metadata_property(void *object, uint32_t subject,
		const char *key, const char *type, const char *value)
{
	struct impl *impl = object;
	if (subject == PW_ID_CORE) {
		char name[1024];

		if (key && value && json_object_find(value, "name", name, sizeof(name)) < 0)
			return 0;

		if (key == NULL || spa_streq(key, DEFAULT_AUDIO_SINK_KEY)) {
			free(impl->default_sink);
			impl->default_sink = (key && value) ? strdup(name) : NULL;

			/* Switch also when default output changes */
			switch_profile_if_needed(impl);
		}
	}
	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_property,
};

int sm_bluez5_autoswitch_start(struct sm_media_session *session)
{
	struct impl *impl;
	int res;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->session = session;
	impl->context = session->context;

	impl->properties = pw_properties_new(NULL, NULL);
	if (impl->properties == NULL) {
		free(impl);
		return -ENOMEM;
	}

	if ((res = sm_media_session_load_state(impl->session, SESSION_KEY, impl->properties)) < 0)
		pw_log_info("can't load "SESSION_KEY" state: %s", spa_strerror(res));

	sm_media_session_add_listener(impl->session, &impl->listener, &session_events, impl);

	if (session->metadata) {
		pw_metadata_add_listener(session->metadata,
				&impl->meta_listener,
				&metadata_events, impl);
	}

	return 0;
}
