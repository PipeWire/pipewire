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

#include <alsa/asoundlib.h>
#include <alsa/use-case.h>

#include <spa/node/node.h>
#include <spa/utils/hook.h>
#include <spa/utils/names.h>
#include <spa/utils/keys.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/dict.h>
#include <spa/debug/pod.h>

#include "pipewire/pipewire.h"

#undef NAME
#define NAME "alsa-endpoint"

struct endpoint {
	struct spa_list link;

	struct pw_properties *props;

	struct alsa_node *obj;
	struct spa_hook listener;

	struct pw_client_endpoint_proxy *client_endpoint;
	struct spa_hook client_endpoint_listener;
	struct pw_endpoint_info info;

	struct spa_param_info params[5];
	uint32_t n_params;

	struct endpoint *monitor;

	unsigned int use_ucm:1;
	snd_use_case_mgr_t *ucm;

	struct spa_audio_info format;

	struct spa_list stream_list;
};

struct stream {
	struct spa_list link;

	struct pw_properties *props;
	struct pw_endpoint_stream_info info;

	struct spa_audio_info format;

	unsigned int active:1;
};

static int client_endpoint_set_id(void *object, uint32_t id)
{
	struct endpoint *endpoint = object;
	endpoint->info.id = id;
	return 0;
}

static int client_endpoint_set_session_id(void *object, uint32_t id)
{
	struct endpoint *endpoint = object;
	endpoint->info.session_id = id;
	return 0;
}

static int client_endpoint_set_param(void *object,
		uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	struct endpoint *endpoint = object;
	struct impl *impl = endpoint->obj->impl;
	pw_log_debug(NAME " %p: endpoint %p set param %d", impl, endpoint, id);
	return pw_node_proxy_set_param((struct pw_node_proxy*)endpoint->obj->proxy,
				id, flags, param);
}


static int client_endpoint_stream_set_param(void *object, uint32_t stream_id,
		uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	return -ENOTSUP;
}

static int stream_set_active(struct endpoint *endpoint, struct stream *stream, bool active)
{
	char buf[1024];
	struct spa_pod_builder b = { 0, };
	struct spa_pod *param;

	if (stream->active == active)
		return 0;

	if (active) {
		stream->format.info.raw.rate = 48000;

		spa_pod_builder_init(&b, buf, sizeof(buf));
		param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &stream->format.info.raw);
		param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamPortConfig, SPA_PARAM_PortConfig,
			SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(endpoint->info.direction),
			SPA_PARAM_PORT_CONFIG_mode,	 SPA_POD_Id(SPA_PARAM_PORT_CONFIG_MODE_dsp),
			SPA_PARAM_PORT_CONFIG_monitor,   SPA_POD_Bool(true),
			SPA_PARAM_PORT_CONFIG_format,    SPA_POD_Pod(param));

		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_pod(2, NULL, param);

		pw_node_proxy_set_param((struct pw_node_proxy*)endpoint->obj->proxy,
				SPA_PARAM_PortConfig, 0, param);
	}
	stream->active = active;
	return 0;
}

static int client_endpoint_create_link(void *object, const struct spa_dict *props)
{
	struct endpoint *endpoint = object;
	struct impl *impl = endpoint->obj->impl;
	struct pw_properties *p;
	int res;

	pw_log_debug(NAME" %p: endpoint %p", impl, endpoint);

	if (props == NULL)
		return -EINVAL;

	p = pw_properties_new_dict(props);
	if (p == NULL)
		return -errno;

	if (endpoint->info.direction == PW_DIRECTION_OUTPUT) {
		const char *str;
		struct sm_object *obj;

		str = spa_dict_lookup(props, PW_KEY_ENDPOINT_LINK_INPUT_ENDPOINT);
		if (str == NULL) {
			pw_log_warn(NAME" %p: no target endpoint given", impl);
			res = -EINVAL;
			goto exit;
		}
		obj = sm_media_session_find_object(impl->session, atoi(str));
		if (obj == NULL || obj->type != PW_TYPE_INTERFACE_Endpoint) {
			pw_log_warn(NAME" %p: could not find endpoint %s (%p)", impl, str, obj);
			res = -EINVAL;
			goto exit;
		}

		pw_properties_setf(p, PW_KEY_LINK_OUTPUT_NODE, "%d", endpoint->obj->info->id);
		pw_properties_setf(p, PW_KEY_LINK_OUTPUT_PORT, "-1");

		pw_endpoint_proxy_create_link((struct pw_endpoint_proxy*)obj->proxy, &p->dict);
	} else {
		pw_properties_setf(p, PW_KEY_LINK_INPUT_NODE, "%d", endpoint->obj->info->id);
		pw_properties_setf(p, PW_KEY_LINK_INPUT_PORT, "-1");

		sm_media_session_create_links(impl->session, &p->dict);
	}

	res = 0;
exit:
	pw_properties_free(p);

	return res;
}

