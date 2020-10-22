/* PipeWire
 * Copyright (C) 2018 Wim Taymans <wim.taymans@gmail.com>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include <errno.h>

#include <spa/utils/result.h>
#include <spa/param/props.h>
#include <spa/debug/types.h>

#include <pipewire/pipewire.h>
#include <extensions/metadata.h>

#include <pulse/context.h>
#include <pulse/timeval.h>
#include <pulse/error.h>
#include <pulse/xmalloc.h>

#include "internal.h"

int pa_context_set_error(PA_CONST pa_context *c, int error) {
	pa_assert(error >= 0);
	pa_assert(error < PA_ERR_MAX);
	if (c) {
		pw_log_debug("context %p: error %d %s", c, error, pa_strerror(error));
		((pa_context*)c)->error = error;
	}
	return error;
}

static void global_free(pa_context *c, struct global *g)
{
	pw_log_debug("context %p: %d", c, g->id);

	spa_list_remove(&g->link);

	if (g->ginfo && g->ginfo->destroy)
		g->ginfo->destroy(g);
	if (g->stream)
		g->stream->global = NULL;
	if (g->proxy)
		pw_proxy_destroy(g->proxy);
	if (g->props)
		pw_properties_free(g->props);
	free(g->type);
	free(g);
}

static void context_unlink(pa_context *c)
{
	pa_stream *s, *t;
	struct global *g;
	pa_operation *o;
	struct module_info *m;

	pw_log_debug("context %p: unlink %d", c, c->state);

	c->disconnect = true;
	c->state_callback = NULL;
	c->state_userdata = NULL;

	spa_list_for_each_safe(s, t, &c->streams, link) {
		pa_stream_set_state(s, c->state == PA_CONTEXT_FAILED ?
				PA_STREAM_FAILED : PA_STREAM_TERMINATED);
	}
	if (c->registry) {
		pw_proxy_destroy((struct pw_proxy*)c->registry);
		c->registry = NULL;
	}
	if (c->core) {
		pw_core_disconnect(c->core);
		c->core = NULL;
	}
	spa_list_consume(g, &c->globals, link)
		global_free(c, g);

	spa_list_consume(o, &c->operations, link)
		pa_operation_cancel(o);
	spa_list_consume(m, &c->modules, link)
		pw_proxy_destroy(m->proxy);
}

void pa_context_set_state(pa_context *c, pa_context_state_t st) {
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	if (c->state == st)
		return;

	pw_log_debug("context %p: state %d", c, st);

	pa_context_ref(c);

	c->state = st;

	if (c->state_callback)
		c->state_callback(c, c->state_userdata);

	if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED)
		context_unlink(c);

	pa_context_unref(c);
}

void pa_context_fail(PA_CONST pa_context *c, int error) {
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	pw_log_debug("context %p: error %d", c, error);

	pa_context_set_error(c, error);
	pa_context_set_state((pa_context*)c, PA_CONTEXT_FAILED);
}

SPA_EXPORT
pa_context *pa_context_new(pa_mainloop_api *mainloop, const char *name)
{
	return pa_context_new_with_proplist(mainloop, name, NULL);
}

pa_stream *pa_context_find_stream(pa_context *c, uint32_t idx)
{
	pa_stream *s;
	spa_list_for_each(s, &c->streams, link) {
		if (s->stream_index == idx)
			return s;
	}
	return NULL;
}

struct global *pa_context_find_global(pa_context *c, uint32_t id)
{
	struct global *g;
	spa_list_for_each(g, &c->globals, link) {
		if (g->id == id)
			return g;
	}
	return NULL;
}

const char *pa_context_find_global_name(pa_context *c, uint32_t id)
{
	struct global *g;
	const char *name = NULL;

	g = pa_context_find_global(c, id & PA_IDX_MASK_MONITOR);
	if (g == NULL)
		return "unknown object";

	if (g->mask & (PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE)) {
		name = pw_properties_get(g->props, PW_KEY_NODE_NAME);
	}
	if (name == NULL)
		name = "unknown";
	return name;
}

static inline bool pa_endswith(const char *s, const char *sfx)
{
	size_t l1, l2;
	l1 = strlen(s);
	l2 = strlen(sfx);
	return l1 >= l2 && pa_streq(s + l1 - l2, sfx);
}

struct global *pa_context_find_global_by_name(pa_context *c, uint32_t mask, const char *name)
{
	struct global *g;
	const char *str;
	uint32_t id;

	if (strcmp(name, "@DEFAULT_SINK@") == 0 || strcmp("@DEFAULT_MONITOR@", name) == 0)
		id = c->default_sink;
	else if (strcmp(name, "@DEFAULT_SOURCE@") == 0)
		id = c->default_sink;
	else
		id = atoi(name);

	spa_list_for_each(g, &c->globals, link) {
		if ((g->mask & mask) == 0)
			continue;
		if (g->props != NULL &&
		    (str = pw_properties_get(g->props, PW_KEY_NODE_NAME)) != NULL) {
			if (strcmp(str, name) == 0)
				return g;
			if (pa_endswith(name, ".monitor") &&
			    strncmp(str, name, strlen(name) - 8) == 0)
				return g;
		}
		if (id == SPA_ID_INVALID || g->id == id || (g->id == (id & PA_IDX_MASK_MONITOR)))
			return g;
	}
	return NULL;
}

struct global *pa_context_find_linked(pa_context *c, uint32_t idx)
{
	struct global *g, *f;

	spa_list_for_each(g, &c->globals, link) {
		uint32_t src_node_id, dst_node_id;

		if (strcmp(g->type, PW_TYPE_INTERFACE_Link) != 0)
			continue;

		src_node_id = g->link_info.src->port_info.node_id;
		dst_node_id = g->link_info.dst->port_info.node_id;

		pw_log_debug("context %p: %p %d %d %d", c, g, idx,
				src_node_id, dst_node_id);

		if (src_node_id == idx)
			f = pa_context_find_global(c, dst_node_id);
		else if (dst_node_id == idx)
			f = pa_context_find_global(c, src_node_id);
		else
			continue;

		if (f == NULL ||
		    !(f->mask & (PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE)))
			continue;
		return f;
	}
	return NULL;
}

static const char *str_etype(pa_subscription_event_type_t event)
{
	switch (event & PA_SUBSCRIPTION_EVENT_TYPE_MASK) {
	case PA_SUBSCRIPTION_EVENT_NEW:
		return "new";
	case PA_SUBSCRIPTION_EVENT_CHANGE:
		return "change";
	case PA_SUBSCRIPTION_EVENT_REMOVE:
		return "remove";
	}
	return "invalid";
}

static const char *str_efac(pa_subscription_event_type_t event)
{
	switch (event & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) {
	case PA_SUBSCRIPTION_EVENT_SINK:
		return "sink";
	case PA_SUBSCRIPTION_EVENT_SOURCE:
		return "source";
	case PA_SUBSCRIPTION_EVENT_SINK_INPUT:
		return "sink-input";
	case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT:
		return "source-output";
	case PA_SUBSCRIPTION_EVENT_MODULE:
		return "module";
	case PA_SUBSCRIPTION_EVENT_CLIENT:
		return "client";
	case PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE:
		return "sample-cache";
	case PA_SUBSCRIPTION_EVENT_SERVER:
		return "server";
	case PA_SUBSCRIPTION_EVENT_AUTOLOAD:
		return "autoload";
	case PA_SUBSCRIPTION_EVENT_CARD:
		return "card";
	}
	return "invalid";
}

static void emit_event(pa_context *c, struct global *g, pa_subscription_event_type_t event)
{
	if (c->subscribe_callback && (c->subscribe_mask & g->mask)) {
		pw_log_debug("context %p: obj %d: emit %s:%s", c, g->id,
				str_etype(event), str_efac(g->event));
		c->subscribe_callback(c,
				event | g->event,
				g->id,
				c->subscribe_userdata);

		if (g->mask == (PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE)) {
			pw_log_debug("context %p: obj %d: emit %s:source", c, g->node_info.monitor,
					str_etype(event));
			c->subscribe_callback(c,
					event | PA_SUBSCRIPTION_EVENT_SOURCE,
					g->node_info.monitor,
					c->subscribe_userdata);
		}
	}
}

static void do_global_sync(struct global *g)
{
	pa_subscription_event_type_t event;

	pw_log_debug("global %p sync", g);
	if (g->ginfo && g->ginfo->sync)
		g->ginfo->sync(g);

	if (g->init) {
		if ((g->mask & (PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT))) {
		    if (g->node_info.device_index == SPA_ID_INVALID ||
		        (g->stream && g->stream->state != PA_STREAM_READY))
				return;
		}
		g->init = false;
		g->changed++;
		event = PA_SUBSCRIPTION_EVENT_NEW;
	} else {
		event = PA_SUBSCRIPTION_EVENT_CHANGE;
	}
	if (g->changed > 0) {
		emit_event(g->context, g, event);
		g->changed = 0;
	}
}


static void global_sync(struct global *g)
{
	pa_context *c = g->context;
	c->pending_seq = pw_core_sync(c->core, PW_ID_CORE, c->pending_seq);
	g->sync = true;
}

static struct param *add_param(struct spa_list *params, uint32_t id, const struct spa_pod *param)
{
	struct param *p;

	if (param == NULL || !spa_pod_is_object(param)) {
		errno = EINVAL;
		return NULL;
	}
	if (id == SPA_ID_INVALID)
		id = SPA_POD_OBJECT_ID(param);

	p = malloc(sizeof(struct param) + SPA_POD_SIZE(param));
	if (p == NULL)
		return NULL;

	p->id = id;
	p->param = SPA_MEMBER(p, sizeof(struct param), struct spa_pod);
	memcpy(p->param, param, SPA_POD_SIZE(param));
	spa_list_append(params, &p->link);

	return p;
}

static void remove_params(struct spa_list *params, uint32_t id)
{
	struct param *p, *t;
	spa_list_for_each_safe(p, t, params, link) {
		if (id == SPA_ID_INVALID || p->id == id) {
			spa_list_remove(&p->link);
			free(p);
		}
	}
}

static void update_device_props(struct global *g)
{
	pa_card_info *i = &g->card_info.info;
	const char *s;

	if ((s = pa_proplist_gets(i->proplist, PW_KEY_DEVICE_ICON_NAME)))
		pa_proplist_sets(i->proplist, PA_PROP_DEVICE_ICON_NAME, s);
}


static void device_event_info(void *object, const struct pw_device_info *info)
{
        struct global *g = object;
	pa_card_info *i = &g->card_info.info;
	const char *str;
	uint32_t n;

	pw_log_debug("global %p: id:%d change-mask:%"PRIu64, g, g->id, info->change_mask);
        info = g->info = pw_device_info_update(g->info, info);

	i->index = g->id;
	i->name = info->props ?
		spa_dict_lookup(info->props, PW_KEY_DEVICE_NAME) : "unknown";
	str = info->props ? spa_dict_lookup(info->props, PW_KEY_MODULE_ID) : NULL;
	i->owner_module = str ? (unsigned)atoi(str) : SPA_ID_INVALID;
	if (info->change_mask & PW_DEVICE_CHANGE_MASK_PROPS) {
		i->driver = info->props ?
			spa_dict_lookup(info->props, PW_KEY_DEVICE_API) : NULL;

		if (i->proplist)
			pa_proplist_update_dict(i->proplist, info->props);
		else {
			i->proplist = pa_proplist_new_dict(info->props);
		}
		update_device_props(g);
		g->changed++;
	}
	if (info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) {
		for (n = 0; n < info->n_params; n++) {
			uint32_t id = info->params[n].id;
			bool do_enum = true;

			if (info->params[n].user == 0)
				continue;

			info->params[n].user = 0;

			switch (id) {
			case SPA_PARAM_EnumProfile:
				if (g->card_info.pending_profiles)
					continue;
				remove_params(&g->card_info.profiles, id);
				g->changed++;
				g->card_info.n_profiles = 0;
				break;
			case SPA_PARAM_EnumRoute:
				if (g->card_info.pending_ports)
					continue;
				remove_params(&g->card_info.ports, id);
				g->changed++;
				g->card_info.n_ports = 0;
				break;
			case SPA_PARAM_Route:
				remove_params(&g->card_info.routes, id);
				g->card_info.n_routes = 0;
				break;
			case SPA_PARAM_Profile:
				break;
			default:
				do_enum = false;
				break;
			}
			if (!(info->params[n].flags & SPA_PARAM_INFO_READ))
				continue;
			if (do_enum) {
				switch (id) {
				case SPA_PARAM_EnumProfile:
				case SPA_PARAM_Profile:
					g->card_info.pending_profiles = true;
					break;
				case SPA_PARAM_EnumRoute:
				case SPA_PARAM_Route:
					g->card_info.pending_ports = true;
					break;
				}
				pw_log_debug("global %p: id:%d do enum %s", g, g->id,
					spa_debug_type_find_name(spa_type_param, id));
				pw_device_enum_params((struct pw_device*)g->proxy,
						0, id, 0, -1, NULL);
			}
		}
	}
	if (i->driver == NULL)
		i->driver = "PipeWire";
	global_sync(g);
}

static int parse_props(struct global *g, const struct spa_pod *param, bool device)
{
	int changed = 0;
	struct spa_pod_prop *prop;
	struct spa_pod_object *obj = (struct spa_pod_object *) param;

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_volume:
		{
			float vol;
			if (spa_pod_get_float(&prop->value, &vol) >= 0 &&
			    g->node_info.volume != vol) {
				g->node_info.volume = vol;
				changed++;
			}
			SPA_FLAG_UPDATE(g->node_info.flags, NODE_FLAG_DEVICE_VOLUME, device);
			SPA_FLAG_UPDATE(g->node_info.flags, NODE_FLAG_HW_VOLUME,
					prop->flags & SPA_POD_PROP_FLAG_HARDWARE);
			break;
		}
		case SPA_PROP_mute:
		{
			bool mute;
			if (spa_pod_get_bool(&prop->value, &mute) >= 0 &&
			    g->node_info.mute != mute) {
				g->node_info.mute = mute;
				changed++;
			}
			SPA_FLAG_UPDATE(g->node_info.flags, NODE_FLAG_DEVICE_MUTE, device);
			SPA_FLAG_UPDATE(g->node_info.flags, NODE_FLAG_HW_MUTE,
					prop->flags & SPA_POD_PROP_FLAG_HARDWARE);
			break;
		}
		case SPA_PROP_channelVolumes:
		{
			uint32_t n_vals;
			float vol[SPA_AUDIO_MAX_CHANNELS];

			n_vals = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					vol, SPA_AUDIO_MAX_CHANNELS);

			if (n_vals != g->node_info.n_channel_volumes) {
				pw_log_debug("channel change %d->%d, trigger remove",
						g->node_info.n_channel_volumes, n_vals);
				if (!g->init)
					emit_event(g->context, g, PA_SUBSCRIPTION_EVENT_REMOVE);
				g->node_info.n_channel_volumes = n_vals;
				/* mark as init, this will emit the NEW event when the
				 * params are updated */
				g->init = g->sync = true;
				changed++;
			}
			if (memcmp(g->node_info.channel_volumes, vol, n_vals * sizeof(float)) != 0) {
				memcpy(g->node_info.channel_volumes, vol, n_vals * sizeof(float));
				changed++;
			}
			SPA_FLAG_UPDATE(g->node_info.flags, NODE_FLAG_DEVICE_VOLUME, device);
			SPA_FLAG_UPDATE(g->node_info.flags, NODE_FLAG_HW_VOLUME,
					prop->flags & SPA_POD_PROP_FLAG_HARDWARE);
			break;
		}
		case SPA_PROP_volumeBase:
			spa_pod_get_float(&prop->value, &g->node_info.base_volume);
			break;
		case SPA_PROP_volumeStep:
			spa_pod_get_float(&prop->value, &g->node_info.volume_step);
			break;
		default:
			break;
		}
	}
	return changed;
}

