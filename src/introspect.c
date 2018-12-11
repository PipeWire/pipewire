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

#define _GNU_SOURCE

#include <errno.h>

#include <spa/param/props.h>

#include <pipewire/log.h>
#include <pipewire/stream.h>

#include <pulse/introspect.h>

#include "internal.h"

static void node_event_info(void *object, struct pw_node_info *info)
{
	struct global *g = object;
	pw_log_debug("update %d", g->id);
	g->info = pw_node_info_update(g->info, info);
}

static void node_event_param(void *object,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct global *g = object;
	pw_log_debug("update param %d", g->id);
}

static const struct pw_node_proxy_events node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
	.info = node_event_info,
	.param = node_event_param,
};

static void module_event_info(void *object, struct pw_module_info *info)
{
        struct global *g = object;
	pa_module_info *i = &g->module_info.info;

	pw_log_debug("update %d", g->id);

        info = g->info = pw_module_info_update(g->info, info);

	i->index = g->id;
	if (info->change_mask & PW_MODULE_CHANGE_MASK_PROPS) {
		if (i->proplist)
			pa_proplist_update_dict(i->proplist, info->props);
		else
			i->proplist = pa_proplist_new_dict(info->props);
	}

	if (info->change_mask & PW_MODULE_CHANGE_MASK_NAME)
		i->name = info->name;
	if (info->change_mask & PW_MODULE_CHANGE_MASK_ARGS)
		i->argument = info->args;
	i->n_used = -1;
	i->auto_unload = false;
}

static const struct pw_module_proxy_events module_events = {
	PW_VERSION_MODULE_PROXY_EVENTS,
	.info = module_event_info,
};

static void client_event_info(void *object, struct pw_client_info *info)
{
        struct global *g = object;
	pa_client_info *i = &g->client_info.info;

	pw_log_debug("update %d", g->id);
	info = g->info = pw_client_info_update(g->info, info);

	i->index = g->id;
	i->owner_module = g->parent_id;

	if (info->change_mask & PW_CLIENT_CHANGE_MASK_PROPS) {
		if (i->proplist)
			pa_proplist_update_dict(i->proplist, info->props);
		else
			i->proplist = pa_proplist_new_dict(info->props);
		i->name = info->props ?
			spa_dict_lookup(info->props, "application.name") : NULL;
		i->driver = info->props ?
			spa_dict_lookup(info->props, PW_CLIENT_PROP_PROTOCOL) : NULL;
	}
}

static const struct pw_client_proxy_events client_events = {
	PW_VERSION_CLIENT_PROXY_EVENTS,
	.info = client_event_info,
};

static void device_event_param(void *object,
		uint32_t id, uint32_t index, uint32_t next,
		const struct spa_pod *param)
{
	struct global *g = object;

	switch (id) {
	case SPA_PARAM_EnumProfile:
	{
		uint32_t id;
		const char *name;

		if (spa_pod_object_parse(param,
				":", SPA_PARAM_PROFILE_id, "i", &id,
				":", SPA_PARAM_PROFILE_name, "s", &name,
				NULL) < 0) {
			pw_log_warn("device %d: can't parse profile", g->id);
			return;
		}
		pw_array_add_ptr(&g->card_info.profiles, pw_spa_pod_copy(param));
		pw_log_debug("device %d: enum profile %d: \"%s\"", g->id, id, name);
		break;
	}
	case SPA_PARAM_Profile:
	{
		uint32_t id;
		if (spa_pod_object_parse(param,
				":", SPA_PARAM_PROFILE_id, "i", &id,
				NULL) < 0) {
			pw_log_warn("device %d: can't parse profile", g->id);
			return;
		}
		g->card_info.active_profile = id;
		pw_log_debug("device %d: current profile %d", g->id, id);
		break;
	}
	default:
		break;
	}
}

static void device_event_info(void *object, struct pw_device_info *info)
{
        struct global *g = object;
	pa_card_info *i = &g->card_info.info;

	pw_log_debug("update %d", g->id);
        info = g->info = pw_device_info_update(g->info, info);

	i->index = g->id;
	i->name = info->name;
	i->owner_module = g->parent_id;
	if (info->change_mask & PW_DEVICE_CHANGE_MASK_PROPS) {
		i->driver = info->props ?
			spa_dict_lookup(info->props, "device.api") : NULL;
		if (i->proplist)
			pa_proplist_update_dict(i->proplist, info->props);
		else
			i->proplist = pa_proplist_new_dict(info->props);
	}
}

static const struct pw_device_proxy_events device_events = {
	PW_VERSION_DEVICE_PROXY_EVENTS,
	.info = device_event_info,
	.param = device_event_param,
};

static void node_destroy(void *data)
{
	struct global *global = data;
	if (global->info)
		pw_node_info_free(global->info);
}

static void module_destroy(void *data)
{
	struct global *global = data;
	if (global->module_info.info.proplist)
		pa_proplist_free(global->module_info.info.proplist);
	if (global->info)
		pw_module_info_free(global->info);
}

