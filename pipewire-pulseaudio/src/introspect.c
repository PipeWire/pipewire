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

#include <pulse/introspect.h>

#include "internal.h"

struct success_ack {
	pa_context_success_cb_t cb;
	int error;
	void *userdata;
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

static int wait_global(pa_context *c, struct global *g, pa_operation *o)
{
	if (g->init) {
		pa_operation_sync(o);
		return -EBUSY;
	}
	return 0;
}

static int wait_globals(pa_context *c, pa_subscription_mask_t mask, pa_operation *o)
{
	struct global *g;
	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & mask))
			continue;
		if (wait_global(c, g, o) < 0)
			return -EBUSY;
	}
	return 0;
}

static void sink_callback(struct sink_data *d)
{
	struct global *g = d->global;
	struct pw_node_info *info = g->info;
	const char *str;
	uint32_t n;
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

	i.sample_spec.format = PA_SAMPLE_S16LE;
	i.sample_spec.rate = 44100;
	if (g->node_info.n_channel_volumes)
		i.sample_spec.channels = g->node_info.n_channel_volumes;
	else
		i.sample_spec.channels = 2;
	pa_channel_map_init_auto(&i.channel_map, i.sample_spec.channels, PA_CHANNEL_MAP_OSS);
	i.owner_module = 0;
	i.volume.channels = i.sample_spec.channels;
	for (n = 0; n < i.volume.channels; n++)
		i.volume.values[n] = g->node_info.volume * g->node_info.channel_volumes[n] * PA_VOLUME_NORM;
	i.mute = g->node_info.mute;
	i.monitor_source = g->node_info.monitor;
	i.monitor_source_name = "unknown";
	i.latency = 0;
	i.driver = "PipeWire";
	i.flags = PA_SINK_HARDWARE |
		  PA_SINK_HW_VOLUME_CTRL | PA_SINK_HW_MUTE_CTRL |
		  PA_SINK_LATENCY | PA_SINK_DYNAMIC_LATENCY |
		  PA_SINK_DECIBEL_VOLUME;
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
	int eol = 1;

	if (d->global) {
		if (wait_global(d->context, d->global, o) < 0)
			return;
		sink_callback(d);
	} else {
		pa_context_set_error(d->context, PA_ERR_INVALID);
		eol = -1;
	}
	d->cb(d->context, NULL, eol, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
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

	g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_SINK, name);

	o = pa_operation_new(c, NULL, sink_info, sizeof(struct sink_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	d->global = g;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
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

	if ((g = pa_context_find_global(c, idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_SINK))
		g = NULL;

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

	if (wait_globals(c, PA_SUBSCRIPTION_MASK_SINK, o) < 0)
		return;
	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SINK))
			continue;
		d->global = g;
		sink_callback(d);
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

	o = pa_operation_new(c, NULL, sink_info_list, sizeof(struct sink_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

static void set_stream_volume(pa_context *c, pa_stream *s, const pa_cvolume *volume, bool mute)
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
}

static void set_node_volume(pa_context *c, struct global *g, const pa_cvolume *volume, bool mute)
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
			return;

		memcpy(g->node_info.channel_volumes, vols, n_channel_volumes * sizeof(float));
		g->node_info.n_channel_volumes = n_channel_volumes;
	} else {
		n_channel_volumes = g->node_info.n_channel_volumes;
		vols = g->node_info.channel_volumes;
		if (mute == g->node_info.mute)
			return;
	}
	g->node_info.mute = mute;

	pw_node_set_param((struct pw_node*)g->proxy,
		SPA_PARAM_Props, 0,
		spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Props,	SPA_PARAM_Props,
			SPA_PROP_mute,			SPA_POD_Bool(mute),
			SPA_PROP_channelVolumes,	SPA_POD_Array(sizeof(float),
								SPA_TYPE_Float,
								n_channel_volumes,
								vols)));
}