static struct global *find_node_for_route(pa_context *c, struct global *card, uint32_t device)
{
	struct global *n;
	spa_list_for_each(n, &c->globals, link) {
		if (strcmp(n->type, PW_TYPE_INTERFACE_Node) != 0)
			continue;
		pw_log_debug("%d/%d %d/%d",
				n->node_info.device_id, card->id,
				n->node_info.profile_device_id, device);
		if (n->node_info.device_id != card->id)
			continue;
		if (n->node_info.profile_device_id != device)
			continue;
		return n;
	}
	return NULL;
}

static void device_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct global *g = object;

	pw_log_debug("update param %d %s", g->id,
			spa_debug_type_find_name(spa_type_param, id));

	switch (id) {
	case SPA_PARAM_EnumProfile:
	{
		uint32_t index;
		const char *name;

		if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&index),
				SPA_PARAM_PROFILE_name,  SPA_POD_String(&name)) < 0) {
			pw_log_warn("device %d: can't parse profile", g->id);
			return;
		}
		if (add_param(&g->card_info.profiles, id, param))
			g->card_info.n_profiles++;

		pw_log_debug("device %d: enum profile %d: \"%s\" n_profiles:%d", g->id,
				index, name, g->card_info.n_profiles);
		break;
	}
	case SPA_PARAM_Profile:
	{
		uint32_t index;
		if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&index)) < 0) {
			pw_log_warn("device %d: can't parse profile", g->id);
			return;
		}
		pw_log_debug("device %d: current profile %d", g->id, index);
		if (g->card_info.active_profile != index) {
			g->changed++;
			g->card_info.active_profile = index;
		}
		break;
	}
	case SPA_PARAM_EnumRoute:
	{
		uint32_t index;
		const char *name;

		if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamRoute, NULL,
				SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
				SPA_PARAM_ROUTE_name,  SPA_POD_String(&name)) < 0) {
			pw_log_warn("device %d: can't parse route", g->id);
			return;
		}
		if (add_param(&g->card_info.ports, id, param))
			g->card_info.n_ports++;

		pw_log_debug("device %d: enum route %d: \"%s\"", g->id, index, name);
		break;
	}
	case SPA_PARAM_Route:
	{
		uint32_t index, device;
		enum spa_direction direction;

		if (spa_pod_parse_object(param,
				SPA_TYPE_OBJECT_ParamRoute, NULL,
				SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
				SPA_PARAM_ROUTE_direction, SPA_POD_Id(&direction),
				SPA_PARAM_ROUTE_device, SPA_POD_Int(&device)) < 0) {
			pw_log_warn("device %d: can't parse route", g->id);
			return;
		}
		if (add_param(&g->card_info.routes, id, param))
			g->card_info.n_routes++;

		pw_log_debug("device %d: active %s route %d device %d", g->id,
				direction == SPA_DIRECTION_OUTPUT ? "output" : "input",
				index, device);
		break;
	}
	default:
		break;
	}
}

