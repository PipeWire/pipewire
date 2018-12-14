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

#include <pipewire/log.h>
#include <pipewire/properties.h>
#include <pipewire/main-loop.h>
#include <pipewire/core.h>
#include <pipewire/remote.h>

#include <pulse/context.h>
#include <pulse/timeval.h>
#include <pulse/error.h>

#include "internal.h"

int pa_context_set_error(pa_context *c, int error) {
	pa_assert(error >= 0);
	pa_assert(error < PA_ERR_MAX);
	if (c && c->error != error) {
		pw_log_debug("context %p: error %d %s", c, error, pa_strerror(error));
		c->error = error;
	}
	return error;
}

static void context_unlink(pa_context *c)
{
	pa_stream *s, *t;
	pa_operation *o;

	pw_log_debug("context %p: unlink %d", c, c->state);

	spa_list_for_each_safe(s, t, &c->streams, link) {
		pa_stream_set_state(s, c->state == PA_CONTEXT_FAILED ?
				PA_STREAM_FAILED : PA_STREAM_TERMINATED);
	}

	spa_list_consume(o, &c->operations, link)
		pa_operation_cancel(o);
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

static void context_fail(pa_context *c, int error) {
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	pw_log_debug("context %p: error %d", c, error);

	pa_context_set_error(c, error);
	pa_context_set_state(c, PA_CONTEXT_FAILED);
}

pa_context *pa_context_new(pa_mainloop_api *mainloop, const char *name)
{
	return pa_context_new_with_proplist(mainloop, name, NULL);
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

struct global *pa_context_find_global_by_name(pa_context *c, uint32_t mask, const char *name)
{
	struct global *g;
	const char *str;
	uint32_t id = atoi(name);

	spa_list_for_each(g, &c->globals, link) {
		if ((g->mask & mask) == 0)
			continue;
		if (g->props != NULL &&
		    (str = pw_properties_get(g->props, "node.name")) != NULL &&
		    strcmp(str, name) == 0)
			return g;
		if (g->id == id)
			return g;
	}
	return NULL;
}

struct global *pa_context_find_linked(pa_context *c, uint32_t idx)
{
	struct global *g, *f;

	spa_list_for_each(g, &c->globals, link) {
		if (g->type != PW_TYPE_INTERFACE_Link)
			continue;

		pw_log_debug("context %p: %p %d %d %d", c, g, idx,
				g->link_info.src->parent_id,
				g->link_info.dst->parent_id);

		if (g->link_info.src->parent_id == idx)
			f = pa_context_find_global(c, g->link_info.dst->parent_id);
		else if (g->link_info.dst->parent_id == idx)
			f = pa_context_find_global(c, g->link_info.src->parent_id);
		else
			continue;

		if (f == NULL)
			continue;
		if (f->mask & PA_SUBSCRIPTION_MASK_DSP) {
			if (!(f->mask & PA_SUBSCRIPTION_MASK_SOURCE) ||
			    g->link_info.dst->parent_id != idx)
				f = pa_context_find_global(c, f->dsp_info.session);
		}
		return f;
	}
	return NULL;
}

static void emit_event(pa_context *c, struct global *g, pa_subscription_event_type_t event)
{
	if (c->subscribe_mask & g->mask) {
		if (c->subscribe_callback) {
			pw_log_debug("context %p: obj %d: emit %d:%d", c, g->id, event, g->event);
			c->subscribe_callback(c,
					event | g->event,
					g->id,
					c->subscribe_userdata);
		}
	}
}

static int set_mask(pa_context *c, struct global *g)
{
	const char *str;
	struct global *f;

	switch (g->type) {
	case PW_TYPE_INTERFACE_Device:
		if (g->props == NULL)
			return 0;
		if ((str = pw_properties_get(g->props, "media.class")) == NULL)
			return 0;
		if (strcmp(str, "Audio/Device") != 0)
			return 0;

		pw_log_debug("found card %d", g->id);
		g->mask = PA_SUBSCRIPTION_MASK_CARD;
		g->event = PA_SUBSCRIPTION_EVENT_CARD;
		break;

	case PW_TYPE_INTERFACE_Node:
		if (g->props == NULL)
			return 0;
		if ((str = pw_properties_get(g->props, "media.class")) == NULL)
			return 0;

		if (strcmp(str, "Audio/Sink") == 0) {
			pw_log_debug("found sink %d", g->id);
			g->mask = PA_SUBSCRIPTION_MASK_SINK;
			g->event = PA_SUBSCRIPTION_EVENT_SINK;
			g->node_info.monitor = SPA_ID_INVALID;
		}
		else if (strcmp(str, "Audio/DSP/Playback") == 0) {
			if ((str = pw_properties_get(g->props, "node.session")) == NULL)
				return 0;
			pw_log_debug("found monitor %d", g->id);
			g->mask = PA_SUBSCRIPTION_MASK_DSP_SINK | PA_SUBSCRIPTION_MASK_SOURCE;
			g->event = PA_SUBSCRIPTION_EVENT_SOURCE;
			g->dsp_info.session = pw_properties_parse_int(str);
			if ((f = pa_context_find_global(c, g->dsp_info.session)) != NULL)
				f->node_info.monitor = g->id;
		}
		else if (strcmp(str, "Audio/Source") == 0) {
			pw_log_debug("found source %d", g->id);
			g->mask = PA_SUBSCRIPTION_MASK_SOURCE;
			g->event = PA_SUBSCRIPTION_EVENT_SOURCE;
		}
		else if (strcmp(str, "Audio/DSP/Capture") == 0) {
			if ((str = pw_properties_get(g->props, "node.session")) == NULL)
				return 0;
			g->mask = PA_SUBSCRIPTION_MASK_DSP_SOURCE;
			g->dsp_info.session = pw_properties_parse_int(str);
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
		break;

	case PW_TYPE_INTERFACE_Module:
		pw_log_debug("found module %d", g->id);
		g->mask = PA_SUBSCRIPTION_MASK_MODULE;
		g->event = PA_SUBSCRIPTION_EVENT_MODULE;
		break;

	case PW_TYPE_INTERFACE_Client:
		pw_log_debug("found client %d", g->id);
		g->mask = PA_SUBSCRIPTION_MASK_CLIENT;
		g->event = PA_SUBSCRIPTION_EVENT_CLIENT;
		break;

	case PW_TYPE_INTERFACE_Port:
		pw_log_debug("found port %d", g->id);
		break;

	case PW_TYPE_INTERFACE_Link:
                if ((str = pw_properties_get(g->props, "link.output")) == NULL)
			return 0;
		g->link_info.src = pa_context_find_global(c, pw_properties_parse_int(str));
                if ((str = pw_properties_get(g->props, "link.input")) == NULL)
			return 0;
		g->link_info.dst = pa_context_find_global(c, pw_properties_parse_int(str));

		if (g->link_info.src == NULL || g->link_info.dst == NULL)
			return 0;

		pw_log_debug("link %d:%d->%d:%d",
				g->link_info.src->parent_id,
				g->link_info.src->id,
				g->link_info.dst->parent_id,
				g->link_info.dst->id);

		if ((f = pa_context_find_global(c, g->link_info.src->parent_id)) != NULL)
			emit_event(c, f, PA_SUBSCRIPTION_EVENT_CHANGE);
		if ((f = pa_context_find_global(c, g->link_info.dst->parent_id)) != NULL)
			emit_event(c, f, PA_SUBSCRIPTION_EVENT_CHANGE);

		break;

	default:
		return 0;
	}
	return 1;
}

static void registry_event_global(void *data, uint32_t id, uint32_t parent_id,
                                  uint32_t permissions, uint32_t type, uint32_t version,
                                  const struct spa_dict *props)
{
	pa_context *c = data;
	struct global *g;

	g = calloc(1, sizeof(struct global));
	pw_log_debug("context %p: global %d %p", c, id, g);
	g->id = id;
	g->parent_id = parent_id;
	g->type = type;
	g->props = props ? pw_properties_new_dict(props) : NULL;

	if (set_mask(c, g) == 0)
		return;

	pw_log_debug("mask %d/%d", g->mask, g->event);
	spa_list_append(&c->globals, &g->link);

	emit_event(c, g, PA_SUBSCRIPTION_EVENT_NEW);
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
	spa_list_remove(&g->link);
	if (g->props)
		pw_properties_free(g->props);
	if (g->destroy)
		g->destroy(g);
	free(g);
}

static const struct pw_registry_proxy_events registry_events =
{
	PW_VERSION_REGISTRY_PROXY_EVENTS,
	.global = registry_event_global,
	.global_remove = registry_event_global_remove,
};

struct ready_data
{
	pa_context *context;
};

static void on_ready(pa_operation *o, void *userdata)
{
	struct ready_data *d = userdata;
	pa_context_set_state(d->context, PA_CONTEXT_READY);
}

static void remote_state_changed(void *data, enum pw_remote_state old,
				 enum pw_remote_state state, const char *error)
{
	pa_context *c = data;
	pa_operation *o;
	struct ready_data *d;

	switch(state) {
	case PW_REMOTE_STATE_ERROR:
		context_fail(c, PA_ERR_CONNECTIONTERMINATED);
		break;
	case PW_REMOTE_STATE_UNCONNECTED:
		break;
	case PW_REMOTE_STATE_CONNECTING:
		pa_context_set_state(c, PA_CONTEXT_CONNECTING);
		break;
	case PW_REMOTE_STATE_CONNECTED:
		pa_context_set_state(c, PA_CONTEXT_AUTHORIZING);
		pa_context_set_state(c, PA_CONTEXT_SETTING_NAME);

		c->core_proxy = pw_remote_get_core_proxy(c->remote);
                c->registry_proxy = pw_core_proxy_get_registry(c->core_proxy,
							       PW_TYPE_INTERFACE_Registry,
                                                               PW_VERSION_REGISTRY, 0);
                pw_registry_proxy_add_listener(c->registry_proxy,
                                               &c->registry_listener,
                                               &registry_events, c);

		o = pa_operation_new(c, NULL, on_ready, sizeof(struct ready_data));
		d = o->userdata;
		d->context = c;
		pa_operation_sync(o);
		pa_operation_unref(o);
		break;
	}
}

static void complete_operations(pa_context *c, uint32_t seq)
{
	pa_operation *o, *t;
	spa_list_for_each_safe(o, t, &c->operations, link) {
		if (o->seq != seq)
			continue;
		pa_operation_ref(o);
		if (o->callback)
			o->callback(o, o->userdata);
		pa_operation_unref(o);
	}
}

static void remote_sync_reply(void *data, uint32_t seq)
{
	pa_context *c = data;
	pw_log_debug("done %d", seq);
	complete_operations(c, seq);
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = remote_state_changed,
	.sync_reply = remote_sync_reply
};

pa_context *pa_context_new_with_proplist(pa_mainloop_api *mainloop, const char *name, pa_proplist *p)
{
	struct pw_core *core;
	struct pw_loop *loop;
	struct pw_remote *r;
	struct pw_properties *props;
	pa_context *c;

	pa_assert(mainloop);

	props = pw_properties_new(NULL, NULL);
	if (name)
		pw_properties_set(props, PA_PROP_APPLICATION_NAME, name);
	pw_properties_set(props, "client.api", "pulseaudio");
	if (p)
		pw_properties_update(props, &p->props->dict);

	loop = mainloop->userdata;
	core = pw_core_new(loop, NULL);

	r = pw_remote_new(core, props, sizeof(struct pa_context));
	if (r == NULL)
		return NULL;

	c = pw_remote_get_user_data(r);
	c->loop = loop;
	c->core = core;
	c->remote = r;

	pw_remote_add_listener(r, &c->remote_listener, &remote_events, c);

	c->proplist = p ? pa_proplist_copy(p) : pa_proplist_new();
	c->refcount = 1;
	c->client_index = PA_INVALID_INDEX;

	if (name)
		pa_proplist_sets(c->proplist, PA_PROP_APPLICATION_NAME, name);

	c->mainloop = mainloop;
	c->error = 0;
	c->state = PA_CONTEXT_UNCONNECTED;

	spa_list_init(&c->globals);

	spa_list_init(&c->streams);
	spa_list_init(&c->operations);

	return c;
}

static void context_free(pa_context *c)
{
	pw_log_debug("context %p: free", c);

	context_unlink(c);

	if (c->proplist)
		pa_proplist_free(c->proplist);
}

void pa_context_unref(pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	if (--c->refcount == 0)
		context_free(c);
}

pa_context* pa_context_ref(pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);
	c->refcount++;
	return c;
}

void pa_context_set_state_callback(pa_context *c, pa_context_notify_cb_t cb, void *userdata)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	if (c->state == PA_CONTEXT_TERMINATED || c->state == PA_CONTEXT_FAILED)
		return;

	c->state_callback = cb;
	c->state_userdata = userdata;
}

void pa_context_set_event_callback(pa_context *c, pa_context_event_cb_t cb, void *userdata)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	if (c->state == PA_CONTEXT_TERMINATED || c->state == PA_CONTEXT_FAILED)
		return;

	c->event_callback = cb;
	c->event_userdata = userdata;
}

