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
static int has_device(struct port_device *devices, uint32_t id)
{
	uint32_t i;

	if (devices->devices == NULL || devices->n_devices == 0)
		return 1;

	for (i = 0; i < devices->n_devices; i++) {
		if (devices->devices[i] == id)
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
	char monitor_name[1024];
	pa_sink_info i;

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
		pa_channel_map_init_extend(&i.channel_map,
				i.sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
	i.owner_module = g->id;
	i.volume.channels = i.sample_spec.channels;
	for (n = 0; n < i.volume.channels; n++)
		i.volume.values[n] = pa_sw_volume_from_linear(g->node_info.volume * g->node_info.channel_volumes[n]);;
	i.mute = g->node_info.mute;
	i.monitor_source = g->node_info.monitor;
	snprintf(monitor_name, sizeof(monitor_name)-1, "%s.monitor", i.name);
	i.monitor_source_name = monitor_name;
	i.latency = 0;
	i.driver = "PipeWire";
	i.flags = PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY |
		  PA_SINK_DECIBEL_VOLUME;
	if (info->props && (str = spa_dict_lookup(info->props, PW_KEY_DEVICE_API)))
		i.flags |= PA_SINK_HARDWARE;
	if (SPA_FLAG_IS_SET(g->node_info.flags, NODE_FLAG_HW_VOLUME))
		  i.flags |= PA_SINK_HW_VOLUME_CTRL;
	if (SPA_FLAG_IS_SET(g->node_info.flags, NODE_FLAG_HW_MUTE))
		  i.flags |= PA_SINK_HW_MUTE_CTRL;
	i.proplist = pa_proplist_new_dict(info->props);
	i.configured_latency = 0;
	i.base_volume = pa_sw_volume_from_linear(g->node_info.base_volume);
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
			if (!has_device(&cg->card_info.port_devices[n], g->node_info.profile_device_id))
				continue;

			spa_zero(spi[j]);
			i.ports[j] = &spi[j];
			spi[j].name = ci->ports[n]->name;
			spi[j].description = ci->ports[n]->description;
			spi[j].priority = ci->ports[n]->priority;
			spi[j].available = ci->ports[n]->available;
			if (n == g->node_info.active_port)
				i.active_port = i.ports[j];
			j++;
		}
		i.n_ports = j;
		if (i.n_ports == 0)
			i.ports = NULL;
		else
			i.ports[j] = NULL;
	}
	if (i.active_port == NULL && i.n_ports > 0)
		i.active_port = i.ports[0];
	i.n_formats = pw_array_get_len(&g->node_info.formats, pa_format_info *);
	i.formats = g->node_info.formats.data;
	d->cb(c, &i, 0, d->userdata);
	pa_proplist_free(i.proplist);
	return 0;
}