static void device_clear_profiles(struct global *g)
{
	pa_card_info *i = &g->card_info.info;
	i->n_profiles = 0;
	free(i->profiles);
	i->profiles = NULL;
	free(g->card_info.card_profiles);
	g->card_info.card_profiles = NULL;
	free(i->profiles2);
	i->profiles2 = NULL;
}

static void device_sync_profiles(struct global *g)
{
	pa_card_info *i = &g->card_info.info;
	uint32_t n_profiles, j;
	struct param *p;

	device_clear_profiles(g);

	n_profiles = g->card_info.n_profiles;

	i->profiles = calloc(n_profiles, sizeof(pa_card_profile_info));
	g->card_info.card_profiles = calloc(n_profiles, sizeof(pa_card_profile_info2));
	i->profiles2 = calloc(n_profiles + 1, sizeof(pa_card_profile_info2 *));
	i->n_profiles = 0;

	pw_log_debug("context %p: info for %d n_profiles:%d", g->context, g->id, n_profiles);

	j = 0;
	spa_list_for_each(p, &g->card_info.profiles, link) {
		uint32_t id, priority = 0, available = 0, n_cap = 0, n_play = 0;
		const char *name = NULL;
		const char *description = NULL;
		struct spa_pod *classes = NULL, *info = NULL;

		if (spa_pod_parse_object(p->param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&id),
				SPA_PARAM_PROFILE_name,  SPA_POD_String(&name),
				SPA_PARAM_PROFILE_description,  SPA_POD_OPT_String(&description),
				SPA_PARAM_PROFILE_priority,  SPA_POD_OPT_Int(&priority),
				SPA_PARAM_PROFILE_available,  SPA_POD_OPT_Id(&available),
				SPA_PARAM_PROFILE_info,  SPA_POD_OPT_Pod(&info),
				SPA_PARAM_PROFILE_classes,  SPA_POD_OPT_Pod(&classes)) < 0) {
			pw_log_warn("device %d: can't parse profile", g->id);
			continue;
		}
		if (classes != NULL) {
			struct spa_pod *iter;

			SPA_POD_STRUCT_FOREACH(classes, iter) {
				struct spa_pod_parser prs;
				struct spa_pod_frame f[1];
				char *class;
				uint32_t count;

				spa_pod_parser_pod(&prs, iter);
				if (spa_pod_parser_get_struct(&prs,
						SPA_POD_String(&class),
						SPA_POD_Int(&count)) < 0)
					continue;

				if (strcmp(class, "Audio/Sink") == 0)
					n_play += count;
				else if (strcmp(class, "Audio/Source") == 0)
					n_cap += count;

				spa_pod_parser_pop(&prs, &f[0]);
			}
		}
		pw_log_debug("profile %d: name:%s", j, name);

		i->profiles[j].name = name;
		i->profiles[j].description = description ? description : name;
		i->profiles[j].n_sinks = n_play;
		i->profiles[j].n_sources = n_cap;
		i->profiles[j].priority = priority;

		i->profiles2[j] = &g->card_info.card_profiles[j];
		i->profiles2[j]->name = i->profiles[j].name;
		i->profiles2[j]->description = i->profiles[j].description;
	        i->profiles2[j]->n_sinks = i->profiles[j].n_sinks;
	        i->profiles2[j]->n_sources = i->profiles[j].n_sources;
	        i->profiles2[j]->priority = i->profiles[j].priority;
	        i->profiles2[j]->available = available != SPA_PARAM_AVAILABILITY_no;

		if (g->card_info.active_profile == id) {
			i->active_profile = &i->profiles[j];
			i->active_profile2 = i->profiles2[j];
		}
		j++;
	}
	i->profiles2[j] = NULL;
	i->n_profiles = j;
}

static void device_clear_ports(struct global *g)
{
	pa_card_info *i = &g->card_info.info;
	uint32_t n;

	pw_log_debug("device %d clear ports %d", g->id, i->n_ports);

	for (n = 0; n < i->n_ports; n++) {
		pa_card_port_info *pi = i->ports[n];
		pa_proplist_free(pi->proplist);
		free(pi->profiles2);
	}

	i->n_ports = 0;
	free(i->ports);
	i->ports = NULL;
	free(g->card_info.card_ports);
	g->card_info.card_ports = NULL;
	free(g->card_info.port_devices);
	g->card_info.port_devices = NULL;
}