static void client_destroy(void *data)
{
	struct global *global = data;
	if (global->client_info.info.proplist)
		pa_proplist_free(global->client_info.info.proplist);
	if (global->info)
		pw_client_info_free(global->info);
}

static void device_destroy(void *data)
{
	struct global *global = data;
	struct spa_pod *profile;

	if (global->card_info.info.proplist)
		pa_proplist_free(global->card_info.info.proplist);
	pw_array_for_each(profile, &global->card_info.profiles)
		free(profile);
	pw_array_clear(&global->card_info.profiles);
	if (global->info)
		pw_device_info_free(global->info);
}

static int ensure_global(pa_context *c, struct global *g)
{
	uint32_t client_version;
	const void *events;
	pw_destroy_t destroy;

	if (g->proxy != NULL)
		return 0;

	switch (g->type) {
	case PW_TYPE_INTERFACE_Node:
		events = &node_events;
                client_version = PW_VERSION_NODE;
                destroy = node_destroy;
		break;
	case PW_TYPE_INTERFACE_Module:
		events = &module_events;
                client_version = PW_VERSION_MODULE;
                destroy = module_destroy;
		break;
	case PW_TYPE_INTERFACE_Client:
		events = &client_events;
                client_version = PW_VERSION_CLIENT;
                destroy = client_destroy;
		break;
	case PW_TYPE_INTERFACE_Device:
		events = &device_events;
                client_version = PW_VERSION_DEVICE;
                destroy = device_destroy;
		pw_array_init(&g->card_info.profiles, 64);
		break;
	default:
		return -EINVAL;
	}

	pw_log_debug("bind %d", g->id);

	g->proxy = pw_registry_proxy_bind(c->registry_proxy, g->id, g->type,
                                      client_version, 0);
	if (g->proxy == NULL)
                return -ENOMEM;

	pw_proxy_add_proxy_listener(g->proxy, &g->proxy_proxy_listener, events, g);
	g->destroy = destroy;

	switch (g->type) {
	case PW_TYPE_INTERFACE_Node:
		pw_node_proxy_enum_params((struct pw_node_proxy*)g->proxy,
				SPA_PARAM_EnumFormat, 0, -1, NULL);
		break;
	case PW_TYPE_INTERFACE_Device:
		pw_device_proxy_enum_params((struct pw_device_proxy*)g->proxy,
				SPA_PARAM_EnumProfile, 0, -1, NULL);
		pw_device_proxy_enum_params((struct pw_device_proxy*)g->proxy,
				SPA_PARAM_Profile, 0, -1, NULL);
		break;
	default:
		break;
	}
	return 0;
}

static void ensure_types(pa_context *c, uint32_t mask)
{
	struct global *g;
	spa_list_for_each(g, &c->globals, link) {
		if (g->mask & mask)
			ensure_global(c, g);
	}
}

struct success_ack {
	pa_context_success_cb_t cb;
	void *userdata;
};

static void on_success(pa_operation *o, void *userdata)
{
	struct success_ack *d = userdata;
	pa_context *c = o->context;
	if (d->cb)
		d->cb(c, PA_OK, d->userdata);
	pa_operation_done(o);
}

struct sink_data {
	pa_context *context;
	pa_sink_info_cb_t cb;
	void *userdata;
	struct global *global;
};

static pa_sink_state_t node_state_to_sink(enum pw_node_state s)
{
	switch(s) {
	case PW_NODE_STATE_ERROR:
		return PA_SINK_UNLINKED;
	case PW_NODE_STATE_CREATING:
		return PA_SINK_INIT;
	case PW_NODE_STATE_SUSPENDED:
		return PA_SINK_SUSPENDED;
	case PW_NODE_STATE_IDLE:
		return PA_SINK_IDLE;
	case PW_NODE_STATE_RUNNING:
		return PA_SINK_RUNNING;
	default:
		return PA_SINK_INVALID_STATE;
	}
}

static void sink_callback(struct sink_data *d)
{
	struct global *g = d->global;
	struct pw_node_info *info = g->info;
	pa_sink_info i;
	pa_format_info ii[1];
	pa_format_info *ip[1];

	pw_log_debug("sink %d %s monitor %d", g->id, info->name, g->node_info.monitor);

	spa_zero(i);
	i.name = info->name;
	i.index = g->id;
	i.description = info->name;
	i.sample_spec.format = PA_SAMPLE_S16LE;
	i.sample_spec.rate = 44100;
	i.sample_spec.channels = 2;
	pa_channel_map_init_auto(&i.channel_map, 2, PA_CHANNEL_MAP_DEFAULT);
	i.owner_module = g->parent_id;
	pa_cvolume_set(&i.volume, 2, PA_VOLUME_NORM);
	i.mute = false;
	i.monitor_source = g->node_info.monitor;
	i.monitor_source_name = "unknown";
	i.latency = 0;
	i.driver = "PipeWire";
	i.flags = 0;
	i.proplist = pa_proplist_new_dict(info->props);
	i.configured_latency = 0;
	i.base_volume = PA_VOLUME_NORM;
	i.state = node_state_to_sink(info->state);
	i.n_volume_steps = PA_VOLUME_NORM+1;
	i.card = PA_INVALID_INDEX;
	i.n_ports = 0;
	i.ports = NULL;
	i.active_port = NULL;
	i.n_formats = 1;
	ii[0].encoding = PA_ENCODING_PCM;
	ii[0].plist = pa_proplist_new();
	ip[0] = ii;
	i.formats = ip;
	d->cb(d->context, &i, 0, d->userdata);
	pa_proplist_free(i.proplist);
	pa_proplist_free(ii[0].plist);
}