int pa_context_errno(pa_context *c)
{
	if (!c)
		return PA_ERR_INVALID;

	pa_assert(c->refcount >= 1);

	return c->error;
}

int pa_context_is_pending(pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY(c, PA_CONTEXT_IS_GOOD(c->state), PA_ERR_BADSTATE);

	return !spa_list_is_empty(&c->operations);
}

pa_context_state_t pa_context_get_state(pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);
	return c->state;
}

int pa_context_connect(pa_context *c, const char *server, pa_context_flags_t flags, const pa_spawn_api *api)
{
	int res;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY(c, c->state == PA_CONTEXT_UNCONNECTED, PA_ERR_BADSTATE);
	PA_CHECK_VALIDITY(c, !(flags & ~(PA_CONTEXT_NOAUTOSPAWN|PA_CONTEXT_NOFAIL)), PA_ERR_INVALID);
	PA_CHECK_VALIDITY(c, !server || *server, PA_ERR_INVALID);

	pa_context_ref(c);

	c->no_fail = !!(flags & PA_CONTEXT_NOFAIL);

	res = pw_remote_connect(c->remote);

	pa_context_unref(c);

	return res;
}

void pa_context_disconnect(pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	pw_remote_disconnect(c->remote);

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
	pa_operation_done(o);
	if (d->cb)
		d->cb(c, d->userdata);
}

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

