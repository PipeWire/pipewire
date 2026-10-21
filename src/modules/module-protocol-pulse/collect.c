/* PipeWire */
/* SPDX-FileCopyrightText: Copyright © 2020 Wim Taymans */
/* SPDX-License-Identifier: MIT */

#include <spa/param/props.h>
#include <spa/pod/builder.h>
#include <spa/pod/parser.h>
#include <spa/utils/string.h>

#include <spa/param/audio/format-utils.h>

#include <pipewire/pipewire.h>

#include "collect.h"
#include "defs.h"
#include "log.h"
#include "manager.h"

void select_best(struct selector *s, struct pw_manager_object *o)
{
	int32_t prio = 0;

	if (pw_properties_fetch_int32(o->props, PW_KEY_PRIORITY_SESSION, &prio) == 0) {
		if (s->best == NULL || prio > s->score) {
			s->best = o;
			s->score = prio;
		}
	}
}

static int select_object_cb(void *data, struct pw_manager_object *o)
{
	struct selector *s = data;
	const char *str;

	if (s->type != NULL && !s->type(o))
		return 0;
	if (o->id == s->id)
		goto found;
	if (o->index == s->index)
		goto found;
	if (s->accumulate)
		s->accumulate(s, o);
	if (s->key != NULL && s->value != NULL &&
	    (str = pw_properties_get(o->props, s->key)) != NULL &&
	    spa_streq(str, s->value))
		goto found;
	if (s->value != NULL && (uint32_t)atoi(s->value) == o->index)
		goto found;
	return 0;
found:
	s->best = o;
	return 1;
}

struct pw_manager_object *select_object(struct pw_manager *m, struct selector *s)
{
	pw_manager_for_each_object(m, select_object_cb, s);
	return s->best;
}

uint32_t id_to_index(struct pw_manager *m, uint32_t id)
{
	return pw_manager_id_to_index(m, id);
}

struct collect_linked_data {
	struct pw_manager *m;
	uint32_t id;
	enum pw_direction direction;
	struct pw_manager_object *object;
};

static int collect_is_linked_cb(void *data, struct pw_manager_object *o)
{
	struct collect_linked_data *d = data;
	uint32_t in_node, out_node;

	if (!pw_manager_object_is_link(o))
		return 0;

	if (pw_properties_fetch_uint32(o->props, PW_KEY_LINK_OUTPUT_NODE, &out_node) != 0 ||
	    pw_properties_fetch_uint32(o->props, PW_KEY_LINK_INPUT_NODE, &in_node) != 0)
		return 0;

	if ((d->direction == PW_DIRECTION_OUTPUT && d->id == out_node) ||
	    (d->direction == PW_DIRECTION_INPUT && d->id == in_node))
		return 1;

	return 0;
}

static bool collect_is_linked(struct pw_manager *m, uint32_t id, enum pw_direction direction)
{
	struct collect_linked_data d = { m, id, direction };
	if (pw_manager_for_each_object(m, collect_is_linked_cb, &d) == 1)
		return true;
	return false;
}

struct pw_manager_object *find_peer_for_link(struct pw_manager *m,
		struct pw_manager_object *o, uint32_t id, enum pw_direction direction)
{
	struct pw_manager_object *p;
	uint32_t in_node, out_node;

	if (pw_properties_fetch_uint32(o->props, PW_KEY_LINK_OUTPUT_NODE, &out_node) != 0 ||
	    pw_properties_fetch_uint32(o->props, PW_KEY_LINK_INPUT_NODE, &in_node) != 0)
		return NULL;

	if (direction == PW_DIRECTION_OUTPUT && id == out_node) {
		struct selector sel = { .id = in_node, .type = pw_manager_object_is_sink, };
		if ((p = select_object(m, &sel)) != NULL)
			return p;
	}
	if (direction == PW_DIRECTION_INPUT && id == in_node) {
		struct selector sel = { .id = out_node, .type = pw_manager_object_is_recordable, };
		if ((p = select_object(m, &sel)) != NULL)
			return p;
	}
	return NULL;
}

static int find_linked_cb(void *data, struct pw_manager_object *o)
{
	struct collect_linked_data *d = data;
	if (!pw_manager_object_is_link(o))
		return 0;
	if ((d->object = find_peer_for_link(d->m, o, d->id, d->direction)) != NULL)
		return 1;
	return 0;
}

struct pw_manager_object *find_linked(struct pw_manager *m, uint32_t id, enum pw_direction direction)
{
	struct collect_linked_data d = { m, id, direction, NULL };
	pw_manager_for_each_object(m, find_linked_cb, &d);
	return d.object;
}

