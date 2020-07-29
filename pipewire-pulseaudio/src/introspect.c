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

#include <spa/param/props.h>

#include <pipewire/pipewire.h>
#include <extensions/metadata.h>

#include <pulse/introspect.h>
#include <pulse/xmalloc.h>

#include "internal.h"

struct success_ack {
	pa_context_success_cb_t cb;
	int error;
	void *userdata;
	uint32_t idx;
};

static void on_success(pa_operation *o, void *userdata)
{
	struct success_ack *d = userdata;
	pa_context *c = o->context;
	pw_log_debug("error:%d", d->error);
	if (d->error != 0)
		pa_context_set_error(c, d->error);
	if (d->cb)
		d->cb(c, d->error ? 0 : 1, d->userdata);
	pa_operation_done(o);
}

struct sink_data {
	pa_sink_info_cb_t cb;
	void *userdata;
	char *name;
	uint32_t idx;
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

static int has_profile(pa_card_profile_info2 **list, pa_card_profile_info2 *active)
{
	for(;*list; list++) {
		if (*list == active)
			return 1;
	}
	return 0;
}

static int sink_callback(pa_context *c, struct global *g, struct sink_data *d)
{
	struct global *cg;
	struct pw_node_info *info = g->info;
	const char *str;
	uint32_t n, j;
	pa_sink_info i;
	pa_format_info ii[1];
	pa_format_info *ip[1];

	spa_zero(i);
	if (info->props && (str = spa_dict_lookup(info->props, PW_KEY_NODE_NAME)))
		i.name = str;
	else
		i.name = "unknown";
	pw_log_debug("sink %d %s monitor %d", g->id, i.name, g->node_info.monitor);
	i.index = g->id;
	if (info->props && (str = spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION)))
		i.description = str;
	else
		i.description = "Unknown";

	i.sample_spec = g->node_info.sample_spec;
	if (g->node_info.n_channel_volumes)
		i.sample_spec.channels = g->node_info.n_channel_volumes;
	else
		i.sample_spec.channels = 2;
	if (i.sample_spec.channels == g->node_info.channel_map.channels)
		i.channel_map = g->node_info.channel_map;
	else
		pa_channel_map_init_auto(&i.channel_map, i.sample_spec.channels, PA_CHANNEL_MAP_OSS);
	i.owner_module = 0;
	i.volume.channels = i.sample_spec.channels;
	for (n = 0; n < i.volume.channels; n++)
		i.volume.values[n] = g->node_info.volume * g->node_info.channel_volumes[n] * PA_VOLUME_NORM;
	i.mute = g->node_info.mute;
	i.monitor_source = g->node_info.monitor;
	i.monitor_source_name = pa_context_find_global_name(c, i.monitor_source);
	i.latency = 0;
	i.driver = "PipeWire";
	i.flags = PA_SINK_HARDWARE |
		  PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY |
		  PA_SINK_DECIBEL_VOLUME;
	if (SPA_FLAG_IS_SET(g->node_info.flags, NODE_FLAG_HW_VOLUME))
		  i.flags |= PA_SINK_HW_VOLUME_CTRL;
	if (SPA_FLAG_IS_SET(g->node_info.flags, NODE_FLAG_HW_MUTE))
		  i.flags |= PA_SINK_HW_MUTE_CTRL;
	i.proplist = pa_proplist_new_dict(info->props);
	i.configured_latency = 0;
	i.base_volume = g->node_info.base_volume * PA_VOLUME_NORM;
	i.n_volume_steps = g->node_info.volume_step * (PA_VOLUME_NORM+1);
	i.state = node_state_to_sink(info->state);
	i.card = g->node_info.device_id;
	i.n_ports = 0;
	i.ports = NULL;
	i.active_port = NULL;
	if ((cg = pa_context_find_global(c, i.card)) != NULL) {
		pa_sink_port_info *spi;
		pa_card_info *ci = &cg->card_info.info;

		spi = alloca(ci->n_ports * sizeof(pa_sink_port_info));
		i.ports = alloca((ci->n_ports + 1) * sizeof(pa_sink_port_info *));

		for (n = 0,j = 0; n < ci->n_ports; n++) {
			if (ci->ports[n]->direction != PA_DIRECTION_OUTPUT)
				continue;
			if (!has_profile(ci->ports[n]->profiles2, ci->active_profile2))
				continue;
			i.ports[j] = &spi[j];
			spi[j].name = ci->ports[n]->name;
			spi[j].description = ci->ports[n]->description;
			spi[j].priority = ci->ports[n]->priority;
			spi[j].available = ci->ports[n]->available;
			if (n == cg->card_info.active_port_output)
				i.active_port = i.ports[j];
			j++;
		}
		i.n_ports = j;
		if (i.n_ports == 0)
			i.ports = NULL;
		else
			i.ports[j] = NULL;
	}
	i.n_formats = 1;
	ii[0].encoding = PA_ENCODING_PCM;
	ii[0].plist = pa_proplist_new();
	ip[0] = ii;
	i.formats = ip;
	d->cb(c, &i, 0, d->userdata);
	pa_proplist_free(i.proplist);
	pa_proplist_free(ii[0].plist);
	return 0;
}