static void sink_info(pa_operation *o, void *userdata)
{
	struct sink_data *d = userdata;
	sink_callback(d);
	d->cb(d->context, NULL, 1, d->userdata);
	pa_operation_done(o);
}

pa_operation* pa_context_get_sink_info_by_name(pa_context *c, const char *name, pa_sink_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct sink_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

	if ((g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_SINK, name)) == NULL)
		return NULL;

	ensure_global(c, g);

	o = pa_operation_new(c, NULL, sink_info, sizeof(struct sink_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	d->global = g;
	pa_operation_sync(o);
	return o;
}

pa_operation* pa_context_get_sink_info_by_index(pa_context *c, uint32_t idx, pa_sink_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct sink_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL)
		return NULL;
	if (!(g->mask & PA_SUBSCRIPTION_MASK_SINK))
		return NULL;

	ensure_global(c, g);

	o = pa_operation_new(c, NULL, sink_info, sizeof(struct sink_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	d->global = g;
	pa_operation_sync(o);
	return o;
}

static void sink_info_list(pa_operation *o, void *userdata)
{
	struct sink_data *d = userdata;
	pa_context *c = d->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SINK))
			continue;
		d->global = g;
		sink_callback(d);
	}
	d->cb(c, NULL, 1, d->userdata);
	pa_operation_done(o);
}