static const struct pw_client_endpoint_proxy_events client_endpoint_events = {
	PW_VERSION_CLIENT_ENDPOINT_PROXY_EVENTS,
	.set_id = client_endpoint_set_id,
	.set_session_id = client_endpoint_set_session_id,
	.set_param = client_endpoint_set_param,
	.stream_set_param = client_endpoint_stream_set_param,
	.create_link = client_endpoint_create_link,
};

static struct stream *endpoint_add_stream(struct endpoint *endpoint)
{
	struct stream *s;
	const char *str;

	s = calloc(1, sizeof(*s));
	if (s == NULL)
		return NULL;

	s->props = pw_properties_new(NULL, NULL);
	if ((str = pw_properties_get(endpoint->props, PW_KEY_MEDIA_CLASS)) != NULL)
		pw_properties_set(s->props, PW_KEY_MEDIA_CLASS, str);
	if ((str = pw_properties_get(endpoint->props, PW_KEY_PRIORITY_SESSION)) != NULL)
		pw_properties_set(s->props, PW_KEY_PRIORITY_SESSION, str);
	if (endpoint->info.direction == PW_DIRECTION_OUTPUT) {
		if (endpoint->monitor != NULL)
			pw_properties_set(s->props, PW_KEY_ENDPOINT_STREAM_NAME, "Monitor");
		else
			pw_properties_set(s->props, PW_KEY_ENDPOINT_STREAM_NAME, "Playback");
	} else {
		pw_properties_set(s->props, PW_KEY_ENDPOINT_STREAM_NAME, "Capture");
	}

	s->info.version = PW_VERSION_ENDPOINT_STREAM_INFO;
	s->info.id = endpoint->info.n_streams;
	s->info.endpoint_id = endpoint->info.id;
	s->info.name = (char*)pw_properties_get(s->props, PW_KEY_ENDPOINT_STREAM_NAME);
	s->info.change_mask = PW_ENDPOINT_STREAM_CHANGE_MASK_PROPS;
	s->info.props = &s->props->dict;
	s->format = endpoint->format;

	pw_log_debug("stream %d", s->info.id);
	pw_client_endpoint_proxy_stream_update(endpoint->client_endpoint,
			s->info.id,
			PW_CLIENT_ENDPOINT_STREAM_UPDATE_INFO,
			0, NULL,
			&s->info);

	spa_list_append(&endpoint->stream_list, &s->link);
	endpoint->info.n_streams++;

	return s;
}

static struct endpoint *make_endpoint(struct alsa_node *obj, struct endpoint *monitor);