static void device_sync_ports(struct global *g)
{
	pa_card_info *i = &g->card_info.info;
	pa_context *c = g->context;
	uint32_t n_ports, j;
	struct param *p;

	device_clear_ports(g);

	n_ports = g->card_info.n_ports;
	i->ports = calloc(n_ports+1, sizeof(pa_card_port_info *));
	g->card_info.card_ports = calloc(n_ports, sizeof(pa_card_port_info));
	g->card_info.port_devices = calloc(n_ports, sizeof(struct port_device));
	i->n_ports = 0;

	pw_log_debug("context %p: info for %d n_ports:%d", g->context, g->id, n_ports);

	j = 0;

	spa_list_for_each(p, &g->card_info.ports, link) {
		uint32_t id, priority;
		enum spa_direction direction;
		const char *name = NULL, *description = NULL;
		enum spa_param_availability available = SPA_PARAM_AVAILABILITY_unknown;
		struct spa_pod *profiles = NULL, *info = NULL, *devices = NULL;
		pa_card_port_info *pi;
		struct port_device *pd;

		if (spa_pod_parse_object(p->param,
				SPA_TYPE_OBJECT_ParamRoute, NULL,
				SPA_PARAM_ROUTE_index, SPA_POD_Int(&id),
				SPA_PARAM_ROUTE_direction, SPA_POD_Id(&direction),
				SPA_PARAM_ROUTE_name,  SPA_POD_String(&name),
				SPA_PARAM_ROUTE_description,  SPA_POD_OPT_String(&description),
				SPA_PARAM_ROUTE_priority,  SPA_POD_OPT_Int(&priority),
				SPA_PARAM_ROUTE_available,  SPA_POD_OPT_Id(&available),
				SPA_PARAM_ROUTE_info,  SPA_POD_OPT_Pod(&info),
				SPA_PARAM_ROUTE_devices,  SPA_POD_OPT_Pod(&devices),
				SPA_PARAM_ROUTE_profiles,  SPA_POD_OPT_Pod(&profiles)) < 0) {
			pw_log_warn("device %d: can't parse route", g->id);
			continue;
		}

		pw_log_debug("port %d: name:%s available:%d", j, name, available);

		pi = i->ports[j] = &g->card_info.card_ports[j];
		spa_zero(*pi);
		pi->name = name;
		pi->description = description;
		pi->priority = priority;
		pi->available = available;
		pi->direction = direction == SPA_DIRECTION_INPUT ? PA_DIRECTION_INPUT : PA_DIRECTION_OUTPUT;
		pi->proplist = pa_proplist_new();
		while (info) {
			struct spa_pod_parser prs;
			struct spa_pod_frame f[1];
			int32_t n, n_items;
			const char *key, *value;

			spa_pod_parser_pod(&prs, info);
			if (spa_pod_parser_push_struct(&prs, &f[0]) < 0 ||
			    spa_pod_parser_get_int(&prs, &n_items) < 0)
				break;

			for (n = 0; n < n_items; n++) {
				if (spa_pod_parser_get(&prs,
						SPA_POD_String(&key),
						SPA_POD_String(&value),
						NULL) < 0)
					break;
				pa_proplist_sets(pi->proplist, key, value);
			}
			spa_pod_parser_pop(&prs, &f[0]);
			break;
		}
		pi->n_profiles = 0;
		pi->profiles = NULL;
		pi->profiles2 = NULL;
		while (profiles) {
			uint32_t *pr, n, n_pr;

			pr = spa_pod_get_array(profiles, &n_pr);
			if (pr == NULL)
				break;

			pi->n_profiles = n_pr;
			pi->profiles2 = calloc(n_pr + 1, sizeof(pa_card_profile_info2 *));
			for (n = 0; n < n_pr; n++)
				pi->profiles2[n] = i->profiles2[pr[n]];
			pi->profiles2[n_pr] = NULL;
			pi->profiles = (pa_card_profile_info **)pi->profiles2;
			break;
		}
		pd = &g->card_info.port_devices[j];
		pd->n_devices = 0;
		pd->devices = NULL;
		if (devices)
			pd->devices = spa_pod_get_array(devices, &pd->n_devices);
		j++;
	}
	i->ports[j] = NULL;
	i->n_ports = j;
	if (i->n_ports == 0) {
		device_clear_ports(g);
	}

	spa_list_for_each(p, &g->card_info.routes, link) {
		struct global *ng;
		uint32_t index, device;
		enum spa_param_availability available = SPA_PARAM_AVAILABILITY_unknown;
		struct spa_pod *props = NULL;
		const char *name;

		if (spa_pod_parse_object(p->param,
				SPA_TYPE_OBJECT_ParamRoute, NULL,
				SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
				SPA_PARAM_ROUTE_name,  SPA_POD_String(&name),
				SPA_PARAM_ROUTE_device, SPA_POD_Int(&device),
				SPA_PARAM_ROUTE_available,  SPA_POD_OPT_Id(&available),
				SPA_PARAM_ROUTE_props, SPA_POD_OPT_Pod(&props)) < 0) {
			pw_log_warn("device %d: can't parse route", g->id);
			continue;
		}
		ng = find_node_for_route(c, g, device);
		if (ng) {
			int changed = 0;
			pw_log_debug("device: %d port:%d: name:%s available:%d", ng->id,
					index, name, available);
			if (ng->node_info.active_port != index) {
				ng->node_info.active_port = index;
				changed++;
			}
			if (ng->node_info.available_port != available) {
				ng->node_info.available_port = available;
				changed++;
			}
			if (props)
				changed += parse_props(ng, props, true);
			if (changed) {
				ng->changed += changed;
				global_sync(ng);
			}
		}
	}
}

static void device_sync(struct global *g)
{
	if (g->card_info.pending_profiles) {
		device_sync_profiles(g);
		g->card_info.pending_profiles = false;
		g->card_info.pending_ports = true;
	}
	if (g->card_info.pending_ports) {
		device_sync_ports(g);
		g->card_info.pending_ports = false;
	}
}

static const struct pw_device_events device_events = {
	PW_VERSION_DEVICE_EVENTS,
	.info = device_event_info,
	.param = device_event_param,
};

static void device_destroy(void *data)
{
	struct global *global = data;

	pw_log_debug("device %d destroy", global->id);

	if (global->card_info.info.proplist)
		pa_proplist_free(global->card_info.info.proplist);

	device_clear_ports(global);
	device_clear_profiles(global);

	remove_params(&global->card_info.routes, SPA_ID_INVALID);
	remove_params(&global->card_info.ports, SPA_ID_INVALID);
	remove_params(&global->card_info.profiles, SPA_ID_INVALID);

	if (global->info)
		pw_device_info_free(global->info);
}

struct global_info device_info = {
	.version = PW_VERSION_DEVICE,
	.events = &device_events,
	.destroy = device_destroy,
	.sync = device_sync,
};

static void clear_node_formats(struct global *g)
{
	pa_format_info *f;
	pw_array_for_each(f, &g->node_info.formats)
		pa_format_info_free(f);
	g->changed++;
}

static void node_event_info(void *object, const struct pw_node_info *info)
{
	struct global *g = object;
	const char *str;
	uint32_t i;

	pw_log_debug("global %p: id:%d change-mask:%"PRIu64, g, g->id, info->change_mask);
	info = g->info = pw_node_info_update(g->info, info);

	if (info->change_mask & PW_NODE_CHANGE_MASK_PROPS) {
		if (info->props && (str = spa_dict_lookup(info->props, "card.profile.device")))
			g->node_info.profile_device_id = atoi(str);
		else
			g->node_info.profile_device_id = SPA_ID_INVALID;
		g->changed++;
	}
	if (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) {
		for (i = 0; i < info->n_params; i++) {
			uint32_t id = info->params[i].id;
			bool do_enum;

			if (info->params[i].user == 0)
				continue;
			info->params[i].user = 0;

			switch (id) {
			case SPA_PARAM_EnumFormat:
				clear_node_formats(g);
				/* fallthrough */
			case SPA_PARAM_Props:
			case SPA_PARAM_Format:
				do_enum = true;
				break;
			default:
				do_enum = false;
				break;
			}

			if (!(info->params[i].flags & SPA_PARAM_INFO_READ))
				continue;

			if (do_enum) {
				pw_log_debug("global %p: id:%d do enum %s", g, g->id,
					spa_debug_type_find_name(spa_type_param, id));

				pw_node_enum_params((struct pw_node*)g->proxy,
					0, id, 0, -1, NULL);
			}
		}
	}
	global_sync(g);
}

static void node_event_param(void *object, int seq,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct global *g = object;

	pw_log_debug("update param %d %s", g->id,
			spa_debug_type_find_name(spa_type_param, id));

	switch (id) {
	case SPA_PARAM_Props:
		if (!SPA_FLAG_IS_SET(g->node_info.flags, NODE_FLAG_DEVICE_VOLUME | NODE_FLAG_DEVICE_MUTE))
			parse_props(g, param, false);
		break;
	case SPA_PARAM_EnumFormat:
	{
		pa_format_info *f = pa_format_info_from_param(param);
		if (f) {
			pw_array_add_ptr(&g->node_info.formats, f);

			if (g->node_info.channel_map.channels == 0)
				pa_format_info_get_channel_map(f, &g->node_info.channel_map);

			if (g->node_info.sample_spec.format == 0 ||
			    g->node_info.sample_spec.rate == 0 ||
			    g->node_info.sample_spec.channels == 0) {
				pa_format_info_get_sample_format(f, &g->node_info.sample_spec.format);
				pa_format_info_get_rate(f, &g->node_info.sample_spec.rate);
				pa_format_info_get_channels(f, &g->node_info.sample_spec.channels);
			}
		}
		break;
	}
	case SPA_PARAM_Format:
		pa_format_parse_param(param, &g->node_info.sample_spec, &g->node_info.channel_map);
		break;
	default:
		break;
	}
}