pa_operation* pa_context_get_sink_info_list(pa_context *c, pa_sink_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct sink_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	ensure_types(c, PA_SUBSCRIPTION_MASK_SINK);
	o = pa_operation_new(c, NULL, sink_info_list, sizeof(struct sink_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

pa_operation* pa_context_set_sink_volume_by_index(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented %d", idx);
	return NULL;
}

pa_operation* pa_context_set_sink_volume_by_name(pa_context *c, const char *name, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented %s", name);
	return NULL;
}

pa_operation* pa_context_set_sink_mute_by_index(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented %d", mute);
	return NULL;
}

pa_operation* pa_context_set_sink_mute_by_name(pa_context *c, const char *name, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented %s", name);
	return NULL;
}

pa_operation* pa_context_suspend_sink_by_name(pa_context *c, const char *sink_name, int suspend, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_suspend_sink_by_index(pa_context *c, uint32_t idx, int suspend,  pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_sink_port_by_index(pa_context *c, uint32_t idx, const char*port, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_sink_port_by_name(pa_context *c, const char*name, const char*port, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}


struct source_data {
	pa_context *context;
	pa_source_info_cb_t cb;
	void *userdata;
	struct global *global;
};

static pa_source_state_t node_state_to_source(enum pw_node_state s)
{
	switch(s) {
	case PW_NODE_STATE_ERROR:
		return PA_SOURCE_UNLINKED;
	case PW_NODE_STATE_CREATING:
		return PA_SOURCE_INIT;
	case PW_NODE_STATE_SUSPENDED:
		return PA_SOURCE_SUSPENDED;
	case PW_NODE_STATE_IDLE:
		return PA_SOURCE_IDLE;
	case PW_NODE_STATE_RUNNING:
		return PA_SOURCE_RUNNING;
	default:
		return PA_SOURCE_INVALID_STATE;
	}
}
static void source_callback(struct source_data *d)
{
	struct global *g = d->global;
	struct pw_node_info *info = g->info;
	pa_source_info i;
	pa_format_info ii[1];
	pa_format_info *ip[1];

	spa_zero(i);
	i.name = info->name;
	i.index = g->id;
	i.description = info->name;
	i.sample_spec.format = PA_SAMPLE_S16LE;
	i.sample_spec.rate = 44100;
	i.sample_spec.channels = 2;
	pa_channel_map_init_auto(&i.channel_map, 2, PA_CHANNEL_MAP_DEFAULT);
	i.owner_module = g->parent_id;
	pa_cvolume_set(&i.volume, 2, PA_VOLUME_NORM);
	i.mute = false;
	if (g->mask & PA_SUBSCRIPTION_MASK_DSP_SINK) {
		i.monitor_of_sink = g->dsp_info.session;
		i.monitor_of_sink_name = "unknown";
	} else {
		i.monitor_of_sink = PA_INVALID_INDEX;
		i.monitor_of_sink_name = NULL;
	}
	i.latency = 0;
	i.driver = "PipeWire";
	i.flags = 0;
	i.proplist = pa_proplist_new_dict(info->props);
	i.configured_latency = 0;
	i.base_volume = PA_VOLUME_NORM;
	i.state = node_state_to_source(info->state);
	i.n_volume_steps = PA_VOLUME_NORM+1;
	i.card = PA_INVALID_INDEX;
	i.n_ports = 0;
	i.ports = NULL;
	i.active_port = NULL;
	i.n_formats = 1;
	ii[0].encoding = PA_ENCODING_PCM;
	ii[0].plist = pa_proplist_new();
	ip[0] = ii;
	i.formats = ip;
	d->cb(d->context, &i, 0, d->userdata);
	pa_proplist_free(i.proplist);
	pa_proplist_free(ii[0].plist);
}

static void source_info(pa_operation *o, void *userdata)
{
	struct source_data *d = userdata;
	source_callback(d);
	d->cb(d->context, NULL, 1, d->userdata);
	pa_operation_done(o);
}

pa_operation* pa_context_get_source_info_by_name(pa_context *c, const char *name, pa_source_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct source_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

	if ((g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_SOURCE, name)) == NULL)
		return NULL;

	ensure_global(c, g);

	o = pa_operation_new(c, NULL, source_info, sizeof(struct source_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	d->global = g;
	pa_operation_sync(o);
	return o;
}

pa_operation* pa_context_get_source_info_by_index(pa_context *c, uint32_t idx, pa_source_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct source_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL)
		return NULL;
	if (!(g->mask & PA_SUBSCRIPTION_MASK_SOURCE))
		return NULL;

	ensure_global(c, g);

	o = pa_operation_new(c, NULL, source_info, sizeof(struct source_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	d->global = g;
	pa_operation_sync(o);
	return o;
}

static void source_info_list(pa_operation *o, void *userdata)
{
	struct source_data *d = userdata;
	pa_context *c = d->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SOURCE))
			continue;
		d->global = g;
		source_callback(d);
	}
	d->cb(c, NULL, 1, d->userdata);
	pa_operation_done(o);
}

pa_operation* pa_context_get_source_info_list(pa_context *c, pa_source_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct source_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	ensure_types(c, PA_SUBSCRIPTION_MASK_SOURCE);
	o = pa_operation_new(c, NULL, source_info_list, sizeof(struct source_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

pa_operation* pa_context_set_source_volume_by_index(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_source_volume_by_name(pa_context *c, const char *name, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_source_mute_by_index(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_source_mute_by_name(pa_context *c, const char *name, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_suspend_source_by_name(pa_context *c, const char *source_name, int suspend, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_suspend_source_by_index(pa_context *c, uint32_t idx, int suspend, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_source_port_by_index(pa_context *c, uint32_t idx, const char*port, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_source_port_by_name(pa_context *c, const char*name, const char*port, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

struct server_data {
	pa_context *context;
	pa_server_info_cb_t cb;
	void *userdata;
	struct global *global;
};

static void server_callback(struct server_data *d)
{
	pa_context *c = d->context;
	const struct pw_core_info *info = pw_remote_get_core_info(c->remote);
	pa_server_info i;

	spa_zero(i);
	i.user_name = info->user_name;
	i.host_name = info->host_name;
	i.server_version = info->version;
	i.server_name = info->name;
	i.sample_spec.format = PA_SAMPLE_S16LE;
	i.sample_spec.rate = 44100;
	i.sample_spec.channels = 2;
	i.default_sink_name = "unknown";
	i.default_source_name = "unknown";
	i.cookie = info->cookie;
        pa_channel_map_init_extend(&i.channel_map, i.sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
	d->cb(d->context, &i, d->userdata);
}

static void server_info(pa_operation *o, void *userdata)
{
	struct server_data *d = userdata;
	server_callback(d);
	pa_operation_done(o);
}

pa_operation* pa_context_get_server_info(pa_context *c, pa_server_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct server_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	o = pa_operation_new(c, NULL, server_info, sizeof(struct server_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

struct module_data {
	pa_context *context;
	pa_module_info_cb_t cb;
	void *userdata;
	struct global *global;
};

static void module_callback(struct module_data *d)
{
	struct global *g = d->global;
	d->cb(d->context, &g->module_info.info, 0, d->userdata);
}

static void module_info(pa_operation *o, void *userdata)
{
	struct module_data *d = userdata;
	module_callback(d);
	d->cb(d->context, NULL, 1, d->userdata);
	pa_operation_done(o);
}

pa_operation* pa_context_get_module_info(pa_context *c, uint32_t idx, pa_module_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct module_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL)
		return NULL;
	if (!(g->mask & PA_SUBSCRIPTION_MASK_MODULE))
		return NULL;

	ensure_global(c, g);

	o = pa_operation_new(c, NULL, module_info, sizeof(struct module_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	d->global = g;
	pa_operation_sync(o);

	return o;
}

static void module_info_list(pa_operation *o, void *userdata)
{
	struct module_data *d = userdata;
	pa_context *c = d->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_MODULE))
			continue;
		d->global = g;
		module_callback(d);
	}
	d->cb(c, NULL, 1, d->userdata);
	pa_operation_done(o);
}

pa_operation* pa_context_get_module_info_list(pa_context *c, pa_module_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct module_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	ensure_types(c, PA_SUBSCRIPTION_MASK_MODULE);
	o = pa_operation_new(c, NULL, module_info_list, sizeof(struct module_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

pa_operation* pa_context_load_module(pa_context *c, const char*name, const char *argument, pa_context_index_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_unload_module(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

struct client_data {
	pa_context *context;
	pa_client_info_cb_t cb;
	void *userdata;
	struct global *global;
};

static void client_callback(struct client_data *d)
{
	struct global *g = d->global;
	d->cb(d->context, &g->client_info.info, 0, d->userdata);
}

static void client_info(pa_operation *o, void *userdata)
{
	struct client_data *d = userdata;
	client_callback(d);
	d->cb(d->context, NULL, 1, d->userdata);
	pa_operation_done(o);
}

pa_operation* pa_context_get_client_info(pa_context *c, uint32_t idx, pa_client_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct client_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL)
		return NULL;
	if (!(g->mask & PA_SUBSCRIPTION_MASK_CLIENT))
		return NULL;

	ensure_global(c, g);

	o = pa_operation_new(c, NULL, client_info, sizeof(struct client_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	d->global = g;
	pa_operation_sync(o);

	return o;
}

static void client_info_list(pa_operation *o, void *userdata)
{
	struct client_data *d = userdata;
	pa_context *c = d->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_CLIENT))
			continue;
		d->global = g;
		client_callback(d);
	}
	d->cb(c, NULL, 1, d->userdata);
	pa_operation_done(o);
}

pa_operation* pa_context_get_client_info_list(pa_context *c, pa_client_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct client_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	ensure_types(c, PA_SUBSCRIPTION_MASK_CLIENT);
	o = pa_operation_new(c, NULL, client_info_list, sizeof(struct client_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

pa_operation* pa_context_kill_client(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	struct global *g;
	pa_operation *o;
	struct success_ack *d;

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL)
		return NULL;
	if (!(g->mask & PA_SUBSCRIPTION_MASK_CLIENT))
		return NULL;

	pw_registry_proxy_destroy(c->registry_proxy, g->id);

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

struct card_data {
	pa_context *context;
	pa_card_info_cb_t cb;
	pa_context_success_cb_t success_cb;
	void *userdata;
	struct global *global;
	char *profile;
};

static void card_callback(struct card_data *d)
{
	struct global *g = d->global;
	pa_card_info *i = &g->card_info.info;
	int n_profiles, j;
	struct spa_pod **profiles;

	n_profiles = pw_array_get_len(&g->card_info.profiles, struct spa_pod*);
	profiles = g->card_info.profiles.data;

	i->profiles = alloca(sizeof(pa_card_profile_info) * n_profiles);
	i->profiles2 = alloca(sizeof(pa_card_profile_info2 *) * n_profiles);
	i->n_profiles = 0;

	for (j = 0; j < n_profiles; j++) {
		uint32_t id;
		const char *name;

		if (spa_pod_object_parse(profiles[j],
				":", SPA_PARAM_PROFILE_id, "i", &id,
				":", SPA_PARAM_PROFILE_name, "s", &name,
				NULL) < 0) {
			pw_log_warn("device %d: can't parse profile %d", g->id, j);
			continue;
		}

		i->profiles[j].name = name;
		i->profiles[j].description = name;
		i->profiles[j].n_sinks = 1;
		i->profiles[j].n_sources = 1;
		i->profiles[j].priority = 1;

		i->profiles2[j] = alloca(sizeof(pa_card_profile_info2));
		i->profiles2[j]->name = i->profiles[j].name;
		i->profiles2[j]->description = i->profiles[j].description;
	        i->profiles2[j]->n_sinks = i->profiles[j].n_sinks;
	        i->profiles2[j]->n_sources = i->profiles[j].n_sources;
	        i->profiles2[j]->priority = i->profiles[j].priority;
	        i->profiles2[j]->available = 1;

		if (g->card_info.active_profile == id) {
			i->active_profile = &i->profiles[j];
			i->active_profile2 = i->profiles2[j];
		}
		i->n_profiles++;
	}
	d->cb(d->context, i, 0, d->userdata);
}

static void card_info(pa_operation *o, void *userdata)
{
	struct card_data *d = userdata;
	card_callback(d);
	d->cb(d->context, NULL, 1, d->userdata);
	pa_operation_done(o);
}

pa_operation* pa_context_get_card_info_by_index(pa_context *c, uint32_t idx, pa_card_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct card_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL)
		return NULL;
	if (!(g->mask & PA_SUBSCRIPTION_MASK_CARD))
		return NULL;

	ensure_global(c, g);

	o = pa_operation_new(c, NULL, card_info, sizeof(struct card_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	d->global = g;
	pa_operation_sync(o);
	return o;
}

pa_operation* pa_context_get_card_info_by_name(pa_context *c, const char *name, pa_card_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct card_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

	if ((g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_CARD, name)) == NULL)
		return NULL;

	ensure_global(c, g);

	o = pa_operation_new(c, NULL, card_info, sizeof(struct card_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	d->global = g;
	pa_operation_sync(o);
	return o;
}

static void card_info_list(pa_operation *o, void *userdata)
{
	struct card_data *d = userdata;
	pa_context *c = d->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_CARD))
			continue;
		d->global = g;
		card_callback(d);
	}
	d->cb(c, NULL, 1, d->userdata);
	pa_operation_done(o);
}

pa_operation* pa_context_get_card_info_list(pa_context *c, pa_card_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct card_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	ensure_types(c, PA_SUBSCRIPTION_MASK_CARD);
	o = pa_operation_new(c, NULL, card_info_list, sizeof(struct card_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

static void card_profile(pa_operation *o, void *userdata)
{
	struct card_data *d = userdata;
	struct global *g = d->global;
	pa_context *c = d->context;
	int res = 0, n_profiles;
	uint32_t i, id = SPA_ID_INVALID;
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod **profiles;

	n_profiles = pw_array_get_len(&g->card_info.profiles, struct spa_pod*);
	profiles = g->card_info.profiles.data;

	for (i = 0; i < n_profiles; i++) {
		uint32_t test_id;
		const char *name;

		if (spa_pod_object_parse(profiles[i],
				":", SPA_PARAM_PROFILE_id, "i", &test_id,
				":", SPA_PARAM_PROFILE_name, "s", &name,
				NULL) < 0) {
			pw_log_warn("device %d: can't parse profile %d", g->id, i);
			continue;
		}
		if (strcmp(name, d->profile) == 0) {
			id = test_id;
			break;
		}
	}
	if (id == SPA_ID_INVALID)
		goto done;;

	pw_device_proxy_set_param((struct pw_device_proxy*)g->proxy,
			SPA_PARAM_Profile, 0,
			spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
				SPA_PARAM_PROFILE_id,         &SPA_POD_Int(id),
				0));
	res = 1;
done:
	if (d->success_cb)
		d->success_cb(c, res, d->userdata);
	pa_operation_done(o);
	free(d->profile);
}

pa_operation* pa_context_set_card_profile_by_index(pa_context *c, uint32_t idx, const char*profile, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct card_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL)
		return NULL;
	if (!(g->mask & PA_SUBSCRIPTION_MASK_CARD))
		return NULL;

	ensure_global(c, g);

	pw_log_debug("Card set profile %s", profile);

	o = pa_operation_new(c, NULL, card_profile, sizeof(struct card_data));
	d = o->userdata;
	d->context = c;
	d->success_cb = cb;
	d->userdata = userdata;
	d->global = g;
	d->profile = strdup(profile);
	pa_operation_sync(o);

	return o;
}

pa_operation* pa_context_set_card_profile_by_name(pa_context *c, const char*name, const char*profile, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_port_latency_offset(pa_context *c, const char *card_name, const char *port_name, int64_t offset, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

static pa_stream *find_stream(pa_context *c, uint32_t idx)
{
	pa_stream *s;
	spa_list_for_each(s, &c->streams, link) {
		if (pw_stream_get_node_id(s->stream) == idx)
			return s;
	}
	return NULL;
}

struct sink_input_data {
	pa_context *context;
	pa_sink_input_info_cb_t cb;
	void *userdata;
	struct global *global;
};

static void sink_input_callback(struct sink_input_data *d)
{
	struct global *g = d->global, *l, *cl;
	struct pw_node_info *info = g->info;
	const char *name;
	pa_sink_input_info i;
	pa_format_info ii[1];
	pa_stream *s;

	if (info == NULL)
		return;

	s = find_stream(d->context, g->id);

	if (info->props) {
		if ((name = spa_dict_lookup(info->props, "media.name")) == NULL &&
		    (name = spa_dict_lookup(info->props, "application.name")) == NULL)
			name = info->name;
	}
	else
		name = info->name;

	cl = pa_context_find_global(d->context, g->parent_id);

	spa_zero(i);
	i.index = g->id;
	i.name = name ? name : "Unknown";
	i.owner_module = PA_INVALID_INDEX;
	i.client = g->parent_id;
	if (s) {
		i.sink = s->device_index;
	}
	else {
		l = pa_context_find_linked(d->context, g->id);
		i.sink = l ? l->id : PA_INVALID_INDEX;
	}
	pa_cvolume_init(&i.volume);
	if (s && s->sample_spec.channels > 0) {
		i.sample_spec = s->sample_spec;
		if (s->channel_map.channels == s->sample_spec.channels)
			i.channel_map = s->channel_map;
		else
			pa_channel_map_init_auto(&i.channel_map,
					i.sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
		pa_cvolume_set(&i.volume, i.sample_spec.channels, s->volume * PA_VOLUME_NORM);
		i.format = s->format;
	}
	else {
		i.sample_spec.format = PA_SAMPLE_S16LE;
		i.sample_spec.rate = 44100;
		i.sample_spec.channels = 2;
		pa_channel_map_init_auto(&i.channel_map, i.sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
		pa_cvolume_set(&i.volume, i.sample_spec.channels, PA_VOLUME_NORM);
		ii[0].encoding = PA_ENCODING_PCM;
		ii[0].plist = pa_proplist_new();
		i.format = ii;
	}
	i.buffer_usec = 0;
	i.sink_usec = 0;
	i.resample_method = "PipeWire resampler";
	i.driver = "PipeWire";
	i.mute = false;
	i.proplist = pa_proplist_new_dict(info->props);
	if (cl && cl->client_info.info.proplist)
		pa_proplist_update(i.proplist, PA_UPDATE_MERGE, cl->client_info.info.proplist);
	i.corked = false;
	i.has_volume = true;
	i.volume_writable = true;

	d->cb(d->context, &i, 0, d->userdata);

	pa_proplist_free(i.proplist);
}

static void sink_input_info(pa_operation *o, void *userdata)
{
	struct sink_input_data *d = userdata;
	sink_input_callback(d);
	d->cb(d->context, NULL, 1, d->userdata);
	pa_operation_done(o);
}

pa_operation* pa_context_get_sink_input_info(pa_context *c, uint32_t idx, pa_sink_input_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct sink_input_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pw_log_debug("context %p: info for %d", c, idx);

	if ((g = pa_context_find_global(c, idx)) == NULL)
		return NULL;
	if (!(g->mask & PA_SUBSCRIPTION_MASK_SINK_INPUT))
		return NULL;

	ensure_global(c, g);

	o = pa_operation_new(c, NULL, sink_input_info, sizeof(struct sink_input_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	d->global = g;
	pa_operation_sync(o);
	return o;
}

static void sink_input_info_list(pa_operation *o, void *userdata)
{
	struct sink_input_data *d = userdata;
	pa_context *c = d->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SINK_INPUT))
			continue;
		d->global = g;
		sink_input_callback(d);
	}
	d->cb(c, NULL, 1, d->userdata);
	pa_operation_done(o);
}

pa_operation* pa_context_get_sink_input_info_list(pa_context *c, pa_sink_input_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct sink_input_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	pw_log_debug("context %p", c);

	ensure_types(c, PA_SUBSCRIPTION_MASK_SINK_INPUT);
	o = pa_operation_new(c, NULL, sink_input_info_list, sizeof(struct sink_input_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

pa_operation* pa_context_move_sink_input_by_name(pa_context *c, uint32_t idx, const char *sink_name, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_move_sink_input_by_index(pa_context *c, uint32_t idx, uint32_t sink_idx, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_sink_input_volume(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pa_stream *s;
	struct global *g;
	pa_operation *o;
	struct success_ack *d;
	float v;

	v = pa_cvolume_avg(volume) / (float) PA_VOLUME_NORM;

	pw_log_debug("contex %p: index %d volume %f", c, idx, v);

	if ((s = find_stream(c, idx)) == NULL) {
		if ((g = pa_context_find_global(c, idx)) == NULL)
			return NULL;
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SINK_INPUT))
			return NULL;
	}

	if (s) {
		s->volume = v;
		pw_stream_set_control(s->stream, PW_STREAM_CONTROL_VOLUME, s->mute ? 0.0 : s->volume);
	}
	else if (g) {
		char buf[1024];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

		pw_node_proxy_set_param((struct pw_node_proxy*)g->proxy,
			SPA_PARAM_Props, 0,
			spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_Props,	SPA_PARAM_Props,
				SPA_PROP_volume,	&SPA_POD_Float(v),
				0));
	}
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

pa_operation* pa_context_set_sink_input_mute(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pa_stream *s;
	struct global *g;
	pa_operation *o;
	struct success_ack *d;

	if ((s = find_stream(c, idx)) == NULL) {
		if ((g = pa_context_find_global(c, idx)) == NULL)
			return NULL;
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SINK_INPUT))
			return NULL;
	}

	if (s) {
		s->mute = mute;
		pw_stream_set_control(s->stream, PW_STREAM_CONTROL_VOLUME, s->mute ? 0.0 : s->volume);
	}
	else if (g) {
		char buf[1024];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

		pw_node_proxy_set_param((struct pw_node_proxy*)g->proxy,
			SPA_PARAM_Props, 0,
			spa_pod_builder_object(&b,
				SPA_TYPE_OBJECT_Props,	SPA_PARAM_Props,
				SPA_PROP_mute,		&SPA_POD_Bool(mute),
				0));
	}
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

pa_operation* pa_context_kill_sink_input(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	pa_stream *s;
	struct global *g;
	pa_operation *o;
	struct success_ack *d;

	if ((s = find_stream(c, idx)) == NULL) {
		if ((g = pa_context_find_global(c, idx)) == NULL)
			return NULL;
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SINK_INPUT))
			return NULL;
	}

	if (s) {
		pw_stream_destroy(s->stream);
	}
	else if (g) {
		pw_registry_proxy_destroy(c->registry_proxy, g->id);
	}
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

struct source_output_data {
	pa_context *context;
	pa_source_output_info_cb_t cb;
	void *userdata;
	struct global *global;
};

static void source_output_callback(struct source_output_data *d)
{
	struct global *g = d->global, *l, *cl;
	struct pw_node_info *info = g->info;
	const char *name;
	pa_source_output_info i;
	pa_format_info ii[1];
	pa_stream *s;

	pw_log_debug("index %d", g->id);
	if (info == NULL)
		return;

	s = find_stream(d->context, g->id);

	if (info->props) {
		if ((name = spa_dict_lookup(info->props, "media.name")) == NULL &&
		    (name = spa_dict_lookup(info->props, "application.name")) == NULL)
			name = info->name;
	}
	else
		name = info->name;

	cl = pa_context_find_global(d->context, g->parent_id);

	spa_zero(i);
	i.index = g->id;
	i.name = name ? name : "Unknown";
	i.owner_module = PA_INVALID_INDEX;
	i.client = g->parent_id;
	if (s) {
		i.source = s->device_index;
	}
	else {
		l = pa_context_find_linked(d->context, g->id);
		i.source = l ? l->id : PA_INVALID_INDEX;
	}
	pa_cvolume_init(&i.volume);
	if (s && s->sample_spec.channels > 0) {
		i.sample_spec = s->sample_spec;
		if (s->channel_map.channels == s->sample_spec.channels)
			i.channel_map = s->channel_map;
		else
			pa_channel_map_init_auto(&i.channel_map,
					i.sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
		pa_cvolume_set(&i.volume, i.sample_spec.channels, s->volume * PA_VOLUME_NORM);
		i.format = s->format;
	}
	else {
		i.sample_spec.format = PA_SAMPLE_S16LE;
		i.sample_spec.rate = 44100;
		i.sample_spec.channels = 2;
		pa_channel_map_init_auto(&i.channel_map, i.sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
		pa_cvolume_set(&i.volume, i.sample_spec.channels, PA_VOLUME_NORM);
		ii[0].encoding = PA_ENCODING_PCM;
		ii[0].plist = pa_proplist_new();
		i.format = ii;
	}
	i.buffer_usec = 0;
	i.source_usec = 0;
	i.resample_method = "PipeWire resampler";
	i.driver = "PipeWire";
	i.mute = false;
	i.proplist = pa_proplist_new_dict(info->props);
	if (cl && cl->client_info.info.proplist)
		pa_proplist_update(i.proplist, PA_UPDATE_MERGE, cl->client_info.info.proplist);
	i.corked = false;
	i.has_volume = true;
	i.volume_writable = true;

	d->cb(d->context, &i, 0, d->userdata);

	pa_proplist_free(i.proplist);
}

static void source_output_info(pa_operation *o, void *userdata)
{
	struct source_output_data *d = userdata;
	source_output_callback(d);
	d->cb(d->context, NULL, 1, d->userdata);
	pa_operation_done(o);
}

pa_operation* pa_context_get_source_output_info(pa_context *c, uint32_t idx, pa_source_output_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct source_output_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL)
		return NULL;
	if (!(g->mask & PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT))
		return NULL;

	ensure_global(c, g);

	o = pa_operation_new(c, NULL, source_output_info, sizeof(struct source_output_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	d->global = g;
	pa_operation_sync(o);

	return o;
}

static void source_output_info_list(pa_operation *o, void *userdata)
{
	struct source_output_data *d = userdata;
	pa_context *c = d->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT))
			continue;
		d->global = g;
		source_output_callback(d);
	}
	d->cb(c, NULL, 1, d->userdata);
	pa_operation_done(o);
}

pa_operation* pa_context_get_source_output_info_list(pa_context *c, pa_source_output_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct source_output_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	ensure_types(c, PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT);
	o = pa_operation_new(c, NULL, source_output_info_list, sizeof(struct source_output_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

pa_operation* pa_context_move_source_output_by_name(pa_context *c, uint32_t idx, const char *source_name, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_move_source_output_by_index(pa_context *c, uint32_t idx, uint32_t source_idx, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_source_output_volume(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_source_output_mute(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_kill_source_output(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	pa_stream *s;
	struct global *g;
	pa_operation *o;
	struct success_ack *d;

	if ((s = find_stream(c, idx)) == NULL) {
		if ((g = pa_context_find_global(c, idx)) == NULL)
			return NULL;
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT))
			return NULL;
	}

	if (s) {
		pw_stream_destroy(s->stream);
	}
	else if (g) {
		pw_registry_proxy_destroy(c->registry_proxy, g->id);
	}
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

pa_operation* pa_context_stat(pa_context *c, pa_stat_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_sample_info_by_name(pa_context *c, const char *name, pa_sample_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_sample_info_by_index(pa_context *c, uint32_t idx, pa_sample_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_sample_info_list(pa_context *c, pa_sample_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_autoload_info_by_name(pa_context *c, const char *name, pa_autoload_type_t type, pa_autoload_info_cb_t cb, void *userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_autoload_info_by_index(pa_context *c, uint32_t idx, pa_autoload_info_cb_t cb, void *userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}

pa_operation* pa_context_get_autoload_info_list(pa_context *c, pa_autoload_info_cb_t cb, void *userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}

pa_operation* pa_context_add_autoload(pa_context *c, const char *name, pa_autoload_type_t type, const char *module, const char*argument, pa_context_index_cb_t cb, void* userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}

pa_operation* pa_context_remove_autoload_by_name(pa_context *c, const char *name, pa_autoload_type_t type, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}

pa_operation* pa_context_remove_autoload_by_index(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}