static void sink_info(pa_operation *o, void *userdata)
{
	struct sink_data *d = userdata;
	struct global *g;
	pa_context *c = o->context;
	int error = 0;

	pw_log_debug("%p name:%s idx:%u", c, d->name, d->idx);

	if (d->name) {
		g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_SINK, d->name);
		pa_xfree(d->name);
	} else {
		if ((g = pa_context_find_global(c, d->idx)) == NULL ||
		    !(g->mask & PA_SUBSCRIPTION_MASK_SINK))
			g = NULL;
	}

	if (g) {
		error = sink_callback(c, g, d);
	} else {
		error = PA_ERR_NOENTITY;
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

	pw_log_debug("%p: name %s", c, name);

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

	pw_log_debug("%p: index %u", c, idx);

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

	pw_log_debug("%p", c);
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
			channel_volumes[i] = pa_sw_volume_to_linear(volume->values[i]);;
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
			channel_volumes[i] = pa_sw_volume_to_linear(volume->values[i]);
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
	g->changed++;

	if (!SPA_FLAG_IS_SET(g->permissions, PW_PERM_W | PW_PERM_X))
		return PA_ERR_ACCESS;

	pw_log_debug("node %p: id:%u", g, g->id);
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
			channel_volumes[i] = pa_sw_volume_to_linear(volume->values[i]);
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
	g->changed++;

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

	pw_log_debug("device %p: id:%u", cg, cg->id);
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

	pw_log_debug("card:%u global:%u flags:%08x", card_id, g->id, g->node_info.flags);

	if (SPA_FLAG_IS_SET(g->node_info.flags, NODE_FLAG_DEVICE_VOLUME | NODE_FLAG_DEVICE_MUTE) &&
	    (cg = pa_context_find_global(c, card_id)) != NULL) {
		id = cg->node_info.active_port;
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
		error = PA_ERR_NOENTITY;
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

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(c, pa_cvolume_valid(volume), PA_ERR_INVALID);

	pw_log_debug("context %p: index %d", c, idx);

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

	pw_log_debug("context %p: name:%s suspend:%d", c, sink_name, suspend);
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_suspend_sink_by_index(pa_context *c, uint32_t idx, int suspend,  pa_context_success_cb_t cb, void* userdata)
{
	pa_operation *o;
	struct success_ack *d;

	pw_log_debug("context %p: index:%u suspend:%d", c, idx, suspend);
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->userdata = userdata;
	pa_operation_sync(o);

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
		pw_log_debug("port id:%u name:\"%s\" dir:%d", test_id, name, test_direction);
		if (test_direction != direction)
			continue;
		if (strcmp(name, port) == 0) {
			id = test_id;
			break;
		}
	}
	pw_log_debug("port %s, id %u", port, id);
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
		error = PA_ERR_NOENTITY;
	}
	if (error != 0)
		pa_context_set_error(c, error);
	if (d->cb)
		d->cb(c, error != 0 ? 0 : 1, d->userdata);
	pa_xfree(d->port);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_set_sink_port_by_index(pa_context *c, uint32_t idx, const char*port, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct device_route *d;

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pw_log_debug("context %p: index:%u port:%s", c, idx, port);
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

	pw_log_debug("context %p: name:%s port:%s", c, name, port);
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
			pa_proplist_setf(i.proplist, PW_KEY_NODE_DESCRIPTION, "Monitor of %s", str);
		pa_proplist_setf(i.proplist, PW_KEY_DEVICE_CLASS, "monitor");
	}

	if ((str = pa_proplist_gets(i.proplist, PW_KEY_NODE_NAME)))
		i.name = str;
	else
		i.name = "unknown";

	pw_log_debug("source %d %s monitor:%d", g->id, i.name, monitor);

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
		pa_channel_map_init_extend(&i.channel_map,
				i.sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
	i.owner_module = g->id;
	i.volume.channels = i.sample_spec.channels;
	for (n = 0; n < i.volume.channels; n++)
		i.volume.values[n] = pa_sw_volume_from_linear(g->node_info.volume * g->node_info.channel_volumes[n]);;
	i.mute = g->node_info.mute;
	if (monitor) {
		i.index = g->node_info.monitor;
		i.monitor_of_sink = g->id;
		i.monitor_of_sink_name = pa_context_find_global_name(c, g->id);
	} else {
		i.index = g->id;
		i.monitor_of_sink = PA_INVALID_INDEX;
		i.monitor_of_sink_name = NULL;
		if (info->props && (str = spa_dict_lookup(info->props, PW_KEY_DEVICE_API)))
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
	i.base_volume = pa_sw_volume_from_linear(g->node_info.base_volume);
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
			if (!has_device(&cg->card_info.port_devices[n], g->node_info.profile_device_id))
				continue;

			spa_zero(spi[j]);
			i.ports[j] = &spi[j];
			spi[j].name = ci->ports[n]->name;
			spi[j].description = ci->ports[n]->description;
			spi[j].priority = ci->ports[n]->priority;
			spi[j].available = ci->ports[n]->available;
			if (n == g->node_info.active_port)
				i.active_port = i.ports[j];
			j++;
		}
		i.n_ports = j;
		if (i.n_ports == 0)
			i.ports = NULL;
		else
			i.ports[j] = NULL;
	}
	if (i.active_port == NULL && i.n_ports > 0)
		i.active_port = i.ports[0];
	i.n_formats = pw_array_get_len(&g->node_info.formats, pa_format_info *);
	i.formats = g->node_info.formats.data;
	d->cb(c, &i, 0, d->userdata);
	pa_proplist_free(i.proplist);
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
		    (((g = pa_context_find_global(c, d->idx & PA_IDX_MASK_MONITOR)) == NULL ||
		    !(g->mask & PA_SUBSCRIPTION_MASK_SOURCE))))
			g = NULL;
	}

	if (g) {
		error = source_callback(c, g, d);
	} else {
		error = PA_ERR_NOENTITY;
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

	pw_log_debug("context %p: name:%s", c, name);
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

	pw_log_debug("context %p", c);
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

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(c, pa_cvolume_valid(volume), PA_ERR_INVALID);

	pw_log_debug("context %p: index %d", c, idx);

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

	pw_log_debug("context %p: name:%s", c, source_name);

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_suspend_source_by_index(pa_context *c, uint32_t idx, int suspend, pa_context_success_cb_t cb, void* userdata)
{
	pa_operation *o;
	struct success_ack *d;

	pw_log_debug("context %p: index:%u", c, idx);
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_source_port_by_index(pa_context *c, uint32_t idx, const char*port, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct device_route *d;

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pw_log_debug("context %p: idx %d port:%s", c, idx, port);

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

	pw_log_debug("context %p: name %s port:%s", c, name, port);

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
	const char *str;
	uint32_t id = SPA_ID_INVALID;


	if (c->metadata) {
		if (mask & PA_SUBSCRIPTION_MASK_SINK)
			id = c->default_sink;
		else if (mask & PA_SUBSCRIPTION_MASK_SOURCE)
			id = c->default_source;
		else
			return NULL;
	}
	spa_list_for_each(g, &c->globals, link) {
		if ((g->mask & mask) != mask)
			continue;
		if (g->props != NULL &&
		    (str = pw_properties_get(g->props, PW_KEY_NODE_NAME)) != NULL &&
		    (id == SPA_ID_INVALID || id == g->id))
			return str;
	}
	return "unknown";
}

static void server_callback(struct server_data *d, pa_context *c)
{
	const struct pw_core_info *info = c->core_info;
	const char *str;
	pa_server_info i;
	char name[1024];

	snprintf(name, sizeof(name)-1, "pulseaudio (on PipeWire %s)", info->version);

	spa_zero(i);
	i.user_name = info->user_name;
	i.host_name = info->host_name;
	i.server_version = pa_get_headers_version();
	i.server_name = name;
	i.sample_spec.format = PA_SAMPLE_FLOAT32NE;
	if (info->props && (str = spa_dict_lookup(info->props, "default.clock.rate")) != NULL)
		i.sample_spec.rate = atoi(str);
	else
		i.sample_spec.rate = 44100;
	i.sample_spec.channels = 2;
	i.default_sink_name = get_default_name(c, PA_SUBSCRIPTION_MASK_SINK);
	i.default_source_name = get_default_name(c, PA_SUBSCRIPTION_MASK_SOURCE);
	i.cookie = info->cookie;
	pa_channel_map_init_extend(&i.channel_map, i.sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
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

	pw_log_debug("context %p", c);
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
		error = PA_ERR_NOENTITY;
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

	pw_log_debug("context %p index:%u", c, idx);
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

	pw_log_debug("context %p", c);
	o = pa_operation_new(c, NULL, module_info_list, sizeof(struct module_data));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

struct load_module {
	pa_context_index_cb_t cb;
	int error;
	void *userdata;
	uint32_t idx;
	struct pw_properties *props;
	struct pw_proxy *proxy;
	struct spa_hook listener;
};

static struct module_info *find_module(pa_context *c, uint32_t idx)
{
	struct module_info *m;
	spa_list_for_each(m, &c->modules, link) {
		if (m->id == idx)
			return m;
	}
	return NULL;
}

static void on_load_module(pa_operation *o, void *userdata)
{
	struct load_module *d = userdata;
	pa_context *c = o->context;
	if (d->error != 0)
		pa_context_set_error(c, d->error);
	if (d->cb)
		d->cb(c, d->idx, d->userdata);
	if (d->props)
		pw_properties_free(d->props);
	if (d->proxy)
		spa_hook_remove(&d->listener);
	pa_operation_done(o);
}

static void module_proxy_removed(void *data)
{
	struct module_info *m = data;
	pw_proxy_destroy(m->proxy);
}

static void module_proxy_destroy(void *data)
{
	struct module_info *m = data;
	spa_hook_remove(&m->listener);
	spa_list_remove(&m->link);
	free(m);
}

static void module_proxy_bound(void *data, uint32_t global_id)
{
	struct module_info *m;
	pa_operation *o = data;
	pa_context *c = o->context;
	struct load_module *d = o->userdata;
	static const struct pw_proxy_events proxy_events = {
		.removed = module_proxy_removed,
		.destroy = module_proxy_destroy,
	};
	d->idx = global_id;

	m = calloc(1, sizeof(struct module_info));
	m->id = global_id;
	m->proxy = d->proxy;
	pw_proxy_add_listener(m->proxy, &m->listener, &proxy_events, m);
	spa_list_append(&c->modules, &m->link);
	on_load_module(o, d);
}

static void module_proxy_error(void *data, int seq, int res, const char *message)
{
	pa_operation *o = data;
	struct load_module *d = o->userdata;
	d->error = res;
	d->idx = PA_INVALID_INDEX;
	pw_proxy_destroy(d->proxy);
	on_load_module(o, d);
}

static int load_null_sink_module(pa_operation *o)
{
	struct load_module *d = o->userdata;
	pa_context *c = o->context;
	static const struct pw_proxy_events proxy_events = {
		.bound = module_proxy_bound,
		.error = module_proxy_error,
	};

	if (d->proxy != NULL)
		return -EBUSY;

	d->proxy = pw_core_create_object(c->core,
                                "adapter",
                                PW_TYPE_INTERFACE_Node,
                                PW_VERSION_NODE,
                                d->props ? &d->props->dict : NULL, 0);
	if (d->proxy == NULL)
		return -errno;

	pw_proxy_add_listener(d->proxy, &d->listener, &proxy_events, o);
	return 0;
}

static void add_props(struct pw_properties *props, const char *str)
{
	char *s = strdup(str), *p = s, *e, f;
	const char *k, *v;

	while (*p) {
		e = strchr(p, '=');
		if (e == NULL)
			break;
		*e = '\0';
		k = p;
		p = e+1;

		if (*p == '\"') {
			p++;
			f = '\"';
		} else {
			f = ' ';
		}
		e = strchr(p, f);
		if (e == NULL)
			break;
		*e = '\0';
		v = p;
		p = e + 1;
		pw_properties_set(props, k, v);
	}
	free(s);
}

SPA_EXPORT
pa_operation* pa_context_load_module(pa_context *c, const char*name, const char *argument, pa_context_index_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct load_module *d;
	int error = PA_ERR_NOTIMPLEMENTED;;
	struct pw_properties *props = NULL;
	const char *str;
	bool sync = true;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(name != NULL);

	pw_log_debug("context %p: name:%s arg:%s", c, name, argument);

	o = pa_operation_new(c, NULL, on_load_module, sizeof(struct load_module));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	d->idx = PA_INVALID_INDEX;

	if (strcmp(name, "module-null-sink") == 0) {
		props = pw_properties_new_string(argument);
		if (props == NULL) {
			error = PA_ERR_INVALID;
			goto done;
		}
		if ((str = pw_properties_get(props, "sink_name")) != NULL) {
			pw_properties_set(props, "node.name", str);
			pw_properties_set(props, "sink_name", NULL);
		} else {
			pw_properties_set(props, "node.name", "null");
		}
		if ((str = pw_properties_get(props, "sink_properties")) != NULL) {
			add_props(props, str);
			pw_properties_set(props, "sink_properties", NULL);
		}
		if ((str = pw_properties_get(props, "device.description")) != NULL) {
			pw_properties_set(props, "node.description", str);
			pw_properties_set(props, "device.description", NULL);
		}
		pw_properties_set(props, "factory.name", "support.null-audio-sink");

		d->props = props;
		error = load_null_sink_module(o);
		sync = error < 0;
	}
done:
	d->error = error;
	if (sync)
		pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_unload_module(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_ack *d;
	struct module_info *m;
	int error;

	pw_log_debug("context %p: %u", c, idx);
	if ((m = find_module(c, idx)) != NULL) {
		pw_proxy_destroy(m->proxy);
		error = 0;
	} else {
		error = PA_ERR_NOENTITY;
	}
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->userdata = userdata;
	d->error = error;
	d->idx = idx;
	pa_operation_sync(o);

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
		error = PA_ERR_NOENTITY;
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

	pw_log_debug("context %p: index:%u", c, idx);
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

	pw_log_debug("context %p", c);
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
		error = PA_ERR_NOENTITY;
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

	pw_log_debug("context %p: index:%u", c, idx);
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
		error = PA_ERR_NOENTITY;
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

	pw_log_debug("context %p: index:%u", c, idx);
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

	pw_log_debug("context %p: name:%s", c, name);
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
		error = PA_ERR_NOENTITY;
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
		error = PA_ERR_NOENTITY;
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
	free(d->profile);
	pa_operation_done(o);
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

	pw_log_debug("%p: index:%u profile:%s", c, idx, profile);
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

	pw_log_debug("%p: name:%s profile:%s", c, name, profile);
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

	pw_log_debug("%p: card_name:%s port_name:%s offset:%"PRIi64, c, card_name, port_name, offset);
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
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

	s = pa_context_find_stream(c, g->id);

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
	i.owner_module = g->id;
	i.client = g->node_info.client_id;
	if (s)
		i.sink = s->device_index;
	else
		i.sink = g->node_info.device_index;

	if (s && s->sample_spec.channels > 0) {
		i.sample_spec = s->sample_spec;
		if (s->channel_map.channels == s->sample_spec.channels)
			i.channel_map = s->channel_map;
		else
			pa_channel_map_init_extend(&i.channel_map,
					i.sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
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
			pa_channel_map_init_extend(&i.channel_map,
					i.sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
		ii[0].encoding = PA_ENCODING_PCM;
		ii[0].plist = pa_proplist_new();
		i.format = ii;
	}
	pa_cvolume_init(&i.volume);
	i.volume.channels = i.sample_spec.channels;
	for (n = 0; n < i.volume.channels; n++)
		i.volume.values[n] = pa_sw_volume_from_linear(g->node_info.volume * g->node_info.channel_volumes[n]);

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
		error = PA_ERR_NOENTITY;
	}
	if (error)
		pa_context_set_error(c, error);
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
	struct global *g, *t;
	int error = 0;

	pw_log_debug("%p", c);

	if ((g = pa_context_find_global(c, d->idx)) == NULL ||
	    !(g->mask & d->mask)) {
		error = PA_ERR_NOENTITY;
		goto done;
	}

	if (d->target_name) {
		t = pa_context_find_global_by_name(c, d->target_mask, d->target_name);
	} else {
		if ((t = pa_context_find_global(c, d->target_idx)) == NULL ||
		    !(t->mask & d->target_mask))
			t = NULL;
	}
	if (t == NULL) {
		error = PA_ERR_NOENTITY;
	} else if (!SPA_FLAG_IS_SET(g->permissions, PW_PERM_M) ||
		(c->metadata && !SPA_FLAG_IS_SET(c->metadata->permissions, PW_PERM_W|PW_PERM_X))) {
		error = PA_ERR_ACCESS;
	} else if (c->metadata) {
		char buf[16];
		snprintf(buf, sizeof(buf), "%d", t->id);
		pw_metadata_set_property(c->metadata->proxy,
				g->id, d->key, SPA_TYPE_INFO_BASE "Id", buf);
	} else {
		error = PA_ERR_NOTIMPLEMENTED;
	}
done:
	if (error != 0)
		pa_context_set_error(c, error);
	if (d->cb)
		d->cb(c, error != 0 ? 0 : 1, d->userdata);
	pa_xfree(d->target_name);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_move_sink_input_by_name(pa_context *c, uint32_t idx, const char *sink_name, pa_context_success_cb_t cb, void* userdata)
{
	pa_operation *o;
	struct target_node *d;

	pw_log_debug("%p: index:%u name:%s", c, idx, sink_name);
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

	pw_log_debug("%p: index:%u sink_index:%u", c, idx, sink_idx);
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

	if ((s = pa_context_find_stream(c, d->idx)) == NULL) {
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
		error = PA_ERR_NOENTITY;
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

	pw_log_debug("context %p: index %d", c, idx);
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

	pw_log_debug("context %p: index %d", c, idx);
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

	if ((s = pa_context_find_stream(c, d->idx)) == NULL) {
		if ((g = pa_context_find_global(c, d->idx)) == NULL ||
		    !(g->mask & d->mask))
			g = NULL;
	}
	if (s) {
		pw_stream_destroy(s->stream);
	} else if (g) {
		pw_registry_destroy(c->registry, g->id);
	} else {
		error = PA_ERR_NOENTITY;
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

	pw_log_debug("context %p: index %d", c, idx);
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
	struct global *cl;
	struct pw_node_info *info = g->info;
	const char *name = NULL;
	uint32_t n;
	pa_source_output_info i;
	pa_format_info ii[1];
	pa_stream *s;

	pw_log_debug("index %d", g->id);
	if (info == NULL)
		return PA_ERR_INVALID;

	s = pa_context_find_stream(c, g->id);

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
	i.owner_module = g->id;
	i.client = g->node_info.client_id;
	if (s)
		i.source = s->device_index;
	else
		i.source = g->node_info.device_index;
	if (s && s->sample_spec.channels > 0) {
		i.sample_spec = s->sample_spec;
		if (s->channel_map.channels == s->sample_spec.channels)
			i.channel_map = s->channel_map;
		else
			pa_channel_map_init_extend(&i.channel_map,
					i.sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
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
			pa_channel_map_init_extend(&i.channel_map,
					i.sample_spec.channels, PA_CHANNEL_MAP_DEFAULT);
		ii[0].encoding = PA_ENCODING_PCM;
		ii[0].plist = pa_proplist_new();
		i.format = ii;
	}
	pa_cvolume_init(&i.volume);
	i.volume.channels = i.sample_spec.channels;
	for (n = 0; n < i.volume.channels; n++)
		i.volume.values[n] = pa_sw_volume_from_linear(g->node_info.volume * g->node_info.channel_volumes[n]);

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
		error = PA_ERR_NOENTITY;
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

	pw_log_debug("%p: index:%u", c, idx);
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

	pw_log_debug("%p", c);
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

	pw_log_debug("%p index:%u name:%s", c, idx, source_name);
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

	pw_log_debug("%p index:%u source_index:%u", c, idx, source_idx);
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

	pw_log_debug("context %p: index %d", c, idx);
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

	pw_log_debug("context %p: index %d", c, idx);
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

	pw_log_debug("context %p: index %d", c, idx);
	o = pa_operation_new(c, NULL, do_kill_stream, sizeof(struct kill_stream));
	d = o->userdata;
	d->idx = idx;
	d->mask = PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

struct stat_ack {
	pa_stat_info_cb_t cb;
	int error;
	void *userdata;
};

static void on_stat_info(pa_operation *o, void *userdata)
{
	struct stat_ack *d = userdata;
	pa_context *c = o->context;
	pa_stat_info i;
	spa_zero(i);
	if (d->error != 0)
		pa_context_set_error(c, d->error);
	if (d->cb)
		d->cb(c, &i, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_stat(pa_context *c, pa_stat_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct stat_ack *d;

	pw_log_debug("%p", c);
	o = pa_operation_new(c, NULL, on_stat_info, sizeof(struct stat_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

struct sample_info {
	pa_sample_info_cb_t cb;
	int error;
	void *userdata;
};

static void on_sample_info(pa_operation *o, void *userdata)
{
	struct sample_info *d = userdata;
	pa_context *c = o->context;
	if (d->error != 0)
		pa_context_set_error(c, d->error);
	if (d->cb)
		d->cb(c, NULL, d->error ? -1 : 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_sample_info_by_name(pa_context *c, const char *name, pa_sample_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct sample_info *d;

	pw_log_debug("%p nane:%s", c, name);
	o = pa_operation_new(c, NULL, on_sample_info, sizeof(struct sample_info));
	d = o->userdata;
	d->cb = cb;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_get_sample_info_by_index(pa_context *c, uint32_t idx, pa_sample_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct sample_info *d;

	pw_log_debug("%p index:%u", c, idx);
	o = pa_operation_new(c, NULL, on_sample_info, sizeof(struct sample_info));
	d = o->userdata;
	d->cb = cb;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}


static void on_sample_info_list(pa_operation *o, void *userdata)
{
	struct sample_info *d = userdata;
	pa_context *c = o->context;
	if (d->error != 0)
		pa_context_set_error(c, d->error);
	if (d->cb)
		d->cb(c, NULL, 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_sample_info_list(pa_context *c, pa_sample_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct sample_info *d;

	pw_log_debug("%p", c);
	o = pa_operation_new(c, NULL, on_sample_info_list, sizeof(struct sample_info));
	d = o->userdata;
	d->cb = cb;
	d->error = PA_ERR_NOTIMPLEMENTED;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
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