void collect_card_info(struct pw_manager_object *card, struct card_info *info)
{
	struct pw_manager_param *p;
	struct spa_list *param_list = pw_manager_object_get_param_list(card);

	spa_list_for_each(p, param_list, link) {
		switch (p->id) {
		case SPA_PARAM_EnumProfile:
			info->n_profiles++;
			break;
		case SPA_PARAM_Profile:
			spa_pod_parse_object(p->param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&info->active_profile));
			break;
		case SPA_PARAM_EnumRoute:
			info->n_ports++;
			break;
		}
	}
}

uint32_t collect_profile_info(struct pw_manager_object *card, struct card_info *card_info,
			      struct profile_info *profile_info)
{
	struct spa_list *param_list = pw_manager_object_get_param_list(card);
	struct pw_manager_param *p;
	struct profile_info *pi;
	uint32_t n;

	n = 0;
	spa_list_for_each(p, param_list, link) {
		struct spa_pod *classes = NULL;

		if (p->id != SPA_PARAM_EnumProfile)
			continue;

		pi = &profile_info[n];
		spa_zero(*pi);

		if (spa_pod_parse_object(p->param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&pi->index),
				SPA_PARAM_PROFILE_name,  SPA_POD_String(&pi->name),
				SPA_PARAM_PROFILE_description,  SPA_POD_OPT_String(&pi->description),
				SPA_PARAM_PROFILE_priority,  SPA_POD_OPT_Int(&pi->priority),
				SPA_PARAM_PROFILE_available,  SPA_POD_OPT_Id(&pi->available),
				SPA_PARAM_PROFILE_classes,  SPA_POD_OPT_PodStruct(&classes)) < 0) {
			continue;
		}
		if (pi->description == NULL)
			pi->description = pi->name;
		if (pi->index == card_info->active_profile)
			card_info->active_profile_name = pi->name;

		if (classes != NULL) {
			struct spa_pod *iter;

			SPA_POD_STRUCT_FOREACH(classes, iter) {
				struct spa_pod_parser prs;
				char *class;
				uint32_t count;

				spa_pod_parser_pod(&prs, iter);
				if (spa_pod_parser_get_struct(&prs,
						SPA_POD_String(&class),
						SPA_POD_Int(&count)) < 0)
					continue;

				if (spa_streq(class, "Audio/Sink"))
					pi->n_sinks += count;
				else if (spa_streq(class, "Audio/Source"))
					pi->n_sources += count;
			}
		}
		n++;
	}
	if (card_info->active_profile_name == NULL && n > 0)
		card_info->active_profile_name = profile_info[0].name;

	return n;
}

uint32_t find_profile_index(struct pw_manager_object *card, const char *name)
{
	struct pw_manager_param *p;
	struct spa_list *param_list = pw_manager_object_get_param_list(card);

	spa_list_for_each(p, param_list, link) {
		uint32_t index;
		const char *test_name;

		if (p->id != SPA_PARAM_EnumProfile)
			continue;

		if (spa_pod_parse_object(p->param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&index),
				SPA_PARAM_PROFILE_name,  SPA_POD_String(&test_name)) < 0)
			continue;

		if (spa_streq(test_name, name))
			return index;

	}
	return SPA_ID_INVALID;
}

static void collect_device_info(struct pw_manager_object *device, struct pw_manager_object *card,
			 struct device_info *dev_info, bool monitor, struct defs *defs)
{
	struct pw_manager_param *p;
	uint32_t enum_channels = 0;
	struct spa_list *device_param_list = pw_manager_object_get_param_list(device);

	if (card) {
		struct spa_list *card_param_list = pw_manager_object_get_param_list(card);

		spa_list_for_each(p, card_param_list, link) {
			uint32_t index, dev;
			struct spa_pod *props;

			if (p->id != SPA_PARAM_Route)
				continue;

			if (spa_pod_parse_object(p->param,
					SPA_TYPE_OBJECT_ParamRoute, NULL,
					SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
					SPA_PARAM_ROUTE_device,  SPA_POD_Int(&dev),
					SPA_PARAM_ROUTE_props,  SPA_POD_OPT_PodObject(&props)) < 0)
				continue;
			if (dev != dev_info->device)
				continue;
			dev_info->active_port = index;
			if (props && !monitor) {
				volume_parse_param(props, &dev_info->volume_info, monitor);
				dev_info->have_volume = true;
			}
		}
	}