static const struct pw_node_events node_events = {
	PW_VERSION_NODE_EVENTS,
	.info = node_event_info,
	.param = node_event_param,
};

static void node_destroy(void *data)
{
	struct global *global = data;
	clear_node_formats(global);
	if (global->info)
		pw_node_info_free(global->info);
}

struct global_info node_info = {
	.version = PW_VERSION_NODE,
	.events = &node_events,
	.destroy = node_destroy,
};


static void module_event_info(void *object, const struct pw_module_info *info)
{
        struct global *g = object;
	pa_module_info *i = &g->module_info.info;

	pw_log_debug("global %p: id:%d change-mask:%"PRIu64, g, g->id, info->change_mask);

        info = g->info = pw_module_info_update(g->info, info);

	i->index = g->id;
	if (info->change_mask & PW_MODULE_CHANGE_MASK_PROPS) {
		if (i->proplist)
			pa_proplist_update_dict(i->proplist, info->props);
		else
			i->proplist = pa_proplist_new_dict(info->props);
		g->changed++;
	}
	i->name = info->name;
	i->argument = info->args;
	i->n_used = -1;
	i->auto_unload = false;
	global_sync(g);
}

static const struct pw_module_events module_events = {
	PW_VERSION_MODULE_EVENTS,
	.info = module_event_info,
};

static void module_destroy(void *data)
{
	struct global *global = data;
	if (global->module_info.info.proplist)
		pa_proplist_free(global->module_info.info.proplist);
	if (global->info)
		pw_module_info_free(global->info);
}

struct global_info module_info = {
	.version = PW_VERSION_MODULE,
	.events = &module_events,
	.destroy = module_destroy,
};

static void client_event_info(void *object, const struct pw_client_info *info)
{
        struct global *g = object;
	const char *str;
	pa_client_info *i = &g->client_info.info;

	pw_log_debug("global %p: id:%d change-mask:%"PRIu64, g, g->id, info->change_mask);
	info = g->info = pw_client_info_update(g->info, info);

	i->index = g->id;
	str = info->props ? spa_dict_lookup(info->props, PW_KEY_MODULE_ID) : NULL;
	i->owner_module = str ? (unsigned)atoi(str) : SPA_ID_INVALID;

	if (info->change_mask & PW_CLIENT_CHANGE_MASK_PROPS) {
		if (i->proplist)
			pa_proplist_update_dict(i->proplist, info->props);
		else
			i->proplist = pa_proplist_new_dict(info->props);
		i->name = info->props ?
			spa_dict_lookup(info->props, PW_KEY_APP_NAME) : NULL;
		i->driver = info->props ?
			spa_dict_lookup(info->props, PW_KEY_PROTOCOL) : NULL;
		g->changed++;
	}
	if (i->name == NULL)
		i->name = "Unknown";
	if (i->driver == NULL)
		i->name = "PipeWire";
	global_sync(g);
}

static const struct pw_client_events client_events = {
	PW_VERSION_CLIENT_EVENTS,
	.info = client_event_info,
};

static void client_destroy(void *data)
{
	struct global *global = data;
	if (global->client_info.info.proplist)
		pa_proplist_free(global->client_info.info.proplist);
	if (global->info)
		pw_client_info_free(global->info);
}

struct global_info client_info = {
	.version = PW_VERSION_CLIENT,
	.events = &client_events,
	.destroy = client_destroy,
};

static int metadata_property(void *object,
                        uint32_t subject,
                        const char *key,
                        const char *type,
                        const char *value)
{
	struct global *global = object;
	pa_context *c = global->context;
	uint32_t val;
	bool changed = false;

	if (subject == PW_ID_CORE) {
		val = (key && value) ? (uint32_t)atoi(value) : SPA_ID_INVALID;
		if (key == NULL || strcmp(key, METADATA_DEFAULT_SINK) == 0) {
			changed = c->default_sink != val;
			c->default_sink = val;
		}
		if (key == NULL || strcmp(key, METADATA_DEFAULT_SOURCE) == 0) {
			changed = c->default_source != val;
			c->default_source = val;
		}
	}
	if (changed)
		emit_event(global->context, global, PA_SUBSCRIPTION_EVENT_CHANGE);

	return 0;
}

static const struct pw_metadata_events metadata_events = {
	PW_VERSION_METADATA_EVENTS,
	.property = metadata_property,
};

static void metadata_destroy(void *data)
{
	struct global *global = data;
	pa_context *c = global->context;
	if (c->metadata == global)
		c->metadata = NULL;
	pw_array_clear(&global->metadata_info.metadata);
}

struct global_info metadata_info = {
	.version = PW_VERSION_METADATA,
	.events = &metadata_events,
	.destroy = metadata_destroy,
};

static void proxy_removed(void *data)
{
	struct global *g = data;
	pw_proxy_destroy(g->proxy);
}

static void proxy_destroy(void *data)
{
	struct global *g = data;
	spa_hook_remove(&g->proxy_listener);
	spa_hook_remove(&g->object_listener);
	g->proxy = NULL;
}

static const struct pw_proxy_events proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.removed = proxy_removed,
	.destroy = proxy_destroy,
};

static void configure_device(pa_stream *s, struct global *g)
{
	const char *str;
	uint32_t old = s->device_index;

	if (s->direction == PA_STREAM_RECORD) {
		if (g->mask == (PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE)) {
			s->device_index = g->node_info.monitor;
		}
		else
			s->device_index = g->id;
	} else {
		s->device_index = g->id;
	}

	free(s->device_name);
	if ((str = pw_properties_get(g->props, PW_KEY_NODE_NAME)) == NULL)
		s->device_name = strdup("unknown");
	else
		s->device_name = strdup(str);

	pw_log_debug("stream %p: linked to %d '%s'", s, s->device_index, s->device_name);

	if (old != SPA_ID_INVALID && old != s->device_index &&
	    s->state == PA_STREAM_READY && s->moved_callback)
		s->moved_callback(s, s->moved_userdata);
}

static void update_link(pa_context *c, uint32_t src_node_id, uint32_t dst_node_id)
{
	struct global *s, *d;

	s = pa_context_find_global(c, src_node_id);
	d = pa_context_find_global(c, dst_node_id);

	if (s == NULL || d == NULL)
		return;

	if (s->stream && s->stream->direct_on_input == dst_node_id) {
		pw_log_debug("node %d linked to stream %d %p (%d)",
				src_node_id, dst_node_id, s->stream, s->stream->state);
	}
	else if (d->stream && d->stream->direct_on_input == src_node_id) {
		pw_log_debug("node %d linked to stream %d %p (%d)",
				dst_node_id, src_node_id, d->stream, d->stream->state);
	}
	else if ((s->mask & (PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE)) &&
	    (d->mask & (PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT))) {
		pw_log_debug("node %d linked to device %d", dst_node_id, src_node_id);
		d->node_info.device_index = src_node_id;
		if (d->stream)
			configure_device(d->stream, s);
		if (!d->init)
			emit_event(c, d, PA_SUBSCRIPTION_EVENT_CHANGE);
	} else if ((s->mask & (PA_SUBSCRIPTION_MASK_SINK_INPUT | PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT)) &&
	    (d->mask & (PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE))) {
		pw_log_debug("node %d linked to device %d", src_node_id, dst_node_id);
		s->node_info.device_index = dst_node_id;
		if (s->stream)
			configure_device(s->stream, d);
		if (!s->init)
			emit_event(c, s, PA_SUBSCRIPTION_EVENT_CHANGE);
	}
}

