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

	if (o->props &&
	    pw_properties_fetch_int32(o->props, PW_KEY_PRIORITY_SESSION, &prio) == 0) {
		if (s->best == NULL || prio > s->score) {
			s->best = o;
			s->score = prio;
		}
	}
}

struct pw_manager_object *select_object(struct pw_manager *m, struct selector *s)
{
	struct pw_manager_object *o;
	const char *str;

	spa_list_for_each(o, &m->object_list, link) {
		if (o->creating || o->removing)
			continue;
		if (s->type != NULL && !s->type(o))
			continue;
		if (o->id == s->id)
			return o;
		if (o->index == s->index)
			return o;
		if (s->accumulate)
			s->accumulate(s, o);
		if (o->props && s->key != NULL && s->value != NULL &&
		    (str = pw_properties_get(o->props, s->key)) != NULL &&
		    spa_streq(str, s->value))
			return o;
		if (s->value != NULL && (uint32_t)atoi(s->value) == o->index)
			return o;
	}
	return s->best;
}

uint32_t id_to_index(struct pw_manager *m, uint32_t id)
{
	struct pw_manager_object *o;
	spa_list_for_each(o, &m->object_list, link) {
		if (o->id == id)
			return o->index;
	}
	return SPA_ID_INVALID;
}

bool collect_is_linked(struct pw_manager *m, uint32_t id, enum pw_direction direction)
{
	struct pw_manager_object *o;
	uint32_t in_node, out_node;

	spa_list_for_each(o, &m->object_list, link) {
		if (o->props == NULL || !pw_manager_object_is_link(o))
			continue;

		if (pw_properties_fetch_uint32(o->props, PW_KEY_LINK_OUTPUT_NODE, &out_node) != 0 ||
                    pw_properties_fetch_uint32(o->props, PW_KEY_LINK_INPUT_NODE, &in_node) != 0)
                        continue;

		if ((direction == PW_DIRECTION_OUTPUT && id == out_node) ||
		    (direction == PW_DIRECTION_INPUT && id == in_node))
			return true;
	}
	return false;
}

struct pw_manager_object *find_peer_for_link(struct pw_manager *m,
		struct pw_manager_object *o, uint32_t id, enum pw_direction direction)
{
	struct pw_manager_object *p;
	uint32_t in_node, out_node;

	if (o->props == NULL)
		return NULL;

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

struct pw_manager_object *find_linked(struct pw_manager *m, uint32_t id, enum pw_direction direction)
{
	struct pw_manager_object *o, *p;