	spa_list_for_each(p, device_param_list, link) {
		switch (p->id) {
		case SPA_PARAM_EnumFormat:
		{
			struct spa_pod *to_free = NULL, *c = p->param;
			struct sample_spec ss = dev_info->ss;
			struct channel_map map = dev_info->map;

			if (!spa_pod_is_fixated(c)) {
				to_free = spa_pod_copy(c);
				if (to_free == NULL)
					break;
				spa_pod_fixate(to_free);
				c = to_free;
			}
			if (format_parse_param(c, true, &ss, &map,
					&defs->sample_spec, &defs->channel_map) >= 0 &&
			    ss.channels > enum_channels) {
				dev_info->ss = ss;
				dev_info->map = map;
				enum_channels = ss.channels;
			}
			free(to_free);
			break;
		}
		case SPA_PARAM_Format:
			format_parse_param(p->param, true, &dev_info->ss, &dev_info->map,
					NULL, NULL);
			break;

		case SPA_PARAM_Props:
			if (!dev_info->have_volume) {
				volume_parse_param(p->param, &dev_info->volume_info, monitor);
				dev_info->have_volume = true;
			}
			dev_info->have_iec958codecs = spa_pod_find_prop(p->param,
					NULL, SPA_PROP_iec958Codecs) != NULL;
			break;
		}
	}
	if (dev_info->ss.channels != dev_info->map.channels)
		dev_info->ss.channels = dev_info->map.channels;
	if (dev_info->volume_info.volume.channels != dev_info->map.channels)
		dev_info->volume_info.volume.channels = dev_info->map.channels;
}

static void update_device_info(struct pw_manager *manager, struct pw_manager_object *o,
		enum pw_direction direction, bool monitor, struct defs *defs, bool stream)
{
	const char *str;
	const char *key = monitor ? "device.info.monitor" : "device.info";
	struct pw_manager_object *card = NULL;
	struct device_info *dev_info, di;

	di = DEVICE_INFO_INIT(direction);
	if ((str = pw_properties_get(o->props, PW_KEY_DEVICE_ID)) != NULL)
		di.card_id = (uint32_t)atoi(str);
	if ((str = pw_properties_get(o->props, "card.profile.device")) != NULL)
		di.device = (uint32_t)atoi(str);
	if (di.card_id != SPA_ID_INVALID) {
		struct selector sel = { .id = di.card_id, .type = pw_manager_object_is_card, };
		card = select_object(manager, &sel);
	}
	collect_device_info(o, card, &di, monitor, defs);

	di.state = pw_manager_node_state(o);
	/* running sink/source that is not linked is reported as idle */
	if (!stream && di.state == STATE_RUNNING &&
	    !collect_is_linked(manager, o->id, pw_direction_reverse(direction)))
		di.state = STATE_IDLE;

	dev_info = pw_manager_object_get_data(o, key);
	if (dev_info) {
		if (memcmp(dev_info, &di, sizeof(di)) != 0) {
			if (monitor || direction == PW_DIRECTION_INPUT)
				o->change_mask |= PW_MANAGER_OBJECT_FLAG_SOURCE;
			else
				o->change_mask |= PW_MANAGER_OBJECT_FLAG_SINK;
		}
	} else {
		o->change_mask = ~0;
		dev_info = pw_manager_object_add_data(o, key, sizeof(*dev_info));
	}
	if (dev_info != NULL)
		*dev_info = di;
}

void get_device_info(struct pw_manager_object *o, struct device_info *info,
		enum pw_direction direction, bool monitor)
{
	const char *key = monitor ? "device.info.monitor" : "device.info";
	struct device_info *di;
	di = pw_manager_object_get_data(o, key);
	if (di != NULL)
		*info = *di;
	else
		*info = DEVICE_INFO_INIT(direction);
}

static bool array_contains(uint32_t *vals, uint32_t n_vals, uint32_t val)
{
	uint32_t n;
	if (vals == NULL || n_vals == 0)
		return false;
	for (n = 0; n < n_vals; n++)
		if (vals[n] == val)
			return true;
	return false;
}

