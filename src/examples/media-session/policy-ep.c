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

#include "media-session.h"

#define NAME "policy-ep"
#define SESSION_KEY	"policy-endpoint"

#define DEFAULT_CHANNELS	2
#define DEFAULT_SAMPLERATE	48000

#define DEFAULT_IDLE_SECONDS	3

struct impl {
	struct timespec now;

	struct sm_media_session *session;
	struct spa_hook listener;

	struct pw_core *core;

	struct spa_list endpoint_list;
	int seq;
};

struct endpoint {
	struct sm_endpoint *obj;

	uint32_t id;
	struct impl *impl;

	struct spa_list link;		/**< link in impl endpoint_list */
	enum pw_direction direction;

	struct endpoint *peer;

	uint32_t client_id;
	int32_t priority;

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
	struct sm_endpoint_stream *obj;

	uint32_t id;
	struct impl *impl;

	struct endpoint *endpoint;
};

static int
handle_endpoint(struct impl *impl, struct sm_object *object)
{
	const char *str, *media_class;
	enum pw_direction direction;
	struct endpoint *ep;
	uint32_t client_id = SPA_ID_INVALID;

	if (object->props) {
		if ((str = pw_properties_get(object->props, PW_KEY_CLIENT_ID)) != NULL)
			client_id = atoi(str);
	}

	media_class = object->props ? pw_properties_get(object->props, PW_KEY_MEDIA_CLASS) : NULL;

	pw_log_debug(NAME" %p: endpoint "PW_KEY_MEDIA_CLASS" %s", impl, media_class);

	if (media_class == NULL)
		return 0;

	ep = sm_object_add_data(object, SESSION_KEY, sizeof(struct endpoint));
	ep->obj = (struct sm_endpoint*)object;
	ep->id = object->id;
	ep->impl = impl;
	ep->client_id = client_id;
	ep->type = ENDPOINT_TYPE_UNKNOWN;
	ep->enabled = true;
	spa_list_append(&impl->endpoint_list, &ep->link);

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
		pw_log_debug(NAME "%p: endpoint %d is stream %s", impl, object->id, ep->media);
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

		pw_log_debug(NAME" %p: endpoint %d prio:%d", impl, object->id, ep->priority);
	}
	return 1;
}

static int
handle_stream(struct impl *impl, struct sm_object *object)
{
	struct sm_endpoint_stream *stream = (struct sm_endpoint_stream*)object;
	struct stream *s;
	struct endpoint *ep;

	if (stream->endpoint == NULL)
		return 0;

	ep = sm_object_get_data(&stream->endpoint->obj, SESSION_KEY);
	if (ep == NULL)
		return 0;

	s = sm_object_add_data(object, SESSION_KEY, sizeof(struct stream));
	s->obj = (struct sm_endpoint_stream*)object;
	s->id = object->id;
	s->impl = impl;
	s->endpoint = ep;

	return 0;
}

static void session_create(void *data, struct sm_object *object)
{
	struct impl *impl = data;
	int res;

	switch (object->type) {
	case PW_TYPE_INTERFACE_Endpoint:
		res = handle_endpoint(impl, object);
		break;

	case PW_TYPE_INTERFACE_EndpointStream:
		res = handle_stream(impl, object);
		break;

	default:
		res = 0;
		break;
	}
	if (res < 0) {
		pw_log_warn(NAME" %p: can't handle global %d", impl, object->id);
	}
	else
		sm_media_session_schedule_rescan(impl->session);
}