static void complete_endpoint(void *data)
{
	struct endpoint *endpoint = data;
	struct stream *stream;
	struct sm_param *p;

	pw_log_debug("endpoint %p: complete", endpoint);

	spa_list_for_each(p, &endpoint->obj->snode->param_list, link) {
		struct spa_audio_info info = { 0, };

		if (p->id != SPA_PARAM_EnumFormat)
			continue;

		if (spa_format_parse(p->param, &info.media_type, &info.media_subtype) < 0)
			continue;

		if (info.media_type != SPA_MEDIA_TYPE_audio ||
		    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
			continue;

		spa_pod_object_fixate((struct spa_pod_object*)p->param);
		if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
			spa_debug_pod(2, NULL, p->param);

		if (spa_format_audio_raw_parse(p->param, &info.info.raw) < 0)
			continue;

		if (endpoint->format.info.raw.channels < info.info.raw.channels)
			endpoint->format = info;
	}

	pw_client_endpoint_proxy_update(endpoint->client_endpoint,
			PW_CLIENT_ENDPOINT_UPDATE_PARAMS |
			PW_CLIENT_ENDPOINT_UPDATE_INFO,
			0, NULL,
			&endpoint->info);

	stream = endpoint_add_stream(endpoint);

	if (endpoint->info.direction == PW_DIRECTION_INPUT) {
		struct endpoint *monitor;

		/* make monitor for sinks */
		monitor = make_endpoint(endpoint->obj, endpoint);
		if (monitor == NULL)
			return;

		endpoint_add_stream(monitor);
	}
	stream_set_active(endpoint, stream, true);
}

static struct endpoint *make_endpoint(struct alsa_node *obj, struct endpoint *monitor)
{
	struct impl *impl = obj->impl;
	struct pw_properties *props;
	struct endpoint *endpoint;
	struct pw_proxy *proxy;
	const char *str, *media_class = NULL, *name = NULL;
	uint32_t subscribe[4], n_subscribe = 0;

	props = pw_properties_new(NULL, NULL);
	if (props == NULL)
		return NULL;

	if (obj->props) {
		if ((media_class = pw_properties_get(obj->props, PW_KEY_MEDIA_CLASS)) != NULL) {
			if (monitor != NULL) {
				pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Source");
			 } else {
				pw_properties_set(props, PW_KEY_MEDIA_CLASS, media_class);
			 }
		}
		if ((str = pw_properties_get(obj->props, PW_KEY_PRIORITY_SESSION)) != NULL)
			pw_properties_set(props, PW_KEY_PRIORITY_SESSION, str);
		if ((name = pw_properties_get(obj->props, PW_KEY_NODE_DESCRIPTION)) != NULL) {
			if (monitor != NULL) {
				pw_properties_setf(props, PW_KEY_ENDPOINT_NAME, "Monitor of %s", monitor->info.name);
				pw_properties_setf(props, PW_KEY_ENDPOINT_MONITOR, "%d", monitor->info.id);
			} else {
				pw_properties_set(props, PW_KEY_ENDPOINT_NAME, name);
			}
		}
	}
	if (obj->object && obj->object->props) {
		if ((str = pw_properties_get(obj->object->props, PW_KEY_DEVICE_ICON_NAME)) != NULL)
			pw_properties_set(props, PW_KEY_ENDPOINT_ICON_NAME, str);
	}

	proxy = sm_media_session_create_object(impl->session,
						"client-endpoint",
						PW_TYPE_INTERFACE_ClientEndpoint,
						PW_VERSION_CLIENT_ENDPOINT_PROXY,
						&props->dict, sizeof(*endpoint));
	if (proxy == NULL) {
		pw_properties_free(props);
		return NULL;
	}

	endpoint = pw_proxy_get_user_data(proxy);
	endpoint->obj = obj;
	endpoint->monitor = monitor;
	endpoint->props = props;
	endpoint->client_endpoint = (struct pw_client_endpoint_proxy *) proxy;
	endpoint->info.version = PW_VERSION_ENDPOINT_INFO;
	endpoint->info.name = (char*)pw_properties_get(endpoint->props, PW_KEY_ENDPOINT_NAME);
	endpoint->info.media_class = (char*)pw_properties_get(endpoint->props, PW_KEY_MEDIA_CLASS);
	endpoint->info.session_id = impl->session->session->obj.id;
	endpoint->info.direction = monitor != NULL ? PW_DIRECTION_OUTPUT : obj->direction;
	endpoint->info.flags = 0;
	endpoint->info.change_mask =
		PW_ENDPOINT_CHANGE_MASK_STREAMS |
		PW_ENDPOINT_CHANGE_MASK_SESSION |
		PW_ENDPOINT_CHANGE_MASK_PROPS |
		PW_ENDPOINT_CHANGE_MASK_PARAMS;
	endpoint->info.n_streams = 0;
	endpoint->info.props = &endpoint->props->dict;
	endpoint->params[0] = SPA_PARAM_INFO(SPA_PARAM_PropInfo, SPA_PARAM_INFO_READ);
	endpoint->params[1] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE);
	endpoint->info.params = endpoint->params;
	endpoint->info.n_params = 2;
	spa_list_init(&endpoint->stream_list);

	pw_log_debug(NAME" %p: new endpoint %p for alsa node %p", impl, endpoint, obj);

	pw_client_endpoint_proxy_add_listener(endpoint->client_endpoint,
			&endpoint->client_endpoint_listener,
			&client_endpoint_events,
			endpoint);

	subscribe[n_subscribe++] = SPA_PARAM_EnumFormat;
	subscribe[n_subscribe++] = SPA_PARAM_Props;
	subscribe[n_subscribe++] = SPA_PARAM_PropInfo;
	pw_log_debug(NAME" %p: endpoint %p proxy %p subscribe %d params", impl,
				endpoint, obj->proxy, n_subscribe);
	pw_node_proxy_subscribe_params((struct pw_node_proxy*)obj->proxy,
				subscribe, n_subscribe);

	if (monitor == NULL)
		sm_media_session_sync(impl->session, complete_endpoint, endpoint);

	return endpoint;
}

