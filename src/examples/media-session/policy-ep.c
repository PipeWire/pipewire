/* PipeWire
 *
 * Copyright © 2019 Wim Taymans
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
#include <math.h>
#include <time.h>

#include "config.h"

#include <spa/node/node.h>
#include <spa/utils/hook.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/pod.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"
#include "extensions/session-manager.h"

#define NAME "policy-ep"

#define DEFAULT_CHANNELS	2
#define DEFAULT_SAMPLERATE	48000

#define DEFAULT_IDLE_SECONDS	3

struct impl;

struct impl {
	struct timespec now;

	struct pw_core *core;
	struct pw_remote *remote;
	struct spa_hook remote_listener;

	struct pw_core_proxy *core_proxy;
	struct spa_hook core_listener;

	struct pw_registry_proxy *registry_proxy;
	struct spa_hook registry_listener;

	struct pw_map globals;

	struct spa_list client_list;
	struct spa_list endpoint_list;
	struct spa_list session_list;
	int seq;
};

struct object {
	struct impl *impl;
	uint32_t id;
	uint32_t type;
	struct pw_proxy *proxy;
	struct spa_hook listener;
};

struct client {
	struct object obj;

	struct spa_list l;

	struct spa_hook listener;
	struct pw_client_info *info;
};

struct session {
	struct object obj;

	struct spa_list l;

	struct spa_hook listener;
	struct pw_session_info *info;
};

struct endpoint {
	struct object obj;

	struct spa_list l;

	struct spa_hook listener;
	struct pw_endpoint_info *info;

	struct endpoint *peer;
	struct session *session;

	uint32_t client_id;
	int32_t priority;

	struct spa_list stream_list;

	enum pw_direction direction;
#define ENDPOINT_TYPE_UNKNOWN	0
#define ENDPOINT_TYPE_STREAM	1
#define ENDPOINT_TYPE_DEVICE	2
	uint32_t type;
	char *media;

	uint32_t media_type;
	uint32_t media_subtype;
	struct spa_audio_info_raw format;

	uint64_t plugged;
	unsigned int exclusive:1;
	unsigned int enabled:1;
	unsigned int busy:1;
};

struct stream {
	struct object obj;

	struct spa_list l;
	enum pw_direction direction;
	struct pw_endpoint_stream_info *info;
	struct endpoint *endpoint;
#define STREAM_FLAG_NONE	0
#define STREAM_FLAG_DSP		(1<<0)
#define STREAM_FLAG_SKIP	(1<<1)
	uint32_t flags;

	struct spa_hook listener;
};

struct link {
	struct object obj;
	struct stream *out;
	struct stream *in;
};

static void add_object(struct impl *impl, struct object *obj)
{
	size_t size = pw_map_get_size(&impl->globals);
        while (obj->id > size)
                pw_map_insert_at(&impl->globals, size++, NULL);
        pw_map_insert_at(&impl->globals, obj->id, obj);
}

static void remove_object(struct impl *impl, struct object *obj)
{
        pw_map_insert_at(&impl->globals, obj->id, NULL);
}

static void *find_object(struct impl *impl, uint32_t id)
{
	void *obj;
	if ((obj = pw_map_lookup(&impl->globals, id)) != NULL)
		return obj;
	return NULL;
}

static void schedule_rescan(struct impl *impl)
{
	if (impl->core_proxy)
		impl->seq = pw_core_proxy_sync(impl->core_proxy, 0, impl->seq);
}

static void endpoint_event_info(void *object, const struct pw_endpoint_info *update)
{
	struct endpoint *e = object;
	struct impl *impl = e->obj.impl;
	struct pw_endpoint_info *info = e->info;
	const char *str;

	pw_log_debug(NAME" %p: info for endpoint %d type %d", impl, e->obj.id, e->type);

	if (info == NULL && update) {
		info = e->info = calloc(1, sizeof(*info));
		info->id = update->id;
		info->name = update->name ? strdup(update->name) : NULL;
		info->media_class = update->media_class ? strdup(update->media_class) : NULL;
		info->direction = update->direction;
		info->flags = update->flags;
	}
	info->change_mask = update->change_mask;
	if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_SESSION) {
		info->session_id = update->session_id;
		e->session = find_object(impl, info->session_id);
	}
	if (update->change_mask & PW_ENDPOINT_CHANGE_MASK_PROPS) {
		if (info->props)
			pw_properties_free ((struct pw_properties *)info->props);
		info->props = (struct spa_dict *) pw_properties_new_dict (update->props);
		if ((str = spa_dict_lookup(info->props, PW_KEY_PRIORITY_SESSION)) != NULL)
			e->priority = pw_properties_parse_int(str);
	}
	e->enabled = true;
}

static void endpoint_event_param(void *object, int seq,
                       uint32_t id, uint32_t index, uint32_t next,
                       const struct spa_pod *param)
{
	struct endpoint *e = object;
	struct impl *impl = e->obj.impl;
	pw_log_debug(NAME" %p: param for endpoint %d, %d", impl, e->obj.id, id);
}

static const struct pw_endpoint_proxy_events endpoint_events = {
	PW_VERSION_ENDPOINT_PROXY_EVENTS,
	.info = endpoint_event_info,
	.param = endpoint_event_param,
};

static void endpoint_proxy_destroy(void *data)
{
	struct endpoint *e = data;
	struct impl *impl = e->obj.impl;
	struct stream *s, *t;

	pw_log_debug(NAME " %p: proxy destroy endpoint %d", impl, e->obj.id);

	spa_list_remove(&e->l);

	spa_list_for_each_safe(s, t, &e->stream_list, l) {
		spa_list_remove(&s->l);
		s->endpoint = NULL;
	}
	free(e->media);
}

static const struct pw_proxy_events endpoint_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = endpoint_proxy_destroy,
};

static int
handle_endpoint(struct impl *impl, uint32_t id,
		uint32_t type, const struct spa_dict *props)
{
	const char *str, *media_class;
	enum pw_direction direction;
	struct pw_proxy *p;
	struct endpoint *ep;
	uint32_t client_id = SPA_ID_INVALID;

	if (props) {
		if ((str = spa_dict_lookup(props, PW_KEY_CLIENT_ID)) != NULL)
			client_id = atoi(str);
	}

	p = pw_registry_proxy_bind(impl->registry_proxy,
			id, type, PW_VERSION_ENDPOINT_PROXY,
			sizeof(struct endpoint));

	ep = pw_proxy_get_user_data(p);
	ep->obj.impl = impl;
	ep->obj.id = id;
	ep->obj.type = type;
	ep->obj.proxy = p;
	ep->client_id = client_id;
	spa_list_init(&ep->stream_list);
	pw_proxy_add_listener(p, &ep->obj.listener, &endpoint_proxy_events, ep);
	pw_proxy_add_object_listener(p, &ep->listener, &endpoint_events, ep);
	add_object(impl, &ep->obj);
	spa_list_append(&impl->endpoint_list, &ep->l);
	ep->type = ENDPOINT_TYPE_UNKNOWN;

	media_class = props ? spa_dict_lookup(props, PW_KEY_MEDIA_CLASS) : NULL;

	pw_log_debug(NAME" %p: endpoint "PW_KEY_MEDIA_CLASS" %s", impl, media_class);

	if (media_class == NULL)
		return 0;

	if (strstr(media_class, "Stream/") == media_class) {
		media_class += strlen("Stream/");

		if (strstr(media_class, "Output/") == media_class) {
			direction = PW_DIRECTION_OUTPUT;
			media_class += strlen("Output/");
		}
		else if (strstr(media_class, "Input/") == media_class) {
			direction = PW_DIRECTION_INPUT;
			media_class += strlen("Input/");
		}
		else
			return 0;

		ep->direction = direction;
		ep->type = ENDPOINT_TYPE_STREAM;
		ep->media = strdup(media_class);
		pw_log_debug(NAME "%p: endpoint %d is stream %s", impl, id, ep->media);
	}
	else {
		if (strstr(media_class, "Audio/") == media_class) {
			media_class += strlen("Audio/");
		}
		else if (strstr(media_class, "Video/") == media_class) {
			media_class += strlen("Video/");
		}
		else
			return 0;

		if (strcmp(media_class, "Sink") == 0)
			direction = PW_DIRECTION_OUTPUT;
		else if (strcmp(media_class, "Source") == 0)
			direction = PW_DIRECTION_INPUT;
		else
			return 0;

		ep->direction = direction;
		ep->type = ENDPOINT_TYPE_DEVICE;

		pw_log_debug(NAME" %p: endpoint %d prio:%d", impl, id, ep->priority);
	}
	return 1;
}

static void stream_event_info(void *object, const struct pw_endpoint_stream_info *info)
{
	struct stream *s = object;
	pw_log_debug(NAME" %p: info for stream %d", s->obj.impl, s->obj.id);
}

static void stream_event_param(void *object, int seq,
                       uint32_t id, uint32_t index, uint32_t next,
                       const struct spa_pod *param)
{
	struct stream *s = object;
	struct endpoint *ep = s->endpoint;
	struct spa_audio_info_raw info = { 0, };

	pw_log_debug(NAME" %p: param for stream %d", s->obj.impl, s->obj.id);

	if (ep == NULL)
		return;

	if (id != SPA_PARAM_EnumFormat)
		return;

	if (spa_format_parse(param, &ep->media_type, &ep->media_subtype) < 0)
		return;

	if (ep->media_type != SPA_MEDIA_TYPE_audio ||
	    ep->media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;

	spa_pod_fixate((struct spa_pod*)param);

	if (spa_format_audio_raw_parse(param, &info) < 0)
		return;

	if (info.channels > ep->format.channels)
		ep->format = info;
}

static const struct pw_endpoint_stream_proxy_events stream_events = {
	PW_VERSION_ENDPOINT_STREAM_PROXY_EVENTS,
	.info = stream_event_info,
	.param = stream_event_param,
};

static void stream_proxy_destroy(void *data)
{
	struct stream *s = data;

	pw_log_debug(NAME " %p: proxy destroy stream %d", s->obj.impl, s->obj.id);

	if (s->endpoint) {
		spa_list_remove(&s->l);
		s->endpoint = NULL;
	}
}

static const struct pw_proxy_events stream_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = stream_proxy_destroy,
};

static int
handle_stream(struct impl *impl, uint32_t id, uint32_t type,
		const struct spa_dict *props)
{
	struct stream *s;
	struct pw_proxy *p;
	struct endpoint *ep;
	const char *str;
	uint32_t endpoint_id;

	if (props == NULL || (str = spa_dict_lookup(props, PW_KEY_ENDPOINT_ID)) == NULL)
		return -EINVAL;

	endpoint_id = atoi(str);

	if ((ep = find_object(impl, endpoint_id)) == NULL)
		return -ESRCH;

	p = pw_registry_proxy_bind(impl->registry_proxy,
			id, type, PW_VERSION_ENDPOINT_STREAM_PROXY,
			sizeof(struct stream));

	s = pw_proxy_get_user_data(p);
	s->obj.impl = impl;
	s->obj.id = id;
	s->obj.type = type;
	s->obj.proxy = p;
	s->endpoint = ep;
	s->direction = ep->direction;

	pw_proxy_add_listener(p, &s->obj.listener, &stream_proxy_events, s);
	pw_proxy_add_object_listener(p, &s->listener, &stream_events, s);
	add_object(impl, &s->obj);

	spa_list_append(&ep->stream_list, &s->l);

	pw_log_debug(NAME" %p: new stream %d for endpoint %d type %d %08x", impl, id, endpoint_id,
			ep->type, s->flags);

	if (ep->type == ENDPOINT_TYPE_DEVICE) {
		pw_endpoint_stream_proxy_enum_params((struct pw_endpoint_stream_proxy*)p,
				0, SPA_PARAM_EnumFormat,
				0, -1, NULL);
	}
	return 0;
}

static void client_event_info(void *object, const struct pw_client_info *info)
{
	struct client *c = object;
	uint32_t i;

	pw_log_debug(NAME" %p: info for client %d", c->obj.impl, c->obj.id);
	c->info = pw_client_info_update(c->info, info);
	for (i = 0; i < info->props->n_items; i++)
		pw_log_debug(NAME" %p:  %s = %s", c,
				info->props->items[i].key,
				info->props->items[i].value);
}

static const struct pw_client_proxy_events client_events = {
	PW_VERSION_CLIENT_PROXY_EVENTS,
	.info = client_event_info,
};

static void client_proxy_destroy(void *data)
{
	struct client *c = data;

	pw_log_debug(NAME " %p: proxy destroy client %d", c->obj.impl, c->obj.id);

	spa_list_remove(&c->l);
	if (c->info)
		pw_client_info_free(c->info);
}

static const struct pw_proxy_events client_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = client_proxy_destroy,
};

static int
handle_client(struct impl *impl, uint32_t id,
		uint32_t type, const struct spa_dict *props)
{
	struct pw_proxy *p;
	struct client *client;
	struct pw_permission perms[2];
	const char *str;

	p = pw_registry_proxy_bind(impl->registry_proxy,
			id, type, PW_VERSION_CLIENT_PROXY,
			sizeof(struct client));

	client = pw_proxy_get_user_data(p);
	client->obj.impl = impl;
	client->obj.id = id;
	client->obj.type = type;
	client->obj.proxy = p;

	pw_proxy_add_listener(p, &client->obj.listener, &client_proxy_events, client);
	pw_proxy_add_object_listener(p, &client->listener, &client_events, client);
	add_object(impl, &client->obj);
	spa_list_append(&impl->client_list, &client->l);

	if (props == NULL)
		return 0;

	str = spa_dict_lookup(props, PW_KEY_ACCESS);
	if (str == NULL)
		return 0;

	if (strcmp(str, "restricted") == 0) {
		perms[0] = PW_PERMISSION_INIT(-1, PW_PERM_RWX);
		pw_client_proxy_update_permissions((struct pw_client_proxy*)p,
				1, perms);
	}
	return 0;
}

static void session_event_info(void *object, const struct pw_session_info *info)
{
	struct session *c = object;
	pw_log_debug(NAME" %p: info for session %d", c->obj.impl, c->obj.id);
}

static const struct pw_session_proxy_events session_events = {
	PW_VERSION_SESSION_PROXY_EVENTS,
	.info = session_event_info,
};

static void session_proxy_destroy(void *data)
{
	struct session *c = data;

	pw_log_debug(NAME " %p: proxy destroy session %d", c->obj.impl, c->obj.id);

	spa_list_remove(&c->l);
}

static const struct pw_proxy_events session_proxy_events = {
	PW_VERSION_PROXY_EVENTS,
	.destroy = session_proxy_destroy,
};

static int
handle_session(struct impl *impl, uint32_t id,
		uint32_t type, const struct spa_dict *props)
{
	struct pw_proxy *p;
	struct session *session;

	p = pw_registry_proxy_bind(impl->registry_proxy,
			id, type, PW_VERSION_SESSION_PROXY,
			sizeof(struct session));

	session = pw_proxy_get_user_data(p);
	session->obj.impl = impl;
	session->obj.id = id;
	session->obj.type = type;
	session->obj.proxy = p;

	pw_proxy_add_listener(p, &session->obj.listener, &session_proxy_events, session);
	pw_proxy_add_object_listener(p, &session->listener, &session_events, session);
	add_object(impl, &session->obj);
	spa_list_append(&impl->session_list, &session->l);

	return 0;
}

static void
registry_global(void *data,uint32_t id,
		uint32_t permissions, uint32_t type, uint32_t version,
		const struct spa_dict *props)
{
	struct impl *impl = data;
	int res;

	pw_log_debug(NAME " %p: new global '%d' %d", impl, id, type);

	switch (type) {
	case PW_TYPE_INTERFACE_Client:
		res = handle_client(impl, id, type, props);
		break;

	case PW_TYPE_INTERFACE_Session:
		res = handle_session(impl, id, type, props);
		break;

	case PW_TYPE_INTERFACE_Endpoint:
		res = handle_endpoint(impl, id, type, props);
		break;

	case PW_TYPE_INTERFACE_EndpointStream:
		res = handle_stream(impl, id, type, props);
		break;

	default:
		res = 0;
		break;
	}
	if (res < 0) {
		pw_log_warn(NAME" %p: can't handle global %d", impl, id);
	}
	else
		schedule_rescan(impl);
}

static void
registry_global_remove(void *data, uint32_t id)
{
	struct impl *impl = data;
	struct object *obj;

	pw_log_debug(NAME " %p: remove global '%d'", impl, id);

	if ((obj = find_object(impl, id)) == NULL)
		return;

	remove_object(impl, obj);
	schedule_rescan(impl);
}

static const struct pw_registry_proxy_events registry_events = {
	PW_VERSION_REGISTRY_PROXY_EVENTS,
        .global = registry_global,
        .global_remove = registry_global_remove,
};


struct find_data {
	struct impl *impl;
	uint32_t path_id;
	const char *media_class;
	struct endpoint *endpoint;
	bool exclusive;
	int priority;
	uint64_t plugged;
};

static int find_endpoint(void *data, struct endpoint *endpoint)
{
	struct find_data *find = data;
	struct impl *impl = find->impl;
	const struct spa_dict *props;
	const char *str;
	int priority = 0;
	uint64_t plugged = 0;

	pw_log_debug(NAME " %p: looking at endpoint '%d' enabled:%d busy:%d exclusive:%d",
			impl, endpoint->obj.id, endpoint->enabled, endpoint->busy, endpoint->exclusive);

	if (!endpoint->enabled)
		return 0;

	if (find->path_id != SPA_ID_INVALID && endpoint->obj.id != find->path_id)
		return 0;

	if (find->path_id == SPA_ID_INVALID) {
		if (endpoint->info == NULL ||
		    (props = endpoint->info->props) == NULL)
			return 0;

		if ((str = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS)) == NULL)
			return 0;

		if (strcmp(str, find->media_class) != 0)
			return 0;

		plugged = endpoint->plugged;
		priority = endpoint->priority;
	}

	if ((find->exclusive && endpoint->busy) || endpoint->exclusive) {
		pw_log_debug(NAME " %p: endpoint '%d' in use", impl, endpoint->obj.id);
		return 0;
	}

	pw_log_debug(NAME " %p: found endpoint '%d' %"PRIu64" prio:%d", impl,
			endpoint->obj.id, plugged, priority);

	if (find->endpoint == NULL ||
	    priority > find->priority ||
	    (priority == find->priority && plugged > find->plugged)) {
		pw_log_debug(NAME " %p: new best %d %" PRIu64, impl, priority, plugged);
		find->endpoint = endpoint;
		find->priority = priority;
		find->plugged = plugged;
	}
	return 0;
}

static int link_endpoints(struct endpoint *endpoint, enum pw_direction direction, struct endpoint *peer, int max)
{
	struct impl *impl = peer->obj.impl;
	struct stream *s;

	pw_log_debug(NAME " %p: link endpoints %d %d %d", impl, max, endpoint->obj.id, peer->obj.id);

	if (endpoint->session == NULL) {
		pw_log_debug(NAME " %p: endpoint has no session", impl);
		return -EINVAL;
	}

	spa_list_for_each(s, &endpoint->stream_list, l) {
		struct pw_properties *props;

		pw_log_debug(NAME " %p: stream %p: %d %d", impl, s, s->direction, s->flags);

		if (s->direction == direction)
			continue;
		if (s->flags & STREAM_FLAG_SKIP)
			continue;

		if (max-- == 0)
			return 0;

		props = pw_properties_new(NULL, NULL);
		if (s->direction == PW_DIRECTION_OUTPUT) {
			pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%d", endpoint->obj.id);
			pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%d", s->obj.id);
			pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%d", peer->obj.id);
			pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%d", -1);
			pw_log_debug(NAME " %p: stream %d:%d -> endpoint %d", impl,
					endpoint->obj.id, s->obj.id, peer->obj.id);

		}
		else {
			pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%d", peer->obj.id);
			pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%d", -1);
			pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%d", endpoint->obj.id);
			pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%d", s->obj.id);
			pw_log_debug(NAME " %p: endpoint %d -> stream %d:%d", impl,
					peer->obj.id, endpoint->obj.id, s->obj.id);
		}

		pw_endpoint_proxy_create_link((struct pw_endpoint_proxy*)endpoint->obj.proxy,
                                          &props->dict);

		pw_properties_free(props);
	}
	endpoint->peer = peer;
	peer->peer = endpoint;

	return 0;
}

static int rescan_endpoint(struct impl *impl, struct endpoint *ep)
{
	struct spa_dict *props;
        const char *str, *media, *category, *role;
        bool exclusive;
        struct find_data find;
	struct pw_endpoint_info *info;
	struct endpoint *peer;
	enum pw_direction direction;

	if (ep->type == ENDPOINT_TYPE_DEVICE)
		return 0;

	if (ep->info == NULL || ep->info->props == NULL) {
		pw_log_debug(NAME " %p: endpoint %d has no properties", impl, ep->obj.id);
		return 0;
	}

	if (ep->peer != NULL)
		return 0;

	info = ep->info;
	props = info->props;

        str = spa_dict_lookup(props, PW_KEY_ENDPOINT_AUTOCONNECT);
        if (str == NULL || !pw_properties_parse_bool(str)) {
		pw_log_debug(NAME" %p: endpoint %d does not need autoconnect", impl, ep->obj.id);
                return 0;
	}

	if ((media = spa_dict_lookup(props, PW_KEY_MEDIA_TYPE)) == NULL)
		media = ep->media;
	if (media == NULL) {
		pw_log_debug(NAME" %p: endpoint %d has unknown media", impl, ep->obj.id);
		return 0;
	}

	spa_zero(find);

	if ((category = spa_dict_lookup(props, PW_KEY_MEDIA_CATEGORY)) == NULL) {
		pw_log_debug(NAME" %p: endpoint %d find category",
			impl, ep->obj.id);
		if (ep->direction == PW_DIRECTION_INPUT) {
			category = "Capture";
		} else if (ep->direction == PW_DIRECTION_OUTPUT) {
			category = "Playback";
		} else {
			pw_log_warn(NAME" %p: endpoint %d can't determine category",
					impl, ep->obj.id);
			return -EINVAL;
		}
	}

	if ((role = spa_dict_lookup(props, PW_KEY_MEDIA_ROLE)) == NULL) {
		if (strcmp(media, "Audio") == 0) {
			if (strcmp(category, "Duplex") == 0)
				role = "Communication";
			else if (strcmp(category, "Capture") == 0)
				role = "Production";
			else
				role = "Music";
		}
		else if (strcmp(media, "Video") == 0) {
			if (strcmp(category, "Duplex") == 0)
				role = "Communication";
			else if (strcmp(category, "Capture") == 0)
				role = "Camera";
			else
				role = "Video";
		}
	}

	if ((str = spa_dict_lookup(props, PW_KEY_NODE_EXCLUSIVE)) != NULL)
		exclusive = pw_properties_parse_bool(str);
	else
		exclusive = false;

	if (strcmp(media, "Audio") == 0) {
		if (strcmp(category, "Playback") == 0)
			find.media_class = "Audio/Sink";
		else if (strcmp(category, "Capture") == 0)
			find.media_class = "Audio/Source";
		else {
			pw_log_debug(NAME" %p: endpoint %d unhandled category %s",
					impl, ep->obj.id, category);
			return -EINVAL;
		}
	}
	else if (strcmp(media, "Video") == 0) {
		if (strcmp(category, "Capture") == 0)
			find.media_class = "Video/Source";
		else {
			pw_log_debug(NAME" %p: endpoint %d unhandled category %s",
					impl, ep->obj.id, category);
			return -EINVAL;
		}
	}
	else {
		pw_log_debug(NAME" %p: endpoint %d unhandled media %s",
				impl, ep->obj.id, media);
		return -EINVAL;
	}

	if (strcmp(category, "Capture") == 0)
		direction = PW_DIRECTION_OUTPUT;
	else if (strcmp(category, "Playback") == 0)
		direction = PW_DIRECTION_INPUT;
	else {
		pw_log_debug(NAME" %p: endpoint %d unhandled category %s",
				impl, ep->obj.id, category);
		return -EINVAL;
	}

	str = spa_dict_lookup(props, PW_KEY_NODE_TARGET);
	if (str != NULL)
		find.path_id = atoi(str);
	else
		find.path_id = SPA_ID_INVALID;

	pw_log_info(NAME " %p: '%s' '%s' '%s' exclusive:%d target %d", impl,
			media, category, role, exclusive, find.path_id);

	find.impl = impl;
	find.exclusive = exclusive;

	spa_list_for_each(peer, &impl->endpoint_list, l)
		find_endpoint(&find, peer);

	if (find.endpoint == NULL && find.path_id != SPA_ID_INVALID) {
		pw_log_debug(NAME " %p: no endpoint found for %d, try endpoint", impl, ep->obj.id);

		if ((peer = find_object(impl, find.path_id)) != NULL) {
			if (peer->obj.type == PW_TYPE_INTERFACE_Endpoint)
				goto do_link;
		}
		else {
			str = spa_dict_lookup(props, PW_KEY_NODE_DONT_RECONNECT);
			if (str != NULL && pw_properties_parse_bool(str)) {
				pw_registry_proxy_destroy(impl->registry_proxy, ep->obj.id);
				return -ENOENT;
			}
		}
	}

	if (find.endpoint == NULL) {
		struct client *client;

		pw_log_warn(NAME " %p: no endpoint found for %d", impl, ep->obj.id);

		client = find_object(impl, ep->client_id);
		if (client && client->obj.type == PW_TYPE_INTERFACE_Client) {
			pw_client_proxy_error((struct pw_client_proxy*)client->obj.proxy,
				ep->obj.id, -ENOENT, "no endpoint available");
		}
		return -ENOENT;
	}
	peer = find.endpoint;

	if (exclusive && peer->busy) {
		pw_log_warn(NAME" %p: endpoint %d busy, can't get exclusive access", impl, peer->obj.id);
		return -EBUSY;
	}
	peer->exclusive = exclusive;

	pw_log_debug(NAME" %p: linking to endpoint '%d'", impl, peer->obj.id);

        peer->busy = true;

do_link:
	link_endpoints(ep, direction, peer, 1);

        return 1;
}

static void do_rescan(struct impl *impl)
{
	struct endpoint *ep;

	clock_gettime(CLOCK_MONOTONIC, &impl->now);
	pw_log_debug("media-session %p: do rescan", impl);

	spa_list_for_each(ep, &impl->endpoint_list, l)
		rescan_endpoint(impl, ep);
}

static void core_done(void *data, uint32_t id, int seq)
{
	struct impl *impl = data;
	pw_log_debug("media-session %p: sync %u %d/%d", impl, id, seq, impl->seq);
	if (impl->seq == seq)
		do_rescan(impl);
}

static const struct pw_core_proxy_events core_events = {
	PW_VERSION_CORE_EVENTS,
	.done = core_done
};

static void on_state_changed(void *_data, enum pw_remote_state old, enum pw_remote_state state, const char *error)
{
	struct impl *impl = _data;

	switch (state) {
	case PW_REMOTE_STATE_ERROR:
		pw_log_error(NAME" %p: remote error: %s", impl, error);
		break;

	case PW_REMOTE_STATE_CONNECTED:
		pw_log_info(NAME" %p: connected", impl);
		impl->core_proxy = pw_remote_get_core_proxy(impl->remote);
		pw_core_proxy_add_listener(impl->core_proxy,
					   &impl->core_listener,
					   &core_events, impl);
		impl->registry_proxy = pw_core_proxy_get_registry(impl->core_proxy,
                                                PW_VERSION_REGISTRY_PROXY, 0);
		pw_registry_proxy_add_listener(impl->registry_proxy,
                                               &impl->registry_listener,
                                               &registry_events, impl);
		schedule_rescan(impl);
		break;

	case PW_REMOTE_STATE_UNCONNECTED:
		pw_log_info(NAME" %p: disconnected", impl);
		impl->core_proxy = NULL;
		impl->registry_proxy = NULL;
		break;

	default:
		printf("remote state: \"%s\"\n", pw_remote_state_as_string(state));
		break;
	}
}

static const struct pw_remote_events remote_events = {
	PW_VERSION_REMOTE_EVENTS,
	.state_changed = on_state_changed,
};

int sm_policy_ep_start(struct pw_remote *remote)
{
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->core = pw_remote_get_core(remote);
	impl->remote = remote;

	pw_map_init(&impl->globals, 64, 64);

	spa_list_init(&impl->client_list);
	spa_list_init(&impl->session_list);
	spa_list_init(&impl->endpoint_list);

	pw_remote_add_listener(impl->remote, &impl->remote_listener, &remote_events, impl);

	return 0;
}

int sm_policy_ep_stop(struct pw_core *core)
{
	return 0;
}