static void session_remove(void *data, struct sm_object *object)
{
	struct impl *impl = data;
	pw_log_debug(NAME " %p: remove global '%d'", impl, object->id);

	switch (object->type) {
	case PW_TYPE_INTERFACE_Endpoint:
	{
		struct endpoint *ep;
		if ((ep = sm_object_get_data(object, SESSION_KEY)) != NULL) {
			spa_list_remove(&ep->link);
			free(ep->media);
		}
		break;
	}
	default:
		break;
	}

	sm_media_session_schedule_rescan(impl->session);
}

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
			impl, endpoint->id, endpoint->enabled, endpoint->busy, endpoint->exclusive);

	if (!endpoint->enabled)
		return 0;

	if (find->path_id != SPA_ID_INVALID && endpoint->id != find->path_id)
		return 0;

	if (find->path_id == SPA_ID_INVALID) {
		if (endpoint->obj->info == NULL ||
		    (props = endpoint->obj->info->props) == NULL)
			return 0;

		if ((str = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS)) == NULL)
			return 0;

		if (strcmp(str, find->media_class) != 0)
			return 0;

		plugged = endpoint->plugged;
		priority = endpoint->priority;
	}

	if ((find->exclusive && endpoint->busy) || endpoint->exclusive) {
		pw_log_debug(NAME " %p: endpoint '%d' in use", impl, endpoint->id);
		return 0;
	}

	pw_log_debug(NAME " %p: found endpoint '%d' %"PRIu64" prio:%d", impl,
			endpoint->id, plugged, priority);

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
	struct impl *impl = peer->impl;
	struct pw_properties *props;

	pw_log_debug(NAME " %p: link endpoints %d %d %d", impl, max, endpoint->id, peer->id);

	if (endpoint->direction == PW_DIRECTION_INPUT) {
		struct endpoint *t = endpoint;
		endpoint = peer;
		peer = t;
	}
	props = pw_properties_new(NULL, NULL);
	pw_properties_setf(props, PW_KEY_ENDPOINT_LINK_OUTPUT_ENDPOINT, "%d", endpoint->id);
	pw_properties_setf(props, PW_KEY_ENDPOINT_LINK_OUTPUT_STREAM, "%d", -1);
	pw_properties_setf(props, PW_KEY_ENDPOINT_LINK_INPUT_ENDPOINT, "%d", peer->id);
	pw_properties_setf(props, PW_KEY_ENDPOINT_LINK_INPUT_STREAM, "%d", -1);
	pw_log_debug(NAME " %p: endpoint %d -> endpoint %d", impl,
			endpoint->id, peer->id);

	pw_endpoint_proxy_create_link((struct pw_endpoint_proxy*)endpoint->obj->obj.proxy,
                                         &props->dict);

	pw_properties_free(props);

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

	if (ep->obj->info == NULL || ep->obj->info->props == NULL) {
		pw_log_debug(NAME " %p: endpoint %d has no properties", impl, ep->id);
		return 0;
	}

	if (ep->peer != NULL)
		return 0;

	info = ep->obj->info;
	props = info->props;

        str = spa_dict_lookup(props, PW_KEY_ENDPOINT_AUTOCONNECT);
        if (str == NULL || !pw_properties_parse_bool(str)) {
		pw_log_debug(NAME" %p: endpoint %d does not need autoconnect", impl, ep->id);
                return 0;
	}

	if ((media = spa_dict_lookup(props, PW_KEY_MEDIA_TYPE)) == NULL)
		media = ep->media;
	if (media == NULL) {
		pw_log_debug(NAME" %p: endpoint %d has unknown media", impl, ep->id);
		return 0;
	}

	spa_zero(find);

	if ((category = spa_dict_lookup(props, PW_KEY_MEDIA_CATEGORY)) == NULL) {
		pw_log_debug(NAME" %p: endpoint %d find category",
			impl, ep->id);
		if (ep->direction == PW_DIRECTION_INPUT) {
			category = "Capture";
		} else if (ep->direction == PW_DIRECTION_OUTPUT) {
			category = "Playback";
		} else {
			pw_log_warn(NAME" %p: endpoint %d can't determine category",
					impl, ep->id);
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
					impl, ep->id, category);
			return -EINVAL;
		}
	}
	else if (strcmp(media, "Video") == 0) {
		if (strcmp(category, "Capture") == 0)
			find.media_class = "Video/Source";
		else {
			pw_log_debug(NAME" %p: endpoint %d unhandled category %s",
					impl, ep->id, category);
			return -EINVAL;
		}
	}
	else {
		pw_log_debug(NAME" %p: endpoint %d unhandled media %s",
				impl, ep->id, media);
		return -EINVAL;
	}

	if (strcmp(category, "Capture") == 0)
		direction = PW_DIRECTION_OUTPUT;
	else if (strcmp(category, "Playback") == 0)
		direction = PW_DIRECTION_INPUT;
	else {
		pw_log_debug(NAME" %p: endpoint %d unhandled category %s",
				impl, ep->id, category);
		return -EINVAL;
	}

	str = spa_dict_lookup(props, PW_KEY_ENDPOINT_TARGET);
	if (str != NULL)
		find.path_id = atoi(str);
	else
		find.path_id = SPA_ID_INVALID;

	pw_log_info(NAME " %p: '%s' '%s' '%s' exclusive:%d target %d", impl,
			media, category, role, exclusive, find.path_id);

	find.impl = impl;
	find.exclusive = exclusive;

	spa_list_for_each(peer, &impl->endpoint_list, link)
		find_endpoint(&find, peer);

	if (find.endpoint == NULL && find.path_id != SPA_ID_INVALID) {
		struct sm_object *obj;
		pw_log_debug(NAME " %p: no endpoint found for %d, try endpoint", impl, ep->id);

		if ((obj = sm_media_session_find_object(impl->session, find.path_id)) != NULL) {
			if (obj->type == PW_TYPE_INTERFACE_Endpoint) {
				peer = sm_object_get_data(obj, SESSION_KEY);
				goto do_link;
			}
		}
		else {
			str = spa_dict_lookup(props, PW_KEY_NODE_DONT_RECONNECT);
			if (str != NULL && pw_properties_parse_bool(str)) {
//				pw_registry_proxy_destroy(impl->registry_proxy, ep->id);
				return -ENOENT;
			}
		}
	}

	if (find.endpoint == NULL) {
		struct sm_object *obj;

		pw_log_warn(NAME " %p: no endpoint found for %d", impl, ep->id);

		obj = sm_media_session_find_object(impl->session, ep->client_id);
		if (obj && obj->type == PW_TYPE_INTERFACE_Client) {
			pw_client_proxy_error((struct pw_client_proxy*)obj->proxy,
				ep->id, -ENOENT, "no endpoint available");
		}
		return -ENOENT;
	}
	peer = find.endpoint;

	if (exclusive && peer->busy) {
		pw_log_warn(NAME" %p: endpoint %d busy, can't get exclusive access", impl, peer->id);
		return -EBUSY;
	}
	peer->exclusive = exclusive;

	pw_log_debug(NAME" %p: linking to endpoint '%d'", impl, peer->id);

        peer->busy = true;

do_link:
	link_endpoints(ep, direction, peer, 1);

        return 1;
}

static void session_rescan(void *data, int seq)
{
	struct impl *impl = data;
	struct endpoint *ep;

	clock_gettime(CLOCK_MONOTONIC, &impl->now);
	pw_log_debug(NAME" %p: rescan", impl);

	spa_list_for_each(ep, &impl->endpoint_list, link)
		rescan_endpoint(impl, ep);
}

static const struct sm_media_session_events session_events = {
	SM_VERSION_MEDIA_SESSION_EVENTS,
	.create = session_create,
	.remove = session_remove,
	.rescan = session_rescan,
};

void *sm_policy_ep_start(struct sm_media_session *session)
{
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	impl->session = session;
	impl->core = session->core;

	spa_list_init(&impl->endpoint_list);

	sm_media_session_add_listener(impl->session, &impl->listener, &session_events, impl);

	return impl;
}

int sm_policy_ep_stop(void *data)
{
	struct impl *impl = data;
	free(impl);
	return 0;
}