uint32_t collect_port_info(struct pw_manager_object *card, struct card_info *card_info,
			   struct device_info *dev_info, struct port_info *port_info,
			   const char **active_port_name)
{
	struct spa_list *param_list;
	struct pw_manager_param *p;
	uint32_t n;
	const char *aport_name = NULL;

	if (card == NULL)
		return 0;

	param_list = pw_manager_object_get_param_list(card);

	n = 0;
	spa_list_for_each(p, param_list, link) {
		int32_t *devices = NULL, *profiles = NULL;
		uint32_t devices_size = 0, devices_type = 0, n_devices = 0;
		uint32_t profiles_size = 0, profiles_type = 0, n_profiles = 0;
		struct port_info *pi;

		if (p->id != SPA_PARAM_EnumRoute)
			continue;

		pi = &port_info[n];
		spa_zero(*pi);

		if (spa_pod_parse_object(p->param,
				SPA_TYPE_OBJECT_ParamRoute, NULL,
				SPA_PARAM_ROUTE_index, SPA_POD_Int(&pi->index),
				SPA_PARAM_ROUTE_direction, SPA_POD_Id(&pi->direction),
				SPA_PARAM_ROUTE_name,  SPA_POD_String(&pi->name),
				SPA_PARAM_ROUTE_description,  SPA_POD_OPT_String(&pi->description),
				SPA_PARAM_ROUTE_priority,  SPA_POD_OPT_Int(&pi->priority),
				SPA_PARAM_ROUTE_available,  SPA_POD_OPT_Id(&pi->available),
				SPA_PARAM_ROUTE_info,  SPA_POD_OPT_Pod(&pi->info),
				SPA_PARAM_ROUTE_devices, SPA_POD_OPT_Array(&devices_size,
					&devices_type, &n_devices, &devices),
				SPA_PARAM_ROUTE_profiles, SPA_POD_OPT_Array(&profiles_size,
					&profiles_type, &n_profiles, &profiles)) < 0)
			continue;

		if (pi->description == NULL)
			pi->description = pi->name;
		if (devices && devices_size == sizeof(pi->devices[0]) &&
		    devices_type == SPA_TYPE_Int) {
			pi->devices = (uint32_t*)devices;
			pi->n_devices = n_devices;
		}
		if (profiles && profiles_size == sizeof(pi->profiles[0]) &&
		    profiles_type == SPA_TYPE_Int) {
			pi->profiles = (uint32_t*)profiles;
			pi->n_profiles = n_profiles;
		}

		if (dev_info != NULL) {
			if (pi->direction != dev_info->direction)
				continue;
			if (!array_contains(pi->profiles, pi->n_profiles, card_info->active_profile))
				continue;
			if (!array_contains(pi->devices, pi->n_devices, dev_info->device))
				continue;
			if (pi->index == dev_info->active_port)
				aport_name = pi->name;
		}

		while (pi->info != NULL) {
			struct spa_pod_parser prs;
			struct spa_pod_frame f[1];
			uint32_t n;
			const char *key, *value;

			spa_pod_parser_pod(&prs, pi->info);
			if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
			    spa_pod_parser_get_int(&prs, (int32_t*)&pi->n_props) < 0)
				break;

			for (n = 0; n < pi->n_props; n++) {
				if (spa_pod_parser_get(&prs,
						SPA_POD_String(&key),
						SPA_POD_String(&value),
						NULL) < 0)
					break;
				if (spa_streq(key, "port.availability-group"))
					pi->availability_group = value;
				else if (spa_streq(key, "port.type"))
					pi->type = port_type_value(value);
			}
			spa_pod_parser_pop(&prs, &f[0]);
			break;
		}
		n++;
	}
	if (aport_name == NULL && n > 0)
		aport_name = port_info[0].name;

	if (active_port_name)
		*active_port_name = aport_name;
	return n;
}

uint32_t find_port_index(struct pw_manager_object *card, uint32_t direction, const char *port_name)
{
	struct spa_list *param_list = pw_manager_object_get_param_list(card);
	struct pw_manager_param *p;

	spa_list_for_each(p, param_list, link) {
		uint32_t index, dir;
		const char *name;

		if (p->id != SPA_PARAM_EnumRoute)
			continue;

		if (spa_pod_parse_object(p->param,
				SPA_TYPE_OBJECT_ParamRoute, NULL,
				SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
				SPA_PARAM_ROUTE_direction, SPA_POD_Id(&dir),
				SPA_PARAM_ROUTE_name, SPA_POD_String(&name)) < 0)
			continue;
		if (dir != direction)
			continue;
		if (spa_streq(name, port_name))
			return index;

	}
	return SPA_ID_INVALID;
}

struct spa_dict *collect_props(struct spa_pod *info, struct spa_dict *dict,
			       struct spa_dict_item *items, size_t capacity)
{
	struct spa_pod_parser prs;
	struct spa_pod_frame f[1];
	int32_t n, n_items;

	spa_pod_parser_pod(&prs, info);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_get_int(&prs, &n_items) < 0)
		return NULL;

	if (n_items < 0 || (size_t) n_items > capacity)
		return NULL;

	for (n = 0; n < n_items; n++) {
		if (spa_pod_parser_get(&prs,
				SPA_POD_String(&items[n].key),
				SPA_POD_String(&items[n].value),
				NULL) < 0)
			break;
	}
	spa_pod_parser_pop(&prs, &f[0]);
	*dict = SPA_DICT_INIT(items, n);
	return dict;
}