static void sink_info(pa_operation *o, void *userdata)
{
	struct sink_data *d = userdata;
	struct global *g;
	pa_context *c = o->context;
	int error = 0;

	if (d->name) {
		g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_SINK, d->name);
		pa_xfree(d->name);
	} else {
		if ((g = pa_context_find_global(c, d->idx)) == NULL ||
		    !(g->mask & PA_SUBSCRIPTION_MASK_SINK))
			g = NULL;
	}

	pw_log_debug("%p", c);

	if (g) {
		error = sink_callback(c, g, d);
	} else {
		error = PA_ERR_INVALID;
	}
	if (error)
		pa_context_set_error(c, error);
	d->cb(c, NULL, error ? -1 : 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_sink_info_by_name(pa_context *c, const char *name, pa_sink_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct sink_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

	pw_log_debug("%p", c);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, sink_info, sizeof(struct sink_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	d->name = pa_xstrdup(name);
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_get_sink_info_by_index(pa_context *c, uint32_t idx, pa_sink_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct sink_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, sink_info, sizeof(struct sink_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	d->idx = idx;
	pa_operation_sync(o);

	return o;
}

static void sink_info_list(pa_operation *o, void *userdata)
{
	struct sink_data *d = userdata;
	pa_context *c = o->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SINK))
			continue;
		sink_callback(c, g, d);
	}
	d->cb(c, NULL, 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_sink_info_list(pa_context *c, pa_sink_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct sink_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, sink_info_list, sizeof(struct sink_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

static int set_stream_volume(pa_context *c, pa_stream *s, const pa_cvolume *volume, bool mute)
{
	uint32_t i, n_channel_volumes;
	float channel_volumes[SPA_AUDIO_MAX_CHANNELS];
	float *vols;

	if (volume) {
		for (i = 0; i < volume->channels; i++)
			channel_volumes[i] = volume->values[i] / (float) PA_VOLUME_NORM;
		vols = channel_volumes;
		n_channel_volumes = volume->channels;
	} else {
		vols = s->channel_volumes;
		n_channel_volumes = s->n_channel_volumes;
	}

	if (n_channel_volumes != s->n_channel_volumes ||
	    !memcmp(s->channel_volumes, vols, n_channel_volumes * sizeof(float)) ||
	    s->mute != mute) {
		float val = s->mute ? 1.0f : 0.0f;
		pw_stream_set_control(s->stream,
				SPA_PROP_mute, 1, &val,
				SPA_PROP_channelVolumes, n_channel_volumes, vols,
				0);
	}
	return 0;
}

static int set_node_volume(pa_context *c, struct global *g, const pa_cvolume *volume, bool mute)
{
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	uint32_t i, n_channel_volumes;
	float channel_volumes[SPA_AUDIO_MAX_CHANNELS];
	float *vols;

	if (volume) {
		for (i = 0; i < volume->channels; i++)
			channel_volumes[i] = volume->values[i] / (float) PA_VOLUME_NORM;
		vols = channel_volumes;
		n_channel_volumes = volume->channels;

		if (n_channel_volumes == g->node_info.n_channel_volumes &&
		    memcmp(g->node_info.channel_volumes, vols, n_channel_volumes * sizeof(float)) == 0 &&
		    mute == g->node_info.mute)
			return 0;

		memcpy(g->node_info.channel_volumes, vols, n_channel_volumes * sizeof(float));
		g->node_info.n_channel_volumes = n_channel_volumes;
	} else {
		n_channel_volumes = g->node_info.n_channel_volumes;
		vols = g->node_info.channel_volumes;
		if (mute == g->node_info.mute)
			return 0;
	}
	g->node_info.mute = mute;

	if (!SPA_FLAG_IS_SET(g->permissions, PW_PERM_W | PW_PERM_X))
		return PA_ERR_ACCESS;

	pw_node_set_param((struct pw_node*)g->proxy,
		SPA_PARAM_Props, 0,
		spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Props,	SPA_PARAM_Props,
			SPA_PROP_mute,			SPA_POD_Bool(mute),
			SPA_PROP_channelVolumes,	SPA_POD_Array(sizeof(float),
								SPA_TYPE_Float,
								n_channel_volumes,
								vols)));
	return 0;
}

static int set_device_volume(pa_context *c, struct global *g, struct global *cg, uint32_t id,
		uint32_t device_id, const pa_cvolume *volume, bool mute)
{
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[2];
	struct spa_pod *param;
	uint32_t i, n_channel_volumes;
	float channel_volumes[SPA_AUDIO_MAX_CHANNELS];
	float *vols;

	if (volume) {
		for (i = 0; i < volume->channels; i++)
			channel_volumes[i] = volume->values[i] / (float) PA_VOLUME_NORM;
		vols = channel_volumes;
		n_channel_volumes = volume->channels;

		if (n_channel_volumes == g->node_info.n_channel_volumes &&
		    memcmp(g->node_info.channel_volumes, vols, n_channel_volumes * sizeof(float)) == 0 &&
		    mute == g->node_info.mute)
			return 0;

		memcpy(g->node_info.channel_volumes, vols, n_channel_volumes * sizeof(float));
		g->node_info.n_channel_volumes = n_channel_volumes;
	} else {
		n_channel_volumes = g->node_info.n_channel_volumes;
		vols = g->node_info.channel_volumes;
		if (mute == g->node_info.mute)
			return 0;
	}
	g->node_info.mute = mute;

	if (!SPA_FLAG_IS_SET(cg->permissions, PW_PERM_W | PW_PERM_X))
		return PA_ERR_ACCESS;

	spa_pod_builder_push_object(&b, &f[0],
			SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
	spa_pod_builder_add(&b,
			SPA_PARAM_ROUTE_index, SPA_POD_Int(id),
			SPA_PARAM_ROUTE_device, SPA_POD_Int(device_id),
			0);
	spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
	spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Props,	SPA_PARAM_Props,
			SPA_PROP_mute,			SPA_POD_Bool(mute),
			SPA_PROP_channelVolumes,	SPA_POD_Array(sizeof(float),
								SPA_TYPE_Float,
								n_channel_volumes,
								vols));
	param = spa_pod_builder_pop(&b, &f[0]);

	pw_device_set_param((struct pw_node*)cg->proxy,
		SPA_PARAM_Route, 0, param);

	return 0;
}

static int set_volume(pa_context *c, struct global *g, const pa_cvolume *volume, bool mute,
		uint32_t mask)
{
	struct global *cg;
	uint32_t id = SPA_ID_INVALID, card_id, device_id;
	int res;

	card_id = g->node_info.device_id;
	device_id = g->node_info.profile_device_id;

	pw_log_info("card:%u global:%u flags:%08x", card_id, g->id, g->node_info.flags);

	if (SPA_FLAG_IS_SET(g->node_info.flags, NODE_FLAG_DEVICE_VOLUME | NODE_FLAG_DEVICE_MUTE) &&
	    (cg = pa_context_find_global(c, card_id)) != NULL) {
		if (mask & PA_SUBSCRIPTION_MASK_SINK)
			id = cg->card_info.active_port_output;
		else if (mask & PA_SUBSCRIPTION_MASK_SOURCE)
			id = cg->card_info.active_port_input;
	}
	if (id != SPA_ID_INVALID && device_id != SPA_ID_INVALID) {
		res = set_device_volume(c, g, cg, id, device_id, volume, mute);
	} else {
		res = set_node_volume(c, g, volume, mute);
	}
	return res;
}

struct volume_data {
	pa_context_success_cb_t cb;
	uint32_t mask;
	void *userdata;
	char *name;
	uint32_t idx;
	bool have_volume;
	pa_cvolume volume;
	int mute;
};

static void do_node_volume_mute(pa_operation *o, void *userdata)
{
	struct volume_data *d = userdata;
	pa_context *c = o->context;
	struct global *g;
	int error = 0;

	if (d->name) {
		g = pa_context_find_global_by_name(c, d->mask, d->name);
		pa_xfree(d->name);
	} else {
		if ((g = pa_context_find_global(c, d->idx)) == NULL ||
		    !(g->mask & d->mask))
			g = NULL;
	}
	if (g) {
		error = set_volume(c, g,
				d->have_volume ? &d->volume : NULL,
				d->have_volume ? g->node_info.mute : d->mute,
				d->mask);
	} else {
		error = PA_ERR_INVALID;
	}
	if (error != 0)
		pa_context_set_error(c, error);
	if (d->cb)
		d->cb(c, error ? 0 : 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_set_sink_volume_by_index(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct volume_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	pw_log_debug("context %p: index %d", c, idx);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(c, pa_cvolume_valid(volume), PA_ERR_INVALID);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_node_volume_mute, sizeof(struct volume_data));
	d = o->userdata;
	d->mask = PA_SUBSCRIPTION_MASK_SINK;
	d->cb = cb;
	d->userdata = userdata;
	d->idx = idx;
	d->volume = *volume;
	d->have_volume = true;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_sink_volume_by_name(pa_context *c, const char *name, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct volume_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(c, pa_cvolume_valid(volume), PA_ERR_INVALID);

	pw_log_debug("context %p: name %s", c, name);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_node_volume_mute, sizeof(struct volume_data));
	d = o->userdata;
	d->cb = cb;
	d->mask = PA_SUBSCRIPTION_MASK_SINK;
	d->userdata = userdata;
	d->name = pa_xstrdup(name);
	d->volume = *volume;
	d->have_volume = true;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_sink_mute_by_index(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct volume_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pw_log_debug("context %p: index %d", c, idx);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_node_volume_mute, sizeof(struct volume_data));
	d = o->userdata;
	d->mask = PA_SUBSCRIPTION_MASK_SINK;
	d->cb = cb;
	d->userdata = userdata;
	d->idx = idx;
	d->mute = mute;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_sink_mute_by_name(pa_context *c, const char *name, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct volume_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

	pw_log_debug("context %p: name %s", c, name);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_node_volume_mute, sizeof(struct volume_data));
	d = o->userdata;
	d->mask = PA_SUBSCRIPTION_MASK_SINK;
	d->cb = cb;
	d->userdata = userdata;
	d->name = pa_xstrdup(name);
	d->mute = mute;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_suspend_sink_by_name(pa_context *c, const char *sink_name, int suspend, pa_context_success_cb_t cb, void* userdata)
{
	pa_operation *o;
	struct success_ack *d;

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->userdata = userdata;
	pa_operation_sync(o);

	pw_log_warn("Not Implemented");
	return o;
}

SPA_EXPORT
pa_operation* pa_context_suspend_sink_by_index(pa_context *c, uint32_t idx, int suspend,  pa_context_success_cb_t cb, void* userdata)
{
	pa_operation *o;
	struct success_ack *d;

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->userdata = userdata;
	pa_operation_sync(o);

	pw_log_warn("Not Implemented");
	return o;
}

static int set_device_route(pa_context *c, struct global *g, const char *port, enum spa_direction direction)
{
	struct global *cg;
	struct param *p;
	uint32_t id = SPA_ID_INVALID, card_id, device_id;
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));

	card_id = g->node_info.device_id;
	device_id = g->node_info.profile_device_id;

	pw_log_info("port \"%s\": card:%u device:%u global:%u", port, card_id, device_id, g->id);

	if ((cg = pa_context_find_global(c, card_id)) == NULL || device_id == SPA_ID_INVALID)
		return PA_ERR_NOENTITY;

	spa_list_for_each(p, &cg->card_info.ports, link) {
		uint32_t test_id;
		const char *name;
		enum spa_direction test_direction;

		if (spa_pod_parse_object(p->param,
				SPA_TYPE_OBJECT_ParamRoute, NULL,
				SPA_PARAM_ROUTE_index, SPA_POD_Int(&test_id),
				SPA_PARAM_ROUTE_direction, SPA_POD_Id(&test_direction),
				SPA_PARAM_ROUTE_name,  SPA_POD_String(&name)) < 0) {
			pw_log_warn("device %d: can't parse route", g->id);
			continue;
		}
		pw_log_info("port id:%u name:\"%s\" dir:%d", test_id, name, test_direction);
		if (test_direction != direction)
			continue;
		if (strcmp(name, port) == 0) {
			id = test_id;
			break;
		}
	}
	pw_log_info("port %s, id %u", port, id);
	if (id == SPA_ID_INVALID)
		return PA_ERR_NOENTITY;

	if (!SPA_FLAG_IS_SET(cg->permissions, PW_PERM_W | PW_PERM_X))
		return PA_ERR_ACCESS;

	pw_device_set_param((struct pw_device*)cg->proxy,
		SPA_PARAM_Route, 0,
		spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamRoute,	SPA_PARAM_Route,
			SPA_PARAM_ROUTE_index, SPA_POD_Int(id),
			SPA_PARAM_ROUTE_direction, SPA_POD_Id(direction),
			SPA_PARAM_ROUTE_device, SPA_POD_Int(device_id)));

	return 0;
}

struct device_route {
	uint32_t mask;
	pa_context_success_cb_t cb;
	void *userdata;
	char *name;
	uint32_t idx;
	char *port;
	enum spa_direction direction;
};

static void do_device_route(pa_operation *o, void *userdata)
{
	struct device_route *d = userdata;
	pa_context *c = o->context;
	struct global *g;
	int error;

	pw_log_debug("%p", c);

	if (d->name) {
		g = pa_context_find_global_by_name(c, d->mask, d->name);
		pa_xfree(d->name);
	} else {
		if ((g = pa_context_find_global(c, d->idx)) == NULL ||
		    !(g->mask & d->mask))
			g = NULL;
	}
	if (g) {
		error = set_device_route(c, g, d->port, d->direction);
	} else {
		error = PA_ERR_INVALID;
	}
	if (error != 0)
		pa_context_set_error(c, error);
	if (d->cb)
		d->cb(c, error != 0 ? 0 : 1, d->userdata);
	pa_operation_done(o);
	pa_xfree(d->port);
}

SPA_EXPORT
pa_operation* pa_context_set_sink_port_by_index(pa_context *c, uint32_t idx, const char*port, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct device_route *d;

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_device_route, sizeof(struct device_route));
	d = o->userdata;
	d->mask = PA_SUBSCRIPTION_MASK_SINK;
	d->cb = cb;
	d->userdata = userdata;
	d->idx = idx;
	d->port = pa_xstrdup(port);
	d->direction = SPA_DIRECTION_OUTPUT;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_sink_port_by_name(pa_context *c, const char*name, const char*port, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct device_route *d;

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_device_route, sizeof(struct device_route));
	d = o->userdata;
	d->mask = PA_SUBSCRIPTION_MASK_SINK;
	d->cb = cb;
	d->userdata = userdata;
	d->name = pa_xstrdup(name);
	d->port = pa_xstrdup(port);
	d->direction = SPA_DIRECTION_OUTPUT;
	pa_operation_sync(o);
	return o;
}


struct source_data {
	pa_source_info_cb_t cb;
	void *userdata;
	char *name;
	uint32_t idx;
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
static int source_callback(pa_context *c, struct global *g, struct source_data *d)
{
	struct global *cg;
	struct pw_node_info *info = g->info;
	const char *str;
	uint32_t n, j;
	pa_source_info i;
	pa_format_info ii[1];
	pa_format_info *ip[1];
	enum pa_source_flags flags;
	bool monitor;

	flags = PA_SOURCE_LATENCY | PA_SOURCE_DYNAMIC_LATENCY |
		  PA_SOURCE_DECIBEL_VOLUME;

	monitor = (g->mask & PA_SUBSCRIPTION_MASK_SINK) != 0;

	spa_zero(i);

	i.proplist = pa_proplist_new_dict(info->props);

	if (monitor) {
		if ((str = spa_dict_lookup(info->props, PW_KEY_NODE_NAME)))
			pa_proplist_setf(i.proplist, PW_KEY_NODE_NAME, "%s.monitor", str);
		if ((str = spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION)))
			pa_proplist_setf(i.proplist, PW_KEY_NODE_DESCRIPTION, "Monitor or %s", str);
	}

	if ((str = pa_proplist_gets(i.proplist, PW_KEY_NODE_NAME)))
		i.name = str;
	else
		i.name = "unknown";

	i.index = g->id;
	if ((str = pa_proplist_gets(i.proplist, PW_KEY_NODE_DESCRIPTION)))
		i.description = str;
	else
		i.description = "unknown";
	i.sample_spec = g->node_info.sample_spec;
	if (g->node_info.n_channel_volumes)
		i.sample_spec.channels = g->node_info.n_channel_volumes;
	else
		i.sample_spec.channels = 2;
	if (i.sample_spec.channels == g->node_info.channel_map.channels)
		i.channel_map = g->node_info.channel_map;
	else
		pa_channel_map_init_auto(&i.channel_map, i.sample_spec.channels, PA_CHANNEL_MAP_OSS);
	i.owner_module = 0;
	i.volume.channels = i.sample_spec.channels;
	for (n = 0; n < i.volume.channels; n++)
		i.volume.values[n] = g->node_info.volume * g->node_info.channel_volumes[n] * PA_VOLUME_NORM;
	i.mute = g->node_info.mute;
	if (monitor) {
		i.monitor_of_sink = g->id;
		i.monitor_of_sink_name = pa_context_find_global_name(c, g->id);
		i.index = g->node_info.monitor;
	} else {
		i.monitor_of_sink = PA_INVALID_INDEX;
		i.monitor_of_sink_name = NULL;
		flags |= PA_SOURCE_HARDWARE;
		if (SPA_FLAG_IS_SET(g->node_info.flags, NODE_FLAG_HW_VOLUME))
			flags |= PA_SINK_HW_VOLUME_CTRL;
		if (SPA_FLAG_IS_SET(g->node_info.flags, NODE_FLAG_HW_MUTE))
			flags |= PA_SINK_HW_MUTE_CTRL;
	}
	i.latency = 0;
	i.driver = "PipeWire";
	i.flags = flags;
	i.configured_latency = 0;
	i.base_volume = g->node_info.base_volume * PA_VOLUME_NORM;
	i.n_volume_steps = g->node_info.volume_step * (PA_VOLUME_NORM+1);
	i.state = node_state_to_source(info->state);
	i.card = g->node_info.device_id;
	i.n_ports = 0;
	i.ports = NULL;
	i.active_port = NULL;
	if (!monitor && (cg = pa_context_find_global(c, i.card)) != NULL) {
		pa_source_port_info *spi;
		pa_card_info *ci = &cg->card_info.info;

		spi = alloca(ci->n_ports * sizeof(pa_source_port_info));
		i.ports = alloca((ci->n_ports + 1) * sizeof(pa_source_port_info *));

		for (n = 0,j = 0; n < ci->n_ports; n++) {
			if (ci->ports[n]->direction != PA_DIRECTION_INPUT)
				continue;
			if (!has_profile(ci->ports[n]->profiles2, ci->active_profile2))
				continue;
			i.ports[j] = &spi[j];
			spi[j].name = ci->ports[n]->name;
			spi[j].description = ci->ports[n]->description;
			spi[j].priority = ci->ports[n]->priority;
			spi[j].available = ci->ports[n]->available;
			if (n == cg->card_info.active_port_input)
				i.active_port = i.ports[j];
			j++;
		}
		i.n_ports = j;
		if (i.n_ports == 0)
			i.ports = NULL;
		else
			i.ports[j] = NULL;
	}
	i.n_formats = 1;
	ii[0].encoding = PA_ENCODING_PCM;
	ii[0].plist = pa_proplist_new();
	ip[0] = ii;
	i.formats = ip;
	d->cb(c, &i, 0, d->userdata);
	pa_proplist_free(i.proplist);
	pa_proplist_free(ii[0].plist);
	return 0;
}

static void source_info(pa_operation *o, void *userdata)
{
	struct source_data *d = userdata;
	pa_context *c = o->context;
	struct global *g;
	int error;

	if (d->name) {
		g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_SOURCE, d->name);
		pa_xfree(d->name);
	} else {
		if (((g = pa_context_find_global(c, d->idx)) == NULL ||
		    !(g->mask & PA_SUBSCRIPTION_MASK_SOURCE)) &&
		    (((g = pa_context_find_global(c, d->idx & PA_IDX_MASK_DSP)) == NULL ||
		    !(g->mask & PA_SUBSCRIPTION_MASK_SOURCE))))
			g = NULL;
	}

	if (g) {
		error = source_callback(c, g, d);
	} else {
		error = PA_ERR_INVALID;
	}
	if (error)
		pa_context_set_error(c, error);
	d->cb(c, NULL, error ? -1 : 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_source_info_by_name(pa_context *c, const char *name, pa_source_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct source_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, source_info, sizeof(struct source_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	d->name = pa_xstrdup(name);
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_get_source_info_by_index(pa_context *c, uint32_t idx, pa_source_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct source_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pw_log_debug("context %p: index %d", c, idx);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, source_info, sizeof(struct source_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	d->idx = idx;
	pa_operation_sync(o);

	return o;
}

static void source_info_list(pa_operation *o, void *userdata)
{
	struct source_data *d = userdata;
	pa_context *c = o->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SOURCE))
			continue;
		source_callback(c, g, d);
	}
	d->cb(c, NULL, 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_source_info_list(pa_context *c, pa_source_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct source_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, source_info_list, sizeof(struct source_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_source_volume_by_index(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct volume_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	pw_log_debug("context %p: index %d", c, idx);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(c, pa_cvolume_valid(volume), PA_ERR_INVALID);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_node_volume_mute, sizeof(struct volume_data));
	d = o->userdata;
	d->mask = PA_SUBSCRIPTION_MASK_SOURCE;
	d->cb = cb;
	d->userdata = userdata;
	d->idx = idx;
	d->volume = *volume;
	d->have_volume = true;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_source_volume_by_name(pa_context *c, const char *name, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct volume_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(c, pa_cvolume_valid(volume), PA_ERR_INVALID);

	pw_log_debug("context %p: name %s", c, name);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_node_volume_mute, sizeof(struct volume_data));
	d = o->userdata;
	d->mask = PA_SUBSCRIPTION_MASK_SOURCE;
	d->cb = cb;
	d->userdata = userdata;
	d->name = pa_xstrdup(name);
	d->volume = *volume;
	d->have_volume = true;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_source_mute_by_index(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct volume_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pw_log_debug("context %p: index %d", c, idx);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_node_volume_mute, sizeof(struct volume_data));
	d = o->userdata;
	d->mask = PA_SUBSCRIPTION_MASK_SOURCE;
	d->cb = cb;
	d->userdata = userdata;
	d->idx = idx;
	d->mute = mute;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_source_mute_by_name(pa_context *c, const char *name, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct volume_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

	pw_log_debug("context %p: name %s", c, name);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_node_volume_mute, sizeof(struct volume_data));
	d = o->userdata;
	d->mask = PA_SUBSCRIPTION_MASK_SOURCE;
	d->cb = cb;
	d->userdata = userdata;
	d->name = pa_xstrdup(name);
	d->mute = mute;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_suspend_source_by_name(pa_context *c, const char *source_name, int suspend, pa_context_success_cb_t cb, void* userdata)
{
	pa_operation *o;
	struct success_ack *d;

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->userdata = userdata;
	pa_operation_sync(o);

	pw_log_warn("Not Implemented");
	return o;
}

SPA_EXPORT
pa_operation* pa_context_suspend_source_by_index(pa_context *c, uint32_t idx, int suspend, pa_context_success_cb_t cb, void* userdata)
{
	pa_operation *o;
	struct success_ack *d;

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->userdata = userdata;
	pa_operation_sync(o);

	pw_log_warn("Not Implemented");
	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_source_port_by_index(pa_context *c, uint32_t idx, const char*port, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct device_route *d;

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pw_log_debug("context %p: idx %d", c, idx);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_device_route, sizeof(struct device_route));
	d = o->userdata;
	d->mask = PA_SUBSCRIPTION_MASK_SOURCE;
	d->cb = cb;
	d->userdata = userdata;
	d->idx = idx;
	d->port = pa_xstrdup(port);
	d->direction = SPA_DIRECTION_INPUT;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_source_port_by_name(pa_context *c, const char*name, const char*port, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct device_route *d;

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

	pw_log_debug("context %p: name %s", c, name);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_device_route, sizeof(struct device_route));
	d = o->userdata;
	d->mask = PA_SUBSCRIPTION_MASK_SOURCE;
	d->cb = cb;
	d->userdata = userdata;
	d->name = pa_xstrdup(name);
	d->port = pa_xstrdup(port);
	d->direction = SPA_DIRECTION_INPUT;
	pa_operation_sync(o);
	return o;
}

struct server_data {
	pa_server_info_cb_t cb;
	void *userdata;
	struct global *global;
};

static const char *get_default_name(pa_context *c, uint32_t mask)
{
	struct global *g;
	const char *str, *id = NULL, *type, *key;

	if (c->metadata) {
		if (mask & PA_SUBSCRIPTION_MASK_SINK)
			key = METADATA_DEFAULT_SINK;
		else if (mask & PA_SUBSCRIPTION_MASK_SOURCE)
			key = METADATA_DEFAULT_SOURCE;
		else
			return NULL;

		if (pa_metadata_get(c->metadata, PW_ID_CORE, key, &type, &id) <= 0)
			id = NULL;
	}
	spa_list_for_each(g, &c->globals, link) {
		if ((g->mask & mask) != mask)
			continue;
		if (g->props != NULL &&
		    (str = pw_properties_get(g->props, PW_KEY_NODE_NAME)) != NULL &&
		    (id == NULL || (uint32_t)atoi(id) == g->id))
			return str;
	}
	return "unknown";
}

static void server_callback(struct server_data *d, pa_context *c)
{
	const struct pw_core_info *info = c->core_info;
	const char *str;
	pa_server_info i;

	spa_zero(i);
	i.user_name = info->user_name;
	i.host_name = info->host_name;
	i.server_version = info->version;
	i.server_name = info->name;
	i.sample_spec.format = PA_SAMPLE_FLOAT32NE;
	if (info->props && (str = spa_dict_lookup(info->props, "default.clock.rate")) != NULL)
		i.sample_spec.rate = atoi(str);
	else
		i.sample_spec.rate = 44100;
	i.sample_spec.channels = 2;
	i.default_sink_name = get_default_name(c, PA_SUBSCRIPTION_MASK_SINK);
	i.default_source_name = get_default_name(c, PA_SUBSCRIPTION_MASK_SOURCE);
	i.cookie = info->cookie;
        pa_channel_map_init_extend(&i.channel_map, i.sample_spec.channels, PA_CHANNEL_MAP_OSS);
	d->cb(c, &i, d->userdata);
}

static void server_info(pa_operation *o, void *userdata)
{
	struct server_data *d = userdata;
	server_callback(d, o->context);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_server_info(pa_context *c, pa_server_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct server_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, server_info, sizeof(struct server_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

struct module_data {
	uint32_t idx;
	pa_module_info_cb_t cb;
	void *userdata;
};

static int module_callback(pa_context *c, struct module_data *d, struct global *g)
{
	d->cb(c, &g->module_info.info, 0, d->userdata);
	return 0;
}

static void module_info(pa_operation *o, void *userdata)
{
	struct module_data *d = userdata;
	pa_context *c = o->context;
	struct global *g;
	int error;

	if ((g = pa_context_find_global(c, d->idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_MODULE))
		g = NULL;

	if (g) {
		error = module_callback(c, d, g);
	} else {
		error = PA_ERR_INVALID;
	}
	if (error)
		pa_context_set_error(c, error);
	d->cb(c, NULL, error ? -1 : 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_module_info(pa_context *c, uint32_t idx, pa_module_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct module_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, module_info, sizeof(struct module_data));
	d = o->userdata;
	d->idx = idx;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

static void module_info_list(pa_operation *o, void *userdata)
{
	struct module_data *d = userdata;
	pa_context *c = o->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_MODULE))
			continue;
		module_callback(c, d, g);
	}
	d->cb(c, NULL, 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_module_info_list(pa_context *c, pa_module_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct module_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, module_info_list, sizeof(struct module_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_load_module(pa_context *c, const char*name, const char *argument, pa_context_index_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation* pa_context_unload_module(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->userdata = userdata;
	pa_operation_sync(o);

	pw_log_warn("Not Implemented");
	return o;
}

struct client_data {
	uint32_t idx;
	pa_client_info_cb_t cb;
	void *userdata;
};

static int client_callback(pa_context *c, struct client_data *d, struct global *g)
{
	d->cb(c, &g->client_info.info, 0, d->userdata);
	return 0;
}

static void client_info(pa_operation *o, void *userdata)
{
	struct client_data *d = userdata;
	pa_context *c = o->context;
	struct global *g;
	int error;

	if ((g = pa_context_find_global(c, d->idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_CLIENT))
		g = NULL;

	if (g) {
		error = client_callback(c, d, g);
	} else {
		error = PA_ERR_INVALID;
	}
	if (error)
		pa_context_set_error(c, error);
	d->cb(c, NULL, error ? -1 : 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_client_info(pa_context *c, uint32_t idx, pa_client_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct client_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, client_info, sizeof(struct client_data));
	d = o->userdata;
	d->idx = idx;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

static void client_info_list(pa_operation *o, void *userdata)
{
	struct client_data *d = userdata;
	pa_context *c = o->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_CLIENT))
			continue;
		client_callback(c, d, g);
	}
	d->cb(c, NULL, 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_client_info_list(pa_context *c, pa_client_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct client_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, client_info_list, sizeof(struct client_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

struct kill_client {
	uint32_t idx;
	pa_context_success_cb_t cb;
	void *userdata;
};

static void do_kill_client(pa_operation *o, void *userdata)
{
	struct kill_client *d = userdata;
	pa_context *c = o->context;
	struct global *g;
	int error = 0;

	if ((g = pa_context_find_global(c, d->idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_CLIENT))
		g = NULL;

	if (g) {
		pw_registry_destroy(c->registry, g->id);
	} else {
		error = PA_ERR_INVALID;
	}
	if (error != 0)
		pa_context_set_error(c, error);
	if (d->cb)
		d->cb(c, error ? 0 : 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_kill_client(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct kill_client *d;

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_kill_client, sizeof(struct kill_client));
	d = o->userdata;
	d->idx = idx;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

struct card_data {
	pa_card_info_cb_t cb;
	pa_context_success_cb_t success_cb;
	char *name;
	uint32_t idx;
	void *userdata;
	char *profile;
};

static int card_callback(pa_context *c, struct card_data *d, struct global *g)
{
	pa_card_info *i = &g->card_info.info;
	d->cb(c, i, 0, d->userdata);
	return 0;
}

static void card_info(pa_operation *o, void *userdata)
{
	struct card_data *d = userdata;
	pa_context *c = o->context;
	struct global *g;
	int error;

	if (d->name) {
		g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_CARD, d->name);
		pa_xfree(d->name);
	} else if ((g = pa_context_find_global(c, d->idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_CARD))
		g = NULL;

	if (g) {
		error = card_callback(c, d, g);
	} else {
		error = PA_ERR_INVALID;
	}
	if (error != 0)
		pa_context_set_error(c, error);
	if (d->cb)
		d->cb(c, NULL, error ? -1 : 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_card_info_by_index(pa_context *c, uint32_t idx, pa_card_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct card_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pw_log_debug("context %p: %u", c, idx);
	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, card_info, sizeof(struct card_data));
	d = o->userdata;
	d->idx = idx;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
pa_operation* pa_context_get_card_info_by_name(pa_context *c, const char *name, pa_card_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct card_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

	pw_log_debug("context %p: %s", c, name);
	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, card_info, sizeof(struct card_data));
	d = o->userdata;
	d->name = pa_xstrdup(name);
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

static void card_info_list(pa_operation *o, void *userdata)
{
	struct card_data *d = userdata;
	pa_context *c = o->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_CARD))
			continue;
		card_callback(c, d, g);
	}
	d->cb(c, NULL, 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_card_info_list(pa_context *c, pa_card_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct card_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	pw_log_debug("context %p", c);
	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, card_info_list, sizeof(struct card_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

static void card_profile(pa_operation *o, void *userdata)
{
	struct card_data *d = userdata;
	struct global *g;
	pa_context *c = o->context;
	int error = 0;
	uint32_t id = SPA_ID_INVALID;
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct param *p;

	if (d->name) {
		g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_CARD, d->name);
		pa_xfree(d->name);
	} else if ((g = pa_context_find_global(c, d->idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_CARD))
		g = NULL;

	if (g == NULL) {
		error = PA_ERR_INVALID;
		goto done;
	}

	spa_list_for_each(p, &g->card_info.profiles, link) {
		uint32_t test_id;
		const char *name;

		if (spa_pod_parse_object(p->param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&test_id),
				SPA_PARAM_PROFILE_name,  SPA_POD_String(&name)) < 0) {
			pw_log_warn("device %d: can't parse profile", g->id);
			continue;
		}
		if (strcmp(name, d->profile) == 0) {
			id = test_id;
			break;
		}
	}
	if (id == SPA_ID_INVALID) {
		error = PA_ERR_INVALID;
		goto done;
	}

	if (!SPA_FLAG_IS_SET(g->permissions, PW_PERM_W | PW_PERM_X)) {
		error = PA_ERR_ACCESS;
		goto done;
	}

	pw_device_set_param((struct pw_device*)g->proxy,
			SPA_PARAM_Profile, 0,
			spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(id)));
done:
	if (error)
		pa_context_set_error(c, error);
	if (d->success_cb)
		d->success_cb(c, error ? 0 : 1, d->userdata);
	pa_operation_done(o);
	free(d->profile);
}

SPA_EXPORT
pa_operation* pa_context_set_card_profile_by_index(pa_context *c, uint32_t idx, const char*profile, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct card_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pw_log_debug("Card set profile %s", profile);
	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, card_profile, sizeof(struct card_data));
	d = o->userdata;
	d->idx = idx;
	d->success_cb = cb;
	d->userdata = userdata;
	d->profile = strdup(profile);
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_card_profile_by_name(pa_context *c, const char*name, const char*profile, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct card_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

	pw_log_debug("Card set profile %s", profile);
	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, card_profile, sizeof(struct card_data));
	d = o->userdata;
	d->name = pa_xstrdup(name);
	d->success_cb = cb;
	d->userdata = userdata;
	d->profile = strdup(profile);
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_port_latency_offset(pa_context *c, const char *card_name, const char *port_name, int64_t offset, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->userdata = userdata;
	pa_operation_sync(o);

	pw_log_warn("Not Implemented");
	return o;
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
	pa_sink_input_info_cb_t cb;
	uint32_t idx;
	void *userdata;
};

static int sink_input_callback(pa_context *c, struct sink_input_data *d, struct global *g)
{
	struct global *cl;
	struct pw_node_info *info = g->info;
	const char *name = NULL;
	uint32_t n;
	pa_sink_input_info i;
	pa_format_info ii[1];
	pa_stream *s;

	if (info == NULL)
		return PA_ERR_INVALID;

	s = find_stream(c, g->id);

	if (info->props) {
		if ((name = spa_dict_lookup(info->props, PW_KEY_MEDIA_NAME)) == NULL &&
		    (name = spa_dict_lookup(info->props, PW_KEY_APP_NAME)) == NULL &&
		    (name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME)) == NULL)
			name = NULL;
	}
	if (name == NULL)
		name = "unknown";

	cl = pa_context_find_global(c, g->node_info.client_id);

	spa_zero(i);
	i.index = g->id;
	i.name = name;
	i.owner_module = PA_INVALID_INDEX;
	i.client = g->node_info.client_id;
	if (s) {
		i.sink = s->device_index;
	}
	else {
		struct global *l;
		l = pa_context_find_linked(c, g->id);
		i.sink = l ? l->id : PA_INVALID_INDEX;
	}
	if (s && s->sample_spec.channels > 0) {
		i.sample_spec = s->sample_spec;
		if (s->channel_map.channels == s->sample_spec.channels)
			i.channel_map = s->channel_map;
		else
			pa_channel_map_init_auto(&i.channel_map,
					i.sample_spec.channels, PA_CHANNEL_MAP_OSS);
		i.format = s->format;
	}
	else {
		i.sample_spec = g->node_info.sample_spec;
		if (g->node_info.n_channel_volumes)
			i.sample_spec.channels = g->node_info.n_channel_volumes;
		else
			i.sample_spec.channels = 2;
		if (i.sample_spec.channels == g->node_info.channel_map.channels)
			i.channel_map = g->node_info.channel_map;
		else
			pa_channel_map_init_auto(&i.channel_map, i.sample_spec.channels, PA_CHANNEL_MAP_OSS);
		ii[0].encoding = PA_ENCODING_PCM;
		ii[0].plist = pa_proplist_new();
		i.format = ii;
	}
	pa_cvolume_init(&i.volume);
	i.volume.channels = i.sample_spec.channels;
	for (n = 0; n < i.volume.channels; n++)
		i.volume.values[n] = g->node_info.volume * g->node_info.channel_volumes[n] * PA_VOLUME_NORM;

	i.mute = g->node_info.mute;
	i.buffer_usec = 0;
	i.sink_usec = 0;
	i.resample_method = "PipeWire resampler";
	i.driver = "PipeWire";
	i.proplist = pa_proplist_new_dict(info->props);
	if (cl && cl->client_info.info.proplist)
		pa_proplist_update(i.proplist, PA_UPDATE_MERGE, cl->client_info.info.proplist);
	i.corked = false;
	i.has_volume = true;
	i.volume_writable = true;

	pw_log_debug("context %p: sink info for %d sink:%d", c, i.index, i.sink);

	d->cb(c, &i, 0, d->userdata);

	pa_proplist_free(i.proplist);
	return 0;
}

static void sink_input_info(pa_operation *o, void *userdata)
{
	struct sink_input_data *d = userdata;
	pa_context *c = o->context;
	struct global *g;
	int error;

	if ((g = pa_context_find_global(c, d->idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_SINK_INPUT))
		g = NULL;

	if (g) {
		error = sink_input_callback(c, d, g);
	} else {
		error = PA_ERR_INVALID;
	}
	if (error)
		pa_context_set_error(c, PA_ERR_INVALID);
	d->cb(c, NULL, error ? -1 : 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_sink_input_info(pa_context *c, uint32_t idx, pa_sink_input_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct sink_input_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pw_log_debug("context %p: info for %d", c, idx);
	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, sink_input_info, sizeof(struct sink_input_data));
	d = o->userdata;
	d->idx = idx;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

static void sink_input_info_list(pa_operation *o, void *userdata)
{
	struct sink_input_data *d = userdata;
	pa_context *c = o->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SINK_INPUT))
			continue;
		sink_input_callback(c, d, g);
	}
	d->cb(c, NULL, 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_sink_input_info_list(pa_context *c, pa_sink_input_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct sink_input_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	pw_log_debug("context %p", c);
	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, sink_input_info_list, sizeof(struct sink_input_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

struct target_node {
	uint32_t idx;
	uint32_t mask;
	uint32_t target_idx;
	uint32_t target_mask;
	char *target_name;
	pa_context_success_cb_t cb;
	void *userdata;
	const char *key;
};

static void do_target_node(pa_operation *o, void *userdata)
{
	struct target_node *d = userdata;
	pa_context *c = o->context;
	struct global *g;
	int error = 0;

	pw_log_debug("%p", c);

	if ((g = pa_context_find_global(c, d->idx)) == NULL ||
	    !(g->mask & d->mask)) {
		error = PA_ERR_NOENTITY;
		goto done;
	}

	if (d->target_name) {
		g = pa_context_find_global_by_name(c, d->target_mask, d->target_name);
	} else {
		if ((g = pa_context_find_global(c, d->target_idx)) == NULL ||
		    !(g->mask & d->target_mask))
			g = NULL;
	}
	if (g == NULL) {
		error = PA_ERR_NOENTITY;
	} else if (c->metadata) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%d", g->id);
		pw_metadata_set_property(c->metadata->proxy,
				d->idx, d->key, SPA_TYPE_INFO_BASE "Id", buf);
	} else {
		error = PA_ERR_NOTIMPLEMENTED;
	}
done:
	if (error != 0)
		pa_context_set_error(c, error);
	if (d->cb)
		d->cb(c, error != 0 ? 0 : 1, d->userdata);
	pa_operation_done(o);
	pa_xfree(d->target_name);
}

SPA_EXPORT
pa_operation* pa_context_move_sink_input_by_name(pa_context *c, uint32_t idx, const char *sink_name, pa_context_success_cb_t cb, void* userdata)
{
	pa_operation *o;
	struct target_node *d;

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_target_node, sizeof(struct target_node));
	d = o->userdata;
	d->idx = idx;
	d->mask = PA_SUBSCRIPTION_MASK_SINK_INPUT;
	d->target_name = pa_xstrdup(sink_name);
	d->target_mask = PA_SUBSCRIPTION_MASK_SINK;
	d->key = METADATA_TARGET_NODE;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_move_sink_input_by_index(pa_context *c, uint32_t idx, uint32_t sink_idx, pa_context_success_cb_t cb, void* userdata)
{
	pa_operation *o;
	struct target_node *d;

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_target_node, sizeof(struct target_node));
	d = o->userdata;
	d->idx = idx;
	d->mask = PA_SUBSCRIPTION_MASK_SINK_INPUT;
	d->target_idx = sink_idx;
	d->target_mask = PA_SUBSCRIPTION_MASK_SINK;
	d->key = METADATA_TARGET_NODE;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

struct stream_volume {
	uint32_t idx;
	uint32_t mask;
	bool have_volume;
	pa_cvolume volume;
	int mute;
	pa_context_success_cb_t cb;
	void *userdata;
};

static void do_stream_volume_mute(pa_operation *o, void *userdata)
{
	struct stream_volume *d = userdata;
	pa_context *c = o->context;
	struct global *g;
	int error = 0;
	pa_stream *s;

	if ((s = find_stream(c, d->idx)) == NULL) {
		if ((g = pa_context_find_global(c, d->idx)) == NULL ||
		    !(g->mask & d->mask))
			g = NULL;
	}
	if (s) {
		error = set_stream_volume(c, s,
				d->have_volume ? &d->volume : NULL,
				d->have_volume ? s->mute : d->mute);
	} else if (g) {
		error = set_node_volume(c, g,
				d->have_volume ? &d->volume : NULL,
				d->have_volume ? g->node_info.mute : d->mute);
	} else {
		error = PA_ERR_INVALID;
	}

	if (error != 0)
		pa_context_set_error(c, error);
	if (d->cb)
		d->cb(c, error != 0 ? 0 : 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_set_sink_input_volume(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct stream_volume *d;

	pw_log_debug("contex %p: index %d", c, idx);
	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_stream_volume_mute, sizeof(struct stream_volume));
	d = o->userdata;
	d->idx = idx;
	d->mask = PA_SUBSCRIPTION_MASK_SINK_INPUT;
	d->volume = *volume;
	d->have_volume = true;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_sink_input_mute(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct stream_volume *d;

	pw_log_debug("contex %p: index %d", c, idx);
	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_stream_volume_mute, sizeof(struct stream_volume));
	d = o->userdata;
	d->idx = idx;
	d->mask = PA_SUBSCRIPTION_MASK_SINK_INPUT;
	d->mute = mute;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

struct kill_stream {
	uint32_t idx;
	uint32_t mask;
	pa_context_success_cb_t cb;
	void *userdata;
};

static void do_kill_stream(pa_operation *o, void *userdata)
{
	struct kill_stream *d = userdata;
	pa_context *c = o->context;
	struct global *g;
	int error = 0;
	pa_stream *s;

	if ((s = find_stream(c, d->idx)) == NULL) {
		if ((g = pa_context_find_global(c, d->idx)) == NULL ||
		    !(g->mask & d->mask))
			g = NULL;
	}
	if (s) {
		pw_stream_destroy(s->stream);
	} else if (g) {
		pw_registry_destroy(c->registry, g->id);
	} else {
		error = PA_ERR_INVALID;
	}
	if (error != 0)
		pa_context_set_error(c, error);
	if (d->cb)
		d->cb(c, error != 0 ? 0 : 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_kill_sink_input(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct kill_stream *d;

	pw_log_debug("contex %p: index %d", c, idx);
	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_kill_stream, sizeof(struct kill_stream));
	d = o->userdata;
	d->idx = idx;
	d->mask = PA_SUBSCRIPTION_MASK_SINK_INPUT;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

struct source_output_data {
	uint32_t idx;
	pa_source_output_info_cb_t cb;
	void *userdata;
};

static int source_output_callback(struct source_output_data *d, pa_context *c, struct global *g)
{
	struct global *l, *cl;
	struct pw_node_info *info = g->info;
	const char *name = NULL;
	uint32_t n;
	pa_source_output_info i;
	pa_format_info ii[1];
	pa_stream *s;

	pw_log_debug("index %d", g->id);
	if (info == NULL)
		return PA_ERR_INVALID;

	s = find_stream(c, g->id);

	if (info->props) {
		if ((name = spa_dict_lookup(info->props, PW_KEY_MEDIA_NAME)) == NULL &&
		    (name = spa_dict_lookup(info->props, PW_KEY_APP_NAME)) == NULL &&
		    (name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME)) == NULL)
			name = NULL;
	}
	if (name == NULL)
		name = "unknown";

	cl = pa_context_find_global(c, g->node_info.client_id);

	spa_zero(i);
	i.index = g->id;
	i.name = name;
	i.owner_module = PA_INVALID_INDEX;
	i.client = g->node_info.client_id;
	if (s) {
		i.source = s->device_index;
	}
	else {
		l = pa_context_find_linked(c, g->id);
		i.source = l ? l->id : PA_INVALID_INDEX;
	}
	if (s && s->sample_spec.channels > 0) {
		i.sample_spec = s->sample_spec;
		if (s->channel_map.channels == s->sample_spec.channels)
			i.channel_map = s->channel_map;
		else
			pa_channel_map_init_auto(&i.channel_map,
					i.sample_spec.channels, PA_CHANNEL_MAP_OSS);
		i.format = s->format;
	}
	else {
		i.sample_spec = g->node_info.sample_spec;
		if (g->node_info.n_channel_volumes)
			i.sample_spec.channels = g->node_info.n_channel_volumes;
		else
			i.sample_spec.channels = 2;
		if (i.sample_spec.channels == g->node_info.channel_map.channels)
			i.channel_map = g->node_info.channel_map;
		else
			pa_channel_map_init_auto(&i.channel_map, i.sample_spec.channels, PA_CHANNEL_MAP_OSS);
		ii[0].encoding = PA_ENCODING_PCM;
		ii[0].plist = pa_proplist_new();
		i.format = ii;
	}
	pa_cvolume_init(&i.volume);
	i.volume.channels = i.sample_spec.channels;
	for (n = 0; n < i.volume.channels; n++)
		i.volume.values[n] = g->node_info.volume * g->node_info.channel_volumes[n] * PA_VOLUME_NORM;

	i.mute = g->node_info.mute;
	i.buffer_usec = 0;
	i.source_usec = 0;
	i.resample_method = "PipeWire resampler";
	i.driver = "PipeWire";
	i.proplist = pa_proplist_new_dict(info->props);
	if (cl && cl->client_info.info.proplist)
		pa_proplist_update(i.proplist, PA_UPDATE_MERGE, cl->client_info.info.proplist);
	i.corked = false;
	i.has_volume = true;
	i.volume_writable = true;

	d->cb(c, &i, 0, d->userdata);

	pa_proplist_free(i.proplist);
	return 0;
}

static void source_output_info(pa_operation *o, void *userdata)
{
	struct source_output_data *d = userdata;
	pa_context *c = o->context;
	struct global *g;
	int error;

	if ((g = pa_context_find_global(c, d->idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT))
		g = NULL;

	if (g) {
		error = source_output_callback(d, c, g);
	} else {
		error = PA_ERR_INVALID;
	}
	if (error)
		pa_context_set_error(c, error);
	d->cb(c, NULL, error ? -1 : 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_source_output_info(pa_context *c, uint32_t idx, pa_source_output_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct source_output_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, source_output_info, sizeof(struct source_output_data));
	d = o->userdata;
	d->idx = idx;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

static void source_output_info_list(pa_operation *o, void *userdata)
{
	struct source_output_data *d = userdata;
	pa_context *c = o->context;
	struct global *g;

	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT))
			continue;
		source_output_callback(d, c, g);
	}
	d->cb(c, NULL, 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_source_output_info_list(pa_context *c, pa_source_output_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct source_output_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, source_output_info_list, sizeof(struct source_output_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_move_source_output_by_name(pa_context *c, uint32_t idx, const char *source_name, pa_context_success_cb_t cb, void* userdata)
{
	pa_operation *o;
	struct target_node *d;

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_target_node, sizeof(struct target_node));
	d = o->userdata;
	d->idx = idx;
	d->mask = PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT;
	d->target_name = pa_xstrdup(source_name);
	d->target_mask = PA_SUBSCRIPTION_MASK_SOURCE;
	d->key = METADATA_TARGET_NODE;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_move_source_output_by_index(pa_context *c, uint32_t idx, uint32_t source_idx, pa_context_success_cb_t cb, void* userdata)
{
	pa_operation *o;
	struct target_node *d;

	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_target_node, sizeof(struct target_node));
	d = o->userdata;
	d->idx = idx;
	d->mask = PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT;
	d->target_idx = source_idx;
	d->target_mask = PA_SUBSCRIPTION_MASK_SOURCE;
	d->key = METADATA_TARGET_NODE;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_source_output_volume(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct stream_volume *d;

	pw_log_debug("contex %p: index %d", c, idx);
	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_stream_volume_mute, sizeof(struct stream_volume));
	d = o->userdata;
	d->idx = idx;
	d->mask = PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT;
	d->volume = *volume;
	d->have_volume = true;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_source_output_mute(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct stream_volume *d;

	pw_log_debug("contex %p: index %d", c, idx);
	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_stream_volume_mute, sizeof(struct stream_volume));
	d = o->userdata;
	d->idx = idx;
	d->mask = PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT;
	d->mute = mute;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_kill_source_output(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct kill_stream *d;

	pw_log_debug("contex %p: index %d", c, idx);
	pa_context_ensure_registry(c);

	o = pa_operation_new(c, NULL, do_kill_stream, sizeof(struct kill_stream));
	d = o->userdata;
	d->idx = idx;
	d->mask = PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_stat(pa_context *c, pa_stat_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation* pa_context_get_sample_info_by_name(pa_context *c, const char *name, pa_sample_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation* pa_context_get_sample_info_by_index(pa_context *c, uint32_t idx, pa_sample_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation* pa_context_get_sample_info_list(pa_context *c, pa_sample_info_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation* pa_context_get_autoload_info_by_name(pa_context *c, const char *name, pa_autoload_type_t type, pa_autoload_info_cb_t cb, void *userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation* pa_context_get_autoload_info_by_index(pa_context *c, uint32_t idx, pa_autoload_info_cb_t cb, void *userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation* pa_context_get_autoload_info_list(pa_context *c, pa_autoload_info_cb_t cb, void *userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation* pa_context_add_autoload(pa_context *c, const char *name, pa_autoload_type_t type, const char *module, const char*argument, pa_context_index_cb_t cb, void* userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation* pa_context_remove_autoload_by_name(pa_context *c, const char *name, pa_autoload_type_t type, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}

SPA_EXPORT
pa_operation* pa_context_remove_autoload_by_index(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void* userdata)
{
	pw_log_warn("Deprecated: Not Implemented");
	return NULL;
}