static int set_mask(pa_context *c, struct global *g)
{
	const char *str;
	struct global_info *ginfo = NULL;

	if (strcmp(g->type, PW_TYPE_INTERFACE_Device) == 0) {
		if (g->props == NULL)
			return 0;
		if ((str = pw_properties_get(g->props, PW_KEY_MEDIA_CLASS)) == NULL)
			return 0;
		if (strcmp(str, "Audio/Device") != 0)
			return 0;

		pw_log_debug("found card %d", g->id);
		g->mask = PA_SUBSCRIPTION_MASK_CARD;
		g->event = PA_SUBSCRIPTION_EVENT_CARD;
		ginfo = &device_info;
		spa_list_init(&g->card_info.profiles);
		spa_list_init(&g->card_info.ports);
		spa_list_init(&g->card_info.routes);
	} else if (strcmp(g->type, PW_TYPE_INTERFACE_Node) == 0) {
		if (g->props == NULL)
			return 0;

		if ((str = pw_properties_get(g->props, PW_KEY_PRIORITY_DRIVER)) != NULL)
			g->priority_driver = pw_properties_parse_int(str);

		if ((str = pw_properties_get(g->props, PW_KEY_MEDIA_CLASS)) == NULL) {
			pw_log_debug("node %d without "PW_KEY_MEDIA_CLASS, g->id);
			return 0;
		}

		if (strcmp(str, "Audio/Sink") == 0) {
			pw_log_debug("found sink %d", g->id);
			g->mask = PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SOURCE;
			g->event = PA_SUBSCRIPTION_EVENT_SINK;
			g->node_info.monitor = g->id | PA_IDX_FLAG_MONITOR;
		}
		else if (strcmp(str, "Audio/Source") == 0) {
			pw_log_debug("found source %d", g->id);
			g->mask = PA_SUBSCRIPTION_MASK_SOURCE;
			g->event = PA_SUBSCRIPTION_EVENT_SOURCE;
		}
		else if (strcmp(str, "Stream/Output/Audio") == 0) {
			pw_log_debug("found sink input %d", g->id);
			g->mask = PA_SUBSCRIPTION_MASK_SINK_INPUT;
			g->event = PA_SUBSCRIPTION_EVENT_SINK_INPUT;
		}
		else if (strcmp(str, "Stream/Input/Audio") == 0) {
			pw_log_debug("found source output %d", g->id);
			g->mask = PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT;
			g->event = PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT;
		}
		g->stream = pa_context_find_stream(c, g->id);
		if (g->stream) {
			pw_log_debug("global stream %p", g->stream);
			g->stream->global = g;
		}

		if ((str = pw_properties_get(g->props, PW_KEY_CLIENT_ID)) != NULL)
			g->node_info.client_id = atoi(str);
		else
			g->node_info.client_id = SPA_ID_INVALID;

		if ((str = pw_properties_get(g->props, PW_KEY_DEVICE_ID)) != NULL)
			g->node_info.device_id = atoi(str);
		else
			g->node_info.device_id = SPA_ID_INVALID;

		ginfo = &node_info;
		g->node_info.device_index = SPA_ID_INVALID;
		g->node_info.sample_spec.format = PA_SAMPLE_S16NE;
		g->node_info.sample_spec.rate = 44100;
		g->node_info.volume = 1.0f;
		g->node_info.mute = false;
		g->node_info.base_volume = 1.0f;
		g->node_info.volume_step = 1.0f / (PA_VOLUME_NORM+1);
		g->node_info.active_port = SPA_ID_INVALID;
		g->node_info.available_port = SPA_PARAM_AVAILABILITY_unknown;
		pw_array_init(&g->node_info.formats, sizeof(void*) * 4);
	} else if (strcmp(g->type, PW_TYPE_INTERFACE_Port) == 0) {
		if (g->props == NULL)
			return 0;

		if ((str = pw_properties_get(g->props, PW_KEY_NODE_ID)) == NULL) {
			pw_log_warn("port %d without "PW_KEY_NODE_ID, g->id);
			return 0;
		}
		g->port_info.node_id = atoi(str);
		pw_log_debug("found port %d node %d", g->id, g->port_info.node_id);
	} else if (strcmp(g->type, PW_TYPE_INTERFACE_Module) == 0) {
		pw_log_debug("found module %d", g->id);
		g->mask = PA_SUBSCRIPTION_MASK_MODULE;
		g->event = PA_SUBSCRIPTION_EVENT_MODULE;
		ginfo = &module_info;
	} else if (strcmp(g->type, PW_TYPE_INTERFACE_Client) == 0) {
		pw_log_debug("found client %d", g->id);
		g->mask = PA_SUBSCRIPTION_MASK_CLIENT;
		g->event = PA_SUBSCRIPTION_EVENT_CLIENT;
		ginfo = &client_info;
	} else if (strcmp(g->type, PW_TYPE_INTERFACE_Link) == 0) {
		uint32_t src_node_id, dst_node_id;

                if ((str = pw_properties_get(g->props, PW_KEY_LINK_OUTPUT_PORT)) == NULL)
			return 0;
		g->link_info.src = pa_context_find_global(c, pw_properties_parse_int(str));
                if ((str = pw_properties_get(g->props, PW_KEY_LINK_INPUT_PORT)) == NULL)
			return 0;
		g->link_info.dst = pa_context_find_global(c, pw_properties_parse_int(str));

		if (g->link_info.src == NULL || g->link_info.dst == NULL)
			return 0;

		src_node_id = g->link_info.src->port_info.node_id;
		dst_node_id = g->link_info.dst->port_info.node_id;

		pw_log_debug("link %d:%d->%d:%d",
				src_node_id, g->link_info.src->id,
				dst_node_id, g->link_info.dst->id);

		update_link(c, src_node_id, dst_node_id);
	} else if (strcmp(g->type, PW_TYPE_INTERFACE_Metadata) == 0) {
		if (c->metadata == NULL) {
			ginfo = &metadata_info;
			c->metadata = g;
			g->mask = PA_SUBSCRIPTION_MASK_SERVER;
			g->event = PA_SUBSCRIPTION_EVENT_SERVER;
		}
		pw_array_init(&g->metadata_info.metadata, 64);
	} else {
		return 0;
	}

	pw_log_debug("global %p: id:%u mask %d/%d", g, g->id, g->mask, g->event);

	if (ginfo) {
		pw_log_debug("bind %d", g->id);

		g->proxy = pw_registry_bind(c->registry, g->id, g->type,
	                                      ginfo->version, 0);
		if (g->proxy == NULL)
	                return -ENOMEM;

		pw_proxy_add_object_listener(g->proxy, &g->object_listener, ginfo->events, g);
		pw_proxy_add_listener(g->proxy, &g->proxy_listener, &proxy_events, g);
		g->ginfo = ginfo;
		global_sync(g);
	} else {
		emit_event(c, g, PA_SUBSCRIPTION_EVENT_NEW);
	}

	return 1;
}

static inline void insert_global(pa_context *c, struct global *global)
{
	struct global *g;
	bool found = false;

	spa_list_for_each(g, &c->globals, link) {
		if (g->priority_driver < global->priority_driver) {
			g = spa_list_prev(g, link);
			found = true;
			break;
		}
	}
	if (!found)
		spa_list_append(&g->link, &global->link);
	else
		spa_list_prepend(&g->link, &global->link);
}

static void registry_event_global(void *data, uint32_t id,
                                  uint32_t permissions, const char *type, uint32_t version,
                                  const struct spa_dict *props)
{
	pa_context *c = data;
	struct global *g;
	int res;

	g = calloc(1, sizeof(struct global));
	pw_log_debug("context %p: global %d %s %p", c, id, type, g);
	g->context = c;
	g->id = id;
	g->permissions = permissions;
	g->type = strdup(type);
	g->init = true;
	g->props = props ? pw_properties_new_dict(props) : NULL;

	res = set_mask(c, g);
	insert_global(c, g);

	if (res != 1)
		global_free(c, g);
}

static void registry_event_global_remove(void *object, uint32_t id)
{
	pa_context *c = object;
	struct global *g;

	pw_log_debug("context %p: remove %d", c, id);
	if ((g = pa_context_find_global(c, id)) == NULL)
		return;

	emit_event(c, g, PA_SUBSCRIPTION_EVENT_REMOVE);

	pw_log_debug("context %p: free %d %p", c, id, g);
	global_free(c, g);
}