uint32_t collect_transport_codec_info(struct pw_manager_object *card,
				      struct transport_codec_info *codecs, uint32_t max_codecs,
				      uint32_t *active)
{
	struct spa_list *param_list = pw_manager_object_get_param_list(card);
	struct pw_manager_param *p;
	uint32_t n_codecs = 0;

	*active = SPA_ID_INVALID;

	if (card == NULL)
		return 0;

	spa_list_for_each(p, param_list, link) {
		uint32_t iid;
		const struct spa_pod_choice *type;
		const struct spa_pod_struct *labels;
		struct spa_pod_parser prs;
		struct spa_pod_frame f;
		int32_t *id;
		bool first;

		if (p->id != SPA_PARAM_PropInfo)
			continue;

		if (spa_pod_parse_object(p->param,
						SPA_TYPE_OBJECT_PropInfo, NULL,
						SPA_PROP_INFO_id, SPA_POD_Id(&iid),
						SPA_PROP_INFO_type, SPA_POD_PodChoice(&type),
						SPA_PROP_INFO_labels, SPA_POD_PodStruct(&labels)) < 0)
			continue;

		if (iid != SPA_PROP_bluetoothAudioCodec)
			continue;

		if (type->pod.size < sizeof(struct spa_pod_choice_body) +
		                     2 * sizeof(int32_t) ||
		    type->body.type != SPA_CHOICE_Enum ||
		    type->body.child.type != SPA_TYPE_Int ||
		    type->body.child.size != sizeof(int32_t))
			continue;

		/*
		 * XXX: PropInfo currently uses Int, not Id, in type and labels.
		 */

		/* Codec name list */
		first = true;
		SPA_POD_CHOICE_FOREACH(type, id) {
			if (first) {
				/* Skip default */
				first = false;
				continue;
			}
			if (n_codecs >= max_codecs)
				break;
			codecs[n_codecs++].id = *id;
		}

		/* Codec description list */
		spa_pod_parser_pod(&prs, (struct spa_pod *)labels);
		if (spa_pod_parser_push_struct(&prs, &f) < 0)
			continue;

		while (1) {
			int32_t id;
			const char *desc;
			uint32_t j;

			if (spa_pod_parser_get_int(&prs, &id) < 0 ||
					spa_pod_parser_get_string(&prs, &desc) < 0)
				break;

			for (j = 0; j < n_codecs; ++j) {
				if (codecs[j].id == (uint32_t)id)
					codecs[j].description = desc;
			}
		}
	}

	/* Active codec */
	spa_list_for_each(p, param_list, link) {
		uint32_t j;
		uint32_t id;

		if (p->id != SPA_PARAM_Props)
			continue;

		if (spa_pod_parse_object(p->param,
						SPA_TYPE_OBJECT_Props, NULL,
						SPA_PROP_bluetoothAudioCodec, SPA_POD_Id(&id)) < 0)
			continue;

		for (j = 0; j < n_codecs; ++j) {
			if (codecs[j].id == id)
				*active = j;
		}
	}

	return n_codecs;
}

void update_object_info(struct pw_manager *manager, struct pw_manager_object *o,
		struct defs *defs)
{
	if (pw_manager_object_is_sink(o)) {
		update_device_info(manager, o, PW_DIRECTION_OUTPUT, false, defs, false);
		update_device_info(manager, o, PW_DIRECTION_OUTPUT, true, defs, false);
	}
	if (pw_manager_object_is_source(o)) {
		update_device_info(manager, o, PW_DIRECTION_INPUT, false, defs, false);
	}
	if (pw_manager_object_is_source_output(o)) {
		update_device_info(manager, o, PW_DIRECTION_INPUT, false, defs, true);
	}
	if (pw_manager_object_is_sink_input(o)) {
		update_device_info(manager, o, PW_DIRECTION_OUTPUT, false, defs, true);
	}
}

struct message_path_data {
	const char *path;
	struct pw_manager_object *object;
};

static int find_message_path_cb(void *data, struct pw_manager_object *o)
{
	struct message_path_data *d = data;
	if (spa_streq(o->message_object_path, d->path)) {
		d->object = o;
		return 1;
	}
	return 0;
}

struct pw_manager_object *find_message_path(struct pw_manager *m, const char *path)
{
	struct message_path_data d = { path, NULL };
	pw_manager_for_each_object(m, find_message_path_cb, &d);
	return d.object;
}