	spa_list_for_each(o, &m->object_list, link) {
		if (!pw_manager_object_is_link(o))
			continue;
		if ((p = find_peer_for_link(m, o, id, direction)) != NULL)
			return p;
	}
	return NULL;
}

void collect_card_info(struct pw_manager_object *card, struct card_info *info)
{
	struct pw_manager_param *p;

	spa_list_for_each(p, &card->param_list, link) {
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
	struct pw_manager_param *p;
	struct profile_info *pi;
	uint32_t n;

	n = 0;
	spa_list_for_each(p, &card->param_list, link) {
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
				SPA_PARAM_PROFILE_classes,  SPA_POD_OPT_Pod(&classes)) < 0) {
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

	spa_list_for_each(p, &card->param_list, link) {
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

	if (card && !monitor) {
		spa_list_for_each(p, &card->param_list, link) {
			uint32_t index, dev;
			struct spa_pod *props;

			if (p->id != SPA_PARAM_Route)
				continue;

			if (spa_pod_parse_object(p->param,
					SPA_TYPE_OBJECT_ParamRoute, NULL,
					SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
					SPA_PARAM_ROUTE_device,  SPA_POD_Int(&dev),
					SPA_PARAM_ROUTE_props,  SPA_POD_OPT_Pod(&props)) < 0)
				continue;
			if (dev != dev_info->device)
				continue;
			dev_info->active_port = index;
			if (props) {
				volume_parse_param(props, &dev_info->volume_info, monitor);
				dev_info->have_volume = true;
			}
		}
	}

	spa_list_for_each(p, &device->param_list, link) {
		switch (p->id) {
		case SPA_PARAM_EnumFormat:
		{
			struct spa_pod *copy = spa_pod_copy(p->param);
			spa_pod_fixate(copy);
			format_parse_param(copy, true, &dev_info->ss, &dev_info->map,
					&defs->sample_spec, &defs->channel_map);
			free(copy);
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
		enum pw_direction direction, bool monitor, struct defs *defs)
{
	const char *str;
	const char *key = monitor ? "device.info.monitor" : "device.info";
	struct pw_manager_object *card = NULL;
	struct pw_node_info *info = o->info;
	struct device_info *dev_info, di;

	if (info == NULL)
		return;

	di = DEVICE_INFO_INIT(direction);
	if ((str = spa_dict_lookup(info->props, PW_KEY_DEVICE_ID)) != NULL)
		di.card_id = (uint32_t)atoi(str);
	if ((str = spa_dict_lookup(info->props, "card.profile.device")) != NULL)
		di.device = (uint32_t)atoi(str);
	if (di.card_id != SPA_ID_INVALID) {
		struct selector sel = { .id = di.card_id, .type = pw_manager_object_is_card, };
		card = select_object(manager, &sel);
	}
	collect_device_info(o, card, &di, monitor, defs);

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
			   struct device_info *dev_info, struct port_info *port_info)
{
	struct pw_manager_param *p;
	uint32_t n;

	if (card == NULL)
		return 0;

	n = 0;
	spa_list_for_each(p, &card->param_list, link) {
		struct spa_pod *devices = NULL, *profiles = NULL;
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
				SPA_PARAM_ROUTE_devices,  SPA_POD_OPT_Pod(&devices),
				SPA_PARAM_ROUTE_profiles,  SPA_POD_OPT_Pod(&profiles)) < 0)
			continue;

		if (pi->description == NULL)
			pi->description = pi->name;
		if (devices)
			pi->devices = spa_pod_get_array(devices, &pi->n_devices);
		if (profiles)
			pi->profiles = spa_pod_get_array(profiles, &pi->n_profiles);

		if (dev_info != NULL) {
			if (pi->direction != dev_info->direction)
				continue;
			if (!array_contains(pi->profiles, pi->n_profiles, card_info->active_profile))
				continue;
			if (!array_contains(pi->devices, pi->n_devices, dev_info->device))
				continue;
			if (pi->index == dev_info->active_port)
				dev_info->active_port_name = pi->name;
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
	if (dev_info != NULL && dev_info->active_port_name == NULL && n > 0)
		dev_info->active_port_name = port_info[0].name;
	return n;
}

uint32_t find_port_index(struct pw_manager_object *card, uint32_t direction, const char *port_name)
{
	struct pw_manager_param *p;

	spa_list_for_each(p, &card->param_list, link) {
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

struct spa_dict *collect_props(struct spa_pod *info, struct spa_dict *dict)
{
	struct spa_pod_parser prs;
	struct spa_pod_frame f[1];
	int32_t n, n_items;

	spa_pod_parser_pod(&prs, info);
	if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
	    spa_pod_parser_get_int(&prs, &n_items) < 0)
		return NULL;

	for (n = 0; n < n_items; n++) {
		if (spa_pod_parser_get(&prs,
				SPA_POD_String(&dict->items[n].key),
				SPA_POD_String(&dict->items[n].value),
				NULL) < 0)
			break;
	}
	spa_pod_parser_pop(&prs, &f[0]);
	dict->n_items = n;
	return dict;
}

uint32_t collect_transport_codec_info(struct pw_manager_object *card,
				      struct transport_codec_info *codecs, uint32_t max_codecs,
				      uint32_t *active)
{
	struct pw_manager_param *p;
	uint32_t n_codecs = 0;

	*active = SPA_ID_INVALID;

	if (card == NULL)
		return 0;

	spa_list_for_each(p, &card->param_list, link) {
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

		if (SPA_POD_CHOICE_TYPE(type) != SPA_CHOICE_Enum ||
				SPA_POD_TYPE(SPA_POD_CHOICE_CHILD(type)) != SPA_TYPE_Int)
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
	spa_list_for_each(p, &card->param_list, link) {
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
		update_device_info(manager, o, PW_DIRECTION_OUTPUT, false, defs);
		update_device_info(manager, o, PW_DIRECTION_OUTPUT, true, defs);
	}
	if (pw_manager_object_is_source(o)) {
		update_device_info(manager, o, PW_DIRECTION_INPUT, false, defs);
	}
	if (pw_manager_object_is_source_output(o)) {
		update_device_info(manager, o, PW_DIRECTION_INPUT, false, defs);
	}
	if (pw_manager_object_is_sink_input(o)) {
		update_device_info(manager, o, PW_DIRECTION_OUTPUT, false, defs);
	}
}