SPA_EXPORT
pa_operation* pa_context_set_sink_volume_by_index(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct success_ack *d;
	int error = 0;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	pw_log_debug("context %p: index %d", c, idx);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(c, pa_cvolume_valid(volume), PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_SINK)) {
		error = PA_ERR_INVALID;
	} else {
		set_node_volume(c, g, volume, g->node_info.mute);
	}
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = error;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_sink_volume_by_name(pa_context *c, const char *name, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct success_ack *d;
	int error = 0;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(c, pa_cvolume_valid(volume), PA_ERR_INVALID);

	pw_log_debug("context %p: name %s", c, name);

	if ((g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_SINK, name)) == NULL) {
		error = PA_ERR_INVALID;
	} else {
		set_node_volume(c, g, volume, g->node_info.mute);
	}
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = error;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_sink_mute_by_index(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct success_ack *d;
	int error = 0;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pw_log_debug("context %p: index %d", c, idx);

	if ((g = pa_context_find_global(c, idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_SINK)) {
		error = PA_ERR_INVALID;
	} else {
		set_node_volume(c, g, NULL, mute);
	}
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = error;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_sink_mute_by_name(pa_context *c, const char *name, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct success_ack *d;
	int error = 0;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

	pw_log_debug("context %p: name %s", c, name);

	if ((g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_SINK, name)) == NULL) {
		error = PA_ERR_INVALID;
	} else {
		set_node_volume(c, g, NULL, mute);
	}

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = error;
	d->userdata = userdata;
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

SPA_EXPORT
pa_operation* pa_context_set_sink_port_by_index(pa_context *c, uint32_t idx, const char*port, pa_context_success_cb_t cb, void *userdata)
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
pa_operation* pa_context_set_sink_port_by_name(pa_context *c, const char*name, const char*port, pa_context_success_cb_t cb, void *userdata)
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
	const char *str;
	uint32_t n;
	pa_source_info i;
	pa_format_info ii[1];
	pa_format_info *ip[1];
	enum pa_sink_flags flags;

	flags = PA_SOURCE_LATENCY | PA_SOURCE_DYNAMIC_LATENCY |
		  PA_SOURCE_DECIBEL_VOLUME;

	spa_zero(i);
	if (info->props && (str = spa_dict_lookup(info->props, PW_KEY_NODE_NAME)))
		i.name = str;
	else
		i.name = "unknown";
	i.index = g->id;
	if (info->props && (str = spa_dict_lookup(info->props, PW_KEY_NODE_DESCRIPTION)))
		i.description = str;
	else
		i.description = "unknown";
	i.sample_spec.format = PA_SAMPLE_S16LE;
	i.sample_spec.rate = 44100;
	if (g->node_info.n_channel_volumes)
		i.sample_spec.channels = g->node_info.n_channel_volumes;
	else
		i.sample_spec.channels = 2;
	pa_channel_map_init_auto(&i.channel_map, i.sample_spec.channels, PA_CHANNEL_MAP_OSS);
	i.owner_module = 0;
	i.volume.channels = i.sample_spec.channels;
	for (n = 0; n < i.volume.channels; n++)
		i.volume.values[n] = g->node_info.volume * g->node_info.channel_volumes[n] * PA_VOLUME_NORM;
	i.mute = g->node_info.mute;
	if (g->mask & PA_SUBSCRIPTION_MASK_SINK) {
		i.monitor_of_sink = g->id;
		i.monitor_of_sink_name = "unknown";
		i.index = g->node_info.monitor;
	} else {
		i.monitor_of_sink = PA_INVALID_INDEX;
		i.monitor_of_sink_name = NULL;
		flags |= PA_SOURCE_HARDWARE | PA_SOURCE_HW_VOLUME_CTRL | PA_SOURCE_HW_MUTE_CTRL;
	}
	i.latency = 0;
	i.driver = "PipeWire";
	i.flags = flags;
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
	int eol = 1;

	if (d->global) {
		if (wait_global(d->context, d->global, o) < 0)
			return;
		source_callback(d);
	} else {
		pa_context_set_error(d->context, PA_ERR_INVALID);
		eol = -1;
	}
	d->cb(d->context, NULL, eol, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
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

	g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_SOURCE, name);

	o = pa_operation_new(c, NULL, source_info, sizeof(struct source_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	d->global = g;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_get_source_info_by_index(pa_context *c, uint32_t idx, pa_source_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct source_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pw_log_debug("context %p: index %d", c, idx);

	if (((g = pa_context_find_global(c, idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_SOURCE)) &&
	    (((g = pa_context_find_global(c, idx & PA_IDX_MASK_DSP)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_SOURCE))))
		g = NULL;

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

	if (wait_globals(c, PA_SUBSCRIPTION_MASK_SOURCE, o) < 0)
		return;
	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SOURCE))
			continue;
		d->global = g;
		source_callback(d);
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

	o = pa_operation_new(c, NULL, source_info_list, sizeof(struct source_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_source_volume_by_index(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct success_ack *d;
	int error = 0;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	pw_log_debug("context %p: index %d", c, idx);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(c, pa_cvolume_valid(volume), PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_SOURCE)) {
		error = PA_ERR_INVALID;
	} else {
		set_node_volume(c, g, volume, g->node_info.mute);
	}

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = error;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_source_volume_by_name(pa_context *c, const char *name, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct success_ack *d;
	int error = 0;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);
	PA_CHECK_VALIDITY_RETURN_NULL(c, pa_cvolume_valid(volume), PA_ERR_INVALID);

	pw_log_debug("context %p: name %s", c, name);

	if ((g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_SOURCE, name)) == NULL) {
		error = PA_ERR_INVALID;
	} else {
		set_node_volume(c, g, volume, g->node_info.mute);
	}

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = error;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_source_mute_by_index(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct success_ack *d;
	int error = 0;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	pw_log_debug("context %p: index %d", c, idx);

	if ((g = pa_context_find_global(c, idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_SOURCE)) {
		error = PA_ERR_INVALID;
	} else {
		set_node_volume(c, g, NULL, mute);
	}

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = error;
	d->userdata = userdata;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_source_mute_by_name(pa_context *c, const char *name, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct success_ack *d;
	int error = 0;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

	pw_log_debug("context %p: name %s", c, name);

	if ((g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_SOURCE, name)) == NULL) {
		error = PA_ERR_INVALID;
	} else {
		set_node_volume(c, g, NULL, mute);
	}

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = error;
	d->userdata = userdata;
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
pa_operation* pa_context_set_source_port_by_name(pa_context *c, const char*name, const char*port, pa_context_success_cb_t cb, void *userdata)
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

struct server_data {
	pa_context *context;
	pa_server_info_cb_t cb;
	void *userdata;
	struct global *global;
};

static void server_callback(struct server_data *d)
{
	pa_context *c = d->context;
	const struct pw_core_info *info = c->core_info;
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
        pa_channel_map_init_extend(&i.channel_map, i.sample_spec.channels, PA_CHANNEL_MAP_OSS);
	d->cb(d->context, &i, d->userdata);
}

static void server_info(pa_operation *o, void *userdata)
{
	struct server_data *d = userdata;
	server_callback(d);
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
	int eol = 1;

	if (d->global) {
		module_callback(d);
	} else {
		pa_context_set_error(d->context, PA_ERR_INVALID);
		eol = -1;
	}
	d->cb(d->context, NULL, eol, d->userdata);

	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_module_info(pa_context *c, uint32_t idx, pa_module_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct module_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_MODULE))
		g = NULL;

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

SPA_EXPORT
pa_operation* pa_context_get_module_info_list(pa_context *c, pa_module_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct module_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	o = pa_operation_new(c, NULL, module_info_list, sizeof(struct module_data));
	d = o->userdata;
	d->context = c;
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
	int eol = 1;

	if (d->global) {
		client_callback(d);
	} else {
		pa_context_set_error(d->context, PA_ERR_INVALID);
		eol = -1;
	}
	d->cb(d->context, NULL, eol, d->userdata);

	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_client_info(pa_context *c, uint32_t idx, pa_client_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct client_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_CLIENT))
		g = NULL;

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

SPA_EXPORT
pa_operation* pa_context_get_client_info_list(pa_context *c, pa_client_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct client_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	o = pa_operation_new(c, NULL, client_info_list, sizeof(struct client_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_kill_client(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	struct global *g;
	pa_operation *o;
	struct success_ack *d;
	int error = 0;

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_CLIENT)) {
		error = PA_ERR_INVALID;
	} else {
		pw_registry_destroy(c->registry, g->id);
	}

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = error;
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
	struct param *p;

	n_profiles = g->card_info.n_profiles;

	i->profiles = alloca(sizeof(pa_card_profile_info) * n_profiles);
	i->profiles2 = alloca(sizeof(pa_card_profile_info2 *) * n_profiles);
	i->n_profiles = 0;

	pw_log_debug("context %p: info for %d", g->context, g->id);

	spa_list_for_each(p, &g->card_info.profiles, link) {
		uint32_t id;
		const char *name;

		if (spa_pod_parse_object(p->param,
				SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(&id),
				SPA_PARAM_PROFILE_name,  SPA_POD_String(&name)) < 0) {
			pw_log_warn("device %d: can't parse profile", g->id);
			continue;
		}

		j = i->n_profiles++;
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
	}
	d->cb(d->context, i, 0, d->userdata);
}

static void card_info(pa_operation *o, void *userdata)
{
	struct card_data *d = userdata;
	int eol = 1;

	if (d->global) {
		if (wait_global(d->context, d->global, o) < 0)
			return;
		card_callback(d);
	} else {
		pa_context_set_error(d->context, PA_ERR_INVALID);
		eol = -1;
	}
	d->cb(d->context, NULL, eol, d->userdata);

	pa_operation_done(o);
}

SPA_EXPORT
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

	pw_log_debug("context %p: %u", c, idx);

	if ((g = pa_context_find_global(c, idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_CARD))
		g = NULL;

	o = pa_operation_new(c, NULL, card_info, sizeof(struct card_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	d->global = g;
	pa_operation_sync(o);
	return o;
}

SPA_EXPORT
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

	pw_log_debug("context %p: %s", c, name);

	g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_CARD, name);

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

	if (wait_globals(c, PA_SUBSCRIPTION_MASK_CARD, o) < 0)
		return;
	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_CARD))
			continue;
		d->global = g;
		card_callback(d);
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
	int res = 0;
	uint32_t id = SPA_ID_INVALID;
	char buf[1024];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct param *p;

	if (g == NULL) {
		pa_context_set_error(c, PA_ERR_INVALID);
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
	if (id == SPA_ID_INVALID)
		goto done;;

	pw_device_set_param((struct pw_device*)g->proxy,
			SPA_PARAM_Profile, 0,
			spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(id)));
	res = 1;
done:
	if (d->success_cb)
		d->success_cb(c, res, d->userdata);
	pa_operation_done(o);
	free(d->profile);
}

SPA_EXPORT
pa_operation* pa_context_set_card_profile_by_index(pa_context *c, uint32_t idx, const char*profile, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct card_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_CARD))
		g = NULL;

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

SPA_EXPORT
pa_operation* pa_context_set_card_profile_by_name(pa_context *c, const char*name, const char*profile, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct card_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY_RETURN_NULL(c, !name || *name, PA_ERR_INVALID);

	g = pa_context_find_global_by_name(c, PA_SUBSCRIPTION_MASK_CARD, name);

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
	pa_context *context;
	pa_sink_input_info_cb_t cb;
	void *userdata;
	struct global *global;
};

static void sink_input_callback(struct sink_input_data *d)
{
	struct global *g = d->global, *cl;
	struct pw_node_info *info = g->info;
	const char *name = NULL;
	uint32_t n;
	pa_sink_input_info i;
	pa_format_info ii[1];
	pa_stream *s;

	if (info == NULL)
		return;

	s = find_stream(d->context, g->id);

	if (info->props) {
		if ((name = spa_dict_lookup(info->props, PW_KEY_MEDIA_NAME)) == NULL &&
		    (name = spa_dict_lookup(info->props, PW_KEY_APP_NAME)) == NULL &&
		    (name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME)) == NULL)
			name = NULL;
	}
	if (name == NULL)
		name = "unknown";

	cl = pa_context_find_global(d->context, g->node_info.client_id);

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
		l = pa_context_find_linked(d->context, g->id);
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
		i.sample_spec.format = PA_SAMPLE_S16LE;
		i.sample_spec.rate = 44100;
		i.sample_spec.channels = g->node_info.n_channel_volumes;
		if (i.sample_spec.channels == 0)
			i.sample_spec.channels = 2;
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

	pw_log_debug("context %p: sink info for %d sink:%d", g->context, i.index, i.sink);

	d->cb(d->context, &i, 0, d->userdata);

	pa_proplist_free(i.proplist);
}

static void sink_input_info(pa_operation *o, void *userdata)
{
	struct sink_input_data *d = userdata;
	int eol = 1;

	if (d->global) {
		if (wait_global(d->context, d->global, o) < 0)
			return;
		sink_input_callback(d);
	} else {
		pa_context_set_error(d->context, PA_ERR_INVALID);
		eol = -1;
	}
	d->cb(d->context, NULL, eol, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
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

	if ((g = pa_context_find_global(c, idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_SINK_INPUT))
		g = NULL;

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

	if (wait_globals(c, PA_SUBSCRIPTION_MASK_SINK_INPUT, o) < 0)
		return;
	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SINK_INPUT))
			continue;
		d->global = g;
		sink_input_callback(d);
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
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_move_sink_input_by_name(pa_context *c, uint32_t idx, const char *sink_name, pa_context_success_cb_t cb, void* userdata)
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
pa_operation* pa_context_move_sink_input_by_index(pa_context *c, uint32_t idx, uint32_t sink_idx, pa_context_success_cb_t cb, void* userdata)
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
pa_operation* pa_context_set_sink_input_volume(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pa_stream *s;
	struct global *g;
	pa_operation *o;
	struct success_ack *d;
	int error = 0;

	pw_log_debug("contex %p: index %d", c, idx);

	if ((s = find_stream(c, idx)) == NULL) {
		if ((g = pa_context_find_global(c, idx)) == NULL ||
		    !(g->mask & PA_SUBSCRIPTION_MASK_SINK_INPUT))
			g = NULL;
	}
	if (s) {
		set_stream_volume(c, s, volume, s->mute);
	} else if (g) {
		set_node_volume(c, g, volume, g->node_info.mute);
	} else {
		error = PA_ERR_INVALID;
	}
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = error;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_sink_input_mute(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pa_stream *s;
	struct global *g;
	pa_operation *o;
	struct success_ack *d;
	int error = 0;

	pw_log_debug("contex %p: index %d", c, idx);

	if ((s = find_stream(c, idx)) == NULL) {
		if ((g = pa_context_find_global(c, idx)) == NULL ||
		    !(g->mask & PA_SUBSCRIPTION_MASK_SINK_INPUT))
			g = NULL;
	}

	if (s) {
		set_stream_volume(c, s, NULL, mute);
	} else if (g) {
		set_node_volume(c, g, NULL, mute);
	} else {
		error = PA_ERR_INVALID;
	}
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = error;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_kill_sink_input(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	pa_stream *s;
	struct global *g;
	pa_operation *o;
	struct success_ack *d;
	int error = 0;

	if ((s = find_stream(c, idx)) == NULL) {
		if ((g = pa_context_find_global(c, idx)) == NULL ||
		    !(g->mask & PA_SUBSCRIPTION_MASK_SINK_INPUT))
			g = NULL;
	}

	if (s) {
		pw_stream_destroy(s->stream);
	} else if (g) {
		pw_registry_destroy(c->registry, g->id);
	} else {
		error = PA_ERR_INVALID;
	}
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = error;
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
	const char *name = NULL;
	uint32_t n;
	pa_source_output_info i;
	pa_format_info ii[1];
	pa_stream *s;

	pw_log_debug("index %d", g->id);
	if (info == NULL)
		return;

	s = find_stream(d->context, g->id);

	if (info->props) {
		if ((name = spa_dict_lookup(info->props, PW_KEY_MEDIA_NAME)) == NULL &&
		    (name = spa_dict_lookup(info->props, PW_KEY_APP_NAME)) == NULL &&
		    (name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME)) == NULL)
			name = NULL;
	}
	if (name == NULL)
		name = "unknown";

	cl = pa_context_find_global(d->context, g->node_info.client_id);

	spa_zero(i);
	i.index = g->id;
	i.name = name;
	i.owner_module = PA_INVALID_INDEX;
	i.client = g->node_info.client_id;
	if (s) {
		i.source = s->device_index;
	}
	else {
		l = pa_context_find_linked(d->context, g->id);
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
		i.sample_spec.format = PA_SAMPLE_S16LE;
		i.sample_spec.rate = 44100;
		i.sample_spec.channels = g->node_info.n_channel_volumes;
		if (i.sample_spec.channels == 0)
			i.sample_spec.channels = 2;
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

	d->cb(d->context, &i, 0, d->userdata);

	pa_proplist_free(i.proplist);
}

static void source_output_info(pa_operation *o, void *userdata)
{
	struct source_output_data *d = userdata;
	int eol = 1;

	if (d->global) {
		if (wait_global(d->context, d->global, o) < 0)
			return;
		source_output_callback(d);
	} else {
		pa_context_set_error(d->context, PA_ERR_INVALID);
		eol = -1;
	}
	d->cb(d->context, NULL, eol, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation* pa_context_get_source_output_info(pa_context *c, uint32_t idx, pa_source_output_info_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct global *g;
	struct source_output_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(cb);

	PA_CHECK_VALIDITY_RETURN_NULL(c, idx != PA_INVALID_INDEX, PA_ERR_INVALID);

	if ((g = pa_context_find_global(c, idx)) == NULL ||
	    !(g->mask & PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT))
		g = NULL;

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

	if (wait_globals(c, PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT, o) < 0)
		return;

	spa_list_for_each(g, &c->globals, link) {
		if (!(g->mask & PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT))
			continue;
		d->global = g;
		source_output_callback(d);
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

	o = pa_operation_new(c, NULL, source_output_info_list, sizeof(struct source_output_data));
	d = o->userdata;
	d->context = c;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_move_source_output_by_name(pa_context *c, uint32_t idx, const char *source_name, pa_context_success_cb_t cb, void* userdata)
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
pa_operation* pa_context_move_source_output_by_index(pa_context *c, uint32_t idx, uint32_t source_idx, pa_context_success_cb_t cb, void* userdata)
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
pa_operation* pa_context_set_source_output_volume(pa_context *c, uint32_t idx, const pa_cvolume *volume, pa_context_success_cb_t cb, void *userdata)
{
	pa_stream *s;
	struct global *g;
	pa_operation *o;
	struct success_ack *d;
	int error = 0;

	pw_log_debug("contex %p: index %d", c, idx);

	if ((s = find_stream(c, idx)) == NULL) {
		if ((g = pa_context_find_global(c, idx)) == NULL ||
		    !(g->mask & PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT))
			g = NULL;
	}

	if (s) {
		set_stream_volume(c, s, volume, s->mute);
	} else if (g) {
		set_node_volume(c, g, volume, g->node_info.mute);
	} else {
		error = PA_ERR_INVALID;
	}
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = error;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_set_source_output_mute(pa_context *c, uint32_t idx, int mute, pa_context_success_cb_t cb, void *userdata)
{
	pa_stream *s;
	struct global *g;
	pa_operation *o;
	struct success_ack *d;
	int error = 0;

	if ((s = find_stream(c, idx)) == NULL) {
		if ((g = pa_context_find_global(c, idx)) == NULL ||
		    !(g->mask & PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT))
			g = NULL;
	}
	if (s) {
		set_stream_volume(c, s, NULL, mute);
	} else if (g) {
		set_node_volume(c, g, NULL, mute);
	} else {
		error = PA_ERR_INVALID;
	}
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = error;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation* pa_context_kill_source_output(pa_context *c, uint32_t idx, pa_context_success_cb_t cb, void *userdata)
{
	pa_stream *s;
	struct global *g;
	pa_operation *o;
	struct success_ack *d;
	int error = 0;

	if ((s = find_stream(c, idx)) == NULL) {
		if ((g = pa_context_find_global(c, idx)) == NULL ||
		    !(g->mask & PA_SUBSCRIPTION_MASK_SOURCE_OUTPUT))
			g = NULL;
	}

	if (s) {
		pw_stream_destroy(s->stream);
	} else if (g) {
		pw_registry_destroy(c->registry, g->id);
	} else {
		error = PA_ERR_INVALID;
	}
	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_ack));
	d = o->userdata;
	d->cb = cb;
	d->error = error;
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