struct success_data {
	pa_context_success_cb_t cb;
	void *userdata;
	int ret;
};

static void on_success(pa_operation *o, void *userdata)
{
	struct success_data *d = userdata;
	pa_context *c = o->context;
	pa_operation_done(o);
	if (d->cb)
		d->cb(c, d->ret, d->userdata);
}

pa_operation* pa_context_exit_daemon(pa_context *c, pa_context_success_cb_t cb, void *userdata)
{
	pa_operation *o;
	struct success_data *d;

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_data));
	d = o->userdata;
	d->ret = PA_ERR_ACCESS;
	d->cb = cb;
	d->userdata = userdata;

	return o;
}

pa_operation* pa_context_set_default_sink(pa_context *c, const char *name, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation* pa_context_set_default_source(pa_context *c, const char *name, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

int pa_context_is_local(pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(c, PA_CONTEXT_IS_GOOD(c->state), PA_ERR_BADSTATE, -1);

	return 1;
}

pa_operation* pa_context_set_name(pa_context *c, const char *name, pa_context_success_cb_t cb, void *userdata)
{
	struct spa_dict dict;
	struct spa_dict_item items[1];
	pa_operation *o;
	struct success_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);
	pa_assert(name);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	items[0] = SPA_DICT_ITEM_INIT(PA_PROP_APPLICATION_NAME, name);
	dict = SPA_DICT_INIT(items, 1);
	pw_remote_update_properties(c->remote, &dict);

	o = pa_operation_new(c, NULL, on_success, sizeof(struct success_data));
	d = o->userdata;
	d->ret = PA_ERR_ACCESS;
	d->cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

const char* pa_context_get_server(pa_context *c)
{
	const struct pw_core_info *info;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	info = pw_remote_get_core_info(c->remote);
	PA_CHECK_VALIDITY_RETURN_NULL(c, info && info->name, PA_ERR_NOENTITY);

	return info->name;
}

uint32_t pa_context_get_protocol_version(pa_context *c)
{
	return PA_PROTOCOL_VERSION;
}

uint32_t pa_context_get_server_protocol_version(pa_context *c)
{
	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_ANY(c, PA_CONTEXT_IS_GOOD(c->state), PA_ERR_BADSTATE, PA_INVALID_INDEX);

	return PA_PROTOCOL_VERSION;
}

pa_operation *pa_context_proplist_update(pa_context *c, pa_update_mode_t mode, pa_proplist *p, pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

pa_operation *pa_context_proplist_remove(pa_context *c, const char *const keys[], pa_context_success_cb_t cb, void *userdata)
{
	pw_log_warn("Not Implemented");
	return NULL;
}

uint32_t pa_context_get_index(pa_context *c)
{
	return c->client_index;
}

pa_time_event* pa_context_rttime_new(pa_context *c, pa_usec_t usec, pa_time_event_cb_t cb, void *userdata)
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

void pa_context_rttime_restart(pa_context *c, pa_time_event *e, pa_usec_t usec)
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

size_t pa_context_get_tile_size(pa_context *c, const pa_sample_spec *ss)
{
	pw_log_warn("Not Implemented");
	return 1024;
}

int pa_context_load_cookie_from_file(pa_context *c, const char *cookie_file_path)
{
	pw_log_warn("Not Implemented");
	return -ENOTSUP;
}