static const struct pw_registry_events registry_events =
{
	PW_VERSION_REGISTRY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

static void core_info(void *data, const struct pw_core_info *info)
{
	pa_context *c = data;
	bool first = c->core_info == NULL;

	pw_log_debug("context %p: info", c);

	if (first) {
		pa_context_set_state(c, PA_CONTEXT_AUTHORIZING);
		pa_context_set_state(c, PA_CONTEXT_SETTING_NAME);
	}

	c->core_info = pw_core_info_update(c->core_info, info);

	if (first)
		pa_context_set_state(c, PA_CONTEXT_READY);
}

static void core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
	pa_context *c = data;

	pw_log_error("context %p: error id:%u seq:%d res:%d (%s): %s", c,
			id, seq, res, spa_strerror(res), message);

	if (id == PW_ID_CORE) {
		if (res == -EPIPE && !c->disconnect)
			pa_context_fail(c, PA_ERR_CONNECTIONTERMINATED);
	}
}

static void core_done(void *data, uint32_t id, int seq)
{
	pa_context *c = data;
	pa_operation *o, *t;
	struct global *g;
	struct spa_list ops;

	pw_log_debug("done id:%u seq:%d/%d", id, seq, c->pending_seq);
	if (c->pending_seq != seq)
		return;

	spa_list_for_each(g, &c->globals, link) {
		if (g->sync) {
			do_global_sync(g);
			g->sync = false;
		}
	}
	if (c->pending_seq != seq)
		return;

	spa_list_init(&ops);
	spa_list_consume(o, &c->operations, link) {
		spa_list_remove(&o->link);
		spa_list_append(&ops, &o->link);
	}
	spa_list_for_each_safe(o, t, &ops, link) {
		if (!o->sync)
			continue;
		pa_operation_ref(o);
		pw_log_debug("sync operation %p complete", o);
		if (o->callback)
			o->callback(o, o->userdata);
		pa_operation_unref(o);
	}
	spa_list_consume(o, &ops, link) {
		if (!o->sync) {
			spa_list_remove(&o->link);
			spa_list_append(&c->operations, &o->link);
			continue;
		}
		pw_log_warn("operation %p canceled", o);
		pa_operation_cancel(o);
	}
}

static const struct pw_core_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.info = core_info,
	.done = core_done,
	.error = core_error
};

struct success_data {
	pa_context_success_cb_t cb;
	void *userdata;
	int error;
};

static void on_success(pa_operation *o, void *userdata)
{
	struct success_data *d = userdata;
	pa_context *c = o->context;
	pw_log_debug("context %p: operation:%p error %d", c, o, d->error);
	if (d->error != 0)
		pa_context_set_error(c, d->error);
	if (d->cb)
		d->cb(c, d->error ? 0 : 1, d->userdata);
	pa_operation_done(o);
}

struct subscribe_data {
	struct success_data success;
	pa_subscription_mask_t mask;
};

static void on_subscribe(pa_operation *o, void *userdata)
{
	struct subscribe_data *d = userdata;
	pa_context *c = o->context;
	c->subscribe_mask = d->mask;
	on_success(o, &d->success);
}

SPA_EXPORT
pa_operation* pa_context_subscribe(pa_context *c, pa_subscription_mask_t m, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct subscribe_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	pw_log_debug("context %p: subscribe %08x", c, m);

	o = pa_operation_new(c, NULL, on_subscribe, sizeof(struct subscribe_data));
	d = o->userdata;
	d->success.cb = cb;
	d->success.userdata = userdata;
	d->mask = m;
	pa_operation_sync(o);

	return o;
}

static void io_event_cb(pa_mainloop_api*ea, pa_io_event* e, int fd, pa_io_event_flags_t events, void *userdata)
{
	pa_context *c = userdata;
	if (events & PA_IO_EVENT_INPUT) {
		pw_log_debug("%p: iterate loop %p", c, c->loop);
		pw_loop_enter(c->loop);
		pw_loop_iterate(c->loop, -1);
		pw_loop_leave(c->loop);
	}
}

SPA_EXPORT
pa_context *pa_context_new_with_proplist(pa_mainloop_api *mainloop, const char *name, PA_CONST pa_proplist *p)
{
	struct pw_context *context;
	struct pw_loop *loop;
	struct pw_properties *props;
	bool fallback_loop = false;
	pa_context *c;

	pa_assert(mainloop);

	props = pw_properties_new(NULL, NULL);
	if (name)
		pw_properties_set(props, PA_PROP_APPLICATION_NAME, name);
	pw_properties_set(props, PW_KEY_CLIENT_API, "pulseaudio");
	if (p)
		pw_properties_update_proplist(props, p);

	if (pa_mainloop_api_is_pipewire(mainloop))
		loop = mainloop->userdata;
	else  {
		loop = pw_loop_new(NULL);
		fallback_loop = true;
	}

	pw_log_debug("mainloop:%p loop:%p", mainloop, loop);

	context = pw_context_new(loop,
			pw_properties_new(
				PW_KEY_CONTEXT_PROFILE_MODULES, "default",
				NULL),
			sizeof(struct pa_context));
	if (context == NULL)
		return NULL;

	c = pw_context_get_user_data(context);
	c->props = props;
	c->fallback_loop = fallback_loop;
	c->loop = loop;
	c->context = context;
	c->proplist = p ? pa_proplist_copy(p) : pa_proplist_new();
	c->refcount = 1;
	c->client_index = PA_INVALID_INDEX;
	c->default_sink = SPA_ID_INVALID;
	c->default_source = SPA_ID_INVALID;
	c->mainloop = mainloop;
	c->error = 0;
	c->state = PA_CONTEXT_UNCONNECTED;

	if (c->fallback_loop) {
		c->io = c->mainloop->io_new(c->mainloop,
				pw_loop_get_fd(c->loop),
				PA_IO_EVENT_INPUT,
				io_event_cb, c);
	}

	if (name)
		pa_proplist_sets(c->proplist, PA_PROP_APPLICATION_NAME, name);

	spa_list_init(&c->globals);

	spa_list_init(&c->streams);
	spa_list_init(&c->operations);
	spa_list_init(&c->modules);

	return c;
}

static void context_free(pa_context *c)
{
	struct pw_loop *loop;
	pw_log_debug("context %p: free", c);

	context_unlink(c);

	pw_properties_free(c->props);
	if (c->proplist)
		pa_proplist_free(c->proplist);
	if (c->core_info)
		pw_core_info_free(c->core_info);

	if (c->io)
		c->mainloop->io_free(c->io);
	loop = c->fallback_loop ? c->loop : NULL;

	pw_context_destroy(c->context);

	if (loop)
		pw_loop_destroy(loop);
}

SPA_EXPORT
void pa_context_unref(pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	if (--c->refcount == 0)
		context_free(c);
}

SPA_EXPORT
pa_context* pa_context_ref(pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);
	c->refcount++;
	return c;
}

SPA_EXPORT
void pa_context_set_state_callback(pa_context *c, pa_context_notify_cb_t cb, void *userdata)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	if (c->state == PA_CONTEXT_TERMINATED || c->state == PA_CONTEXT_FAILED)
		return;

	c->state_callback = cb;
	c->state_userdata = userdata;
}

SPA_EXPORT
void pa_context_set_event_callback(pa_context *c, pa_context_event_cb_t cb, void *userdata)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	if (c->state == PA_CONTEXT_TERMINATED || c->state == PA_CONTEXT_FAILED)
		return;

	c->event_callback = cb;
	c->event_userdata = userdata;
}

SPA_EXPORT
int pa_context_errno(PA_CONST pa_context *c)
{
	if (!c)
		return PA_ERR_INVALID;

	pa_assert(c->refcount >= 1);

	return c->error;
}

SPA_EXPORT
int pa_context_is_pending(PA_CONST pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY(c, PA_CONTEXT_IS_GOOD(c->state), PA_ERR_BADSTATE);

	return !spa_list_is_empty(&c->operations);
}

SPA_EXPORT
pa_context_state_t pa_context_get_state(PA_CONST pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);
	return c->state;
}