/** fallback, one stream for each node */
static int setup_alsa_fallback_endpoint(struct alsa_object *obj)
{
	struct alsa_node *n;

	spa_list_for_each(n, &obj->node_list, link) {
		struct endpoint *endpoint;

		endpoint = make_endpoint(n, NULL);
		if (endpoint == NULL)
			return -errno;
	}
	return 0;
}

/** UCM.
 *
 * We create 1 stream for each verb + modifier combination
 */
static int setup_alsa_ucm_endpoint(struct alsa_object *obj)
{
	const char *str, *card_name = NULL;
	char *name_free = NULL;
	int i, res, num_verbs;
	const char **verb_list = NULL;
	snd_use_case_mgr_t *ucm;

	card_name = pw_properties_get(obj->props, SPA_KEY_API_ALSA_CARD_NAME);
	if (card_name == NULL &&
	    (str = pw_properties_get(obj->props, SPA_KEY_API_ALSA_CARD)) != NULL) {
		snd_card_get_name(atoi(str), &name_free);
		card_name = name_free;
		pw_log_debug("got card name %s for index %s", card_name, str);
	}
	if (card_name == NULL) {
		res = -ENOTSUP;
		goto exit;
	}

	if ((res = snd_use_case_mgr_open(&ucm, card_name)) < 0) {
		pw_log_error("can not open UCM for %s: %s", card_name, snd_strerror(res));
		goto exit;
	}

	num_verbs = snd_use_case_verb_list(ucm, &verb_list);
	if (num_verbs < 0) {
		res = num_verbs;
		pw_log_error("UCM verb list not found for %s: %s", card_name, snd_strerror(num_verbs));
		goto close_exit;
	}

	for (i = 0; i < num_verbs; i++) {
		pw_log_debug("verb: %s", verb_list[i]);
	}

	snd_use_case_free_list(verb_list, num_verbs);

	res = -ENOTSUP;

close_exit:
	snd_use_case_mgr_close(ucm);
exit:
	free(name_free);
	return res;
}

static int setup_alsa_endpoint(struct alsa_object *obj)
{
	int res;

	if ((res = setup_alsa_ucm_endpoint(obj)) < 0)
		res = setup_alsa_fallback_endpoint(obj);

	return res;
}

#if 0
static int
handle_device(struct impl *impl, struct sm_object *obj)
{
	return 0;
}

static void session_update(void *data, struct sm_object *object)
{
	struct impl *impl = data;
	int res;

	switch (object->type) {
	case PW_TYPE_INTERFACE_Device:
		res = handle_device(impl, object);
		break;

	default:
		res = 0;
		break;
	}
	if (res < 0) {
		pw_log_warn(NAME" %p: can't handle global %d: %s", impl,
				object->id, spa_strerror(res));
	}
}

static void session_remove(void *data, struct sm_object *object)
{
	switch (object->type) {
	case PW_TYPE_INTERFACE_Device:
		break;
	default:
		break;
	}
}


static const struct sm_media_session_events session_events = {
	SM_VERSION_MEDIA_SESSION_EVENTS,
	.update = session_update,
	.remove = session_remove,
};

void *sm_alsa_endpoint_start(struct sm_media_session *session)
{
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	impl->session = session;
	sm_media_session_add_listener(session, &impl->listener, &session_events, impl);

	return impl;
}

int sm_alsa_endpoint_stop(void *data)
{
	return 0;
}
#endif