SPA_EXPORT
int pa_context_connect(pa_context *c, const char *server, pa_context_flags_t flags, const pa_spawn_api *api)
{
	int res = 0;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY(c, c->state == PA_CONTEXT_UNCONNECTED, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(c, !(flags & ~(PA_CONTEXT_NOAUTOSPAWN|PA_CONTEXT_NOFAIL)), PA_ERR_INVALID);
	PA_CHECK_VALIDITY(c, !server || *server, PA_ERR_INVALID);

	pa_context_ref(c);

	c->no_fail = !!(flags & PA_CONTEXT_NOFAIL);

	pa_context_set_state(c, PA_CONTEXT_CONNECTING);

	if (server)
		pw_properties_set(c->props, PW_KEY_REMOTE_NAME, server);

	c->core = pw_context_connect(c->context, pw_properties_copy(c->props), 0);
	if (c->core == NULL) {
                pa_context_fail(c, PA_ERR_CONNECTIONREFUSED);
		res = -1;
		goto exit;
	}
	pw_core_add_listener(c->core, &c->core_listener, &core_events, c);

	c->registry = pw_core_get_registry(c->core,
			PW_VERSION_REGISTRY, 0);
	pw_registry_add_listener(c->registry,
			&c->registry_listener,
			&registry_events, c);

exit:
	pa_context_unref(c);

	return res;
}

SPA_EXPORT
void pa_context_disconnect(pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	c->disconnect = true;
	if (c->registry) {
		pw_proxy_destroy((struct pw_proxy*)c->registry);
		c->registry = NULL;
	}
	if (c->core) {
		pw_core_disconnect(c->core);
		c->core = NULL;
	}
	if (PA_CONTEXT_IS_GOOD(c->state))
		pa_context_set_state(c, PA_CONTEXT_TERMINATED);
}

struct notify_data {
	pa_context_notify_cb_t cb;
	void *userdata;
};

static void on_notify(pa_operation *o, void *userdata)
{
	struct notify_data *d = userdata;
	pa_context *c = o->context;

	pw_log_debug("%p", c);

	if (d->cb)
		d->cb(c, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_drain(pa_context *c, pa_context_notify_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct notify_data *d;

	o = pa_operation_new(c, NULL, on_notify, sizeof(struct notify_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_exit_daemon(pa_context *c, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_data *d;

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_data));
	d = o->userdata;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);
	pw_log_warn("Not Implemented");

	return o;
}

struct default_node {
	uint32_t mask;
	pa_context_success_cb_t cb;
	void *userdata;
	char *name;
	const char *key;
};

static void do_default_node(pa_operation *o, void *userdata)
{
	struct default_node *d = userdata;
	pa_context *c = o->context;
	struct global *g;
	int error = 0;

	pw_log_debug("%p mask:%d name:%s", c, d->mask, d->name);

	g = pa_context_find_global_by_name(c, d->mask, d->name);
	if (g == NULL) {
		error = PA_ERR_NOENTITY;
	} else if (!SPA_FLAG_IS_SET(g->permissions, PW_PERM_M) ||
		(c->metadata && !SPA_FLAG_IS_SET(c->metadata->permissions, PW_PERM_W|PW_PERM_X))) {
		error = PA_ERR_ACCESS;
	} else if (c->metadata) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%d", g->id);
		pw_metadata_set_property(c->metadata->proxy,
				PW_ID_CORE, d->key, SPA_TYPE_INFO_BASE"Id", buf);
	} else {
		error = PA_ERR_NOTIMPLEMENTED;
	}
	if (error != 0)
		pa_context_set_error(c, error);
	if (d->cb)
		d->cb(c, error != 0 ? 0 : 1, d->userdata);
	pa_xfree(d->name);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_set_default_sink(pa_context *c, const char *name, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct default_node *d;

	o = pa_operation_new(c, NULL, do_default_node, sizeof(*d));
	d = o->userdata;
	d->mask = PA_SUBSCRIPTION_MASK_SINK;
	d->name = pa_xstrdup(name);
	d->key = METADATA_DEFAULT_SINK;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_default_source(pa_context *c, const char *name, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct default_node *d;

	o = pa_operation_new(c, NULL, do_default_node, sizeof(*d));
	d = o->userdata;
	d->mask = PA_SUBSCRIPTION_MASK_SOURCE;
	d->name = pa_xstrdup(name);
	d->key = METADATA_DEFAULT_SOURCE;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
int pa_context_is_local(PA_CONST pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(c, PA_CONTEXT_IS_GOOD(c->state), PA_ERR_BADSTATE, -1);

	return 1;
}

SPA_EXPORT
pa_operation* pa_context_set_name(pa_context *c, const char *name, pa_context_success_cb_t cb, void *userdata)
{
	struct spa_dict dict;
	struct spa_dict_item items[1];
	pa_operation *o;
	struct success_data *d;
	int changed;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(name);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	items[0] = SPA_DICT_ITEM_INIT(PA_PROP_APPLICATION_NAME, name);
	dict = SPA_DICT_INIT(items, 1);
	changed = pw_properties_update(c->props, &dict);

	if (changed) {
		struct pw_client *client;

		client = pw_core_get_client(c->core);
		pw_client_update_properties(client, &c->props->dict);
	}

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
const char* pa_context_get_server(PA_CONST pa_context *c)
{
	const struct pw_core_info *info;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	info = c->core_info;
	PA_CHECK_VALIDITY_RETURN_NULL(c, info && info->name, PA_ERR_NOENTITY);

	return info->name;
}

SPA_EXPORT
uint32_t pa_context_get_protocol_version(PA_CONST pa_context *c)
{
	return PA_PROTOCOL_VERSION;
}

SPA_EXPORT
uint32_t pa_context_get_server_protocol_version(PA_CONST pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(c, PA_CONTEXT_IS_GOOD(c->state), PA_ERR_BADSTATE, PA_INVALID_INDEX);

	return PA_PROTOCOL_VERSION;
}

SPA_EXPORT
pa_operation *pa_context_proplist_update(pa_context *c, pa_update_mode_t mode, PA_CONST pa_proplist *p, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_data *d;

	spa_assert(c);
	spa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, mode == PA_UPDATE_SET ||
			mode == PA_UPDATE_MERGE || mode == PA_UPDATE_REPLACE, PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	pa_proplist_update(c->proplist, mode, p);

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
pa_operation *pa_context_proplist_remove(pa_context *c, const char *const keys[], pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_data *d;

	spa_assert(c);
	spa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, keys && keys[0], PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	pw_log_warn("Not Implemented");

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
uint32_t pa_context_get_index(PA_CONST pa_context *c)
{
	struct pw_client *client;

	pa_assert(c);
	spa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE, PA_INVALID_INDEX);
	client = pw_core_get_client(c->core);
	if (client == NULL)
		return PA_INVALID_INDEX;

	return pw_proxy_get_bound_id((struct pw_proxy*)client);
}

SPA_EXPORT
pa_time_event* pa_context_rttime_new(PA_CONST pa_context *c, pa_usec_t usec, pa_time_event_cb_t cb, void *userdata)
{
	struct timeval tv;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(c->mainloop);

	if (usec == PA_USEC_INVALID)
		return c->mainloop->time_new(c->mainloop, NULL, cb, userdata);

	pa_timeval_store(&tv, usec);

	return c->mainloop->time_new(c->mainloop, &tv, cb, userdata);
}

SPA_EXPORT
void pa_context_rttime_restart(PA_CONST pa_context *c, pa_time_event *e, pa_usec_t usec)
{
	struct timeval tv;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(c->mainloop);

	if (usec == PA_USEC_INVALID)
		c->mainloop->time_restart(e, NULL);
	else {
		pa_timeval_store(&tv, usec);
		c->mainloop->time_restart(e, &tv);
	}
}

SPA_EXPORT
size_t pa_context_get_tile_size(PA_CONST pa_context *c, const pa_sample_spec *ss)
{
	size_t fs, mbs;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(c, !ss || pa_sample_spec_valid(ss), PA_ERR_INVALID, (size_t) -1);

	fs = ss ? pa_frame_size(ss) : 1;
	mbs = PA_ROUND_DOWN(4096, fs);
	return PA_MAX(mbs, fs);
}

SPA_EXPORT
int pa_context_load_cookie_from_file(pa_context *c, const char *cookie_file_path)
{
	return 0;
}
