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

#include "pipewire/pipewire.h"
#include "pipewire/private.h"

struct endpoint {
	struct spa_list link;

	struct pw_properties *props;

	struct alsa_node *obj;
	struct spa_hook listener;

	struct pw_client_endpoint_proxy *client_endpoint;
	struct spa_hook client_endpoint_listener;
	struct pw_endpoint_info info;

	unsigned int use_ucm:1;
	snd_use_case_mgr_t *ucm;

	struct spa_list stream_list;
	struct spa_audio_info format;

	unsigned int active:1;
};

struct stream {
	struct spa_list link;

	struct pw_properties *props;
	struct pw_endpoint_stream_info info;

	unsigned int active:1;
};

static int client_endpoint_set_id(void *object, uint32_t id)
{
	struct endpoint *endpoint = object;
	endpoint->info.id = id;
	pw_client_endpoint_proxy_update(endpoint->client_endpoint,
			PW_CLIENT_ENDPOINT_UPDATE_INFO,
			0, NULL,
			&endpoint->info);
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
	return -ENOTSUP;
}


static int client_endpoint_stream_set_param(void *object, uint32_t stream_id,
		uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	return -ENOTSUP;
}

static int client_endpoint_create_link(void *object, const struct spa_dict *props)
{
	struct endpoint *endpoint = object;
	struct impl *impl = endpoint->obj->monitor->impl;
	struct pw_properties *p;
	char buf[1024];
	struct spa_pod_builder b = { 0, };
	struct spa_pod *param;
	int res;

	pw_log_debug(NAME" %p: endpoint %p", impl, endpoint);

	if (!endpoint->active) {
		endpoint->format.info.raw.rate = 48000;

		spa_pod_builder_init(&b, buf, sizeof(buf));
		param = spa_format_audio_raw_build(&b, SPA_PARAM_Format, &endpoint->format.info.raw);
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

		endpoint->active = true;
	}

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

static void node_event_param(void *object, int seq,
                       uint32_t id, uint32_t index, uint32_t next,
                       const struct spa_pod *param)
{
	struct endpoint *endpoint = object;
	struct alsa_node *n = endpoint->obj;
	struct impl *impl = n->monitor->impl;
	struct spa_audio_info info = { 0, };

	pw_log_debug(NAME" %p: param for node %d, %d", impl, n->info->id, id);

	if (id != SPA_PARAM_EnumFormat)
		goto error;

	if (spa_format_parse(param, &info.media_type, &info.media_subtype) < 0)
		goto error;

	if (info.media_type != SPA_MEDIA_TYPE_audio ||
	    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return;

	spa_pod_object_fixate((struct spa_pod_object*)param);
	if (pw_log_level_enabled(SPA_LOG_LEVEL_DEBUG))
		spa_debug_pod(2, NULL, param);

	if (spa_format_audio_raw_parse(param, &info.info.raw) < 0)
		goto error;

	if (endpoint->format.info.raw.channels < info.info.raw.channels)
		endpoint->format = info;
	return;

      error:
	pw_log_warn("unhandled param:");
	if (pw_log_level_enabled(SPA_LOG_LEVEL_WARN))
		spa_debug_pod(2, NULL, param);
	return;
}

static const struct pw_node_proxy_events endpoint_node_events = {
	PW_VERSION_NODE_PROXY_EVENTS,
	.param = node_event_param,
};

static struct endpoint *make_endpoint(struct alsa_node *obj)
{
	struct impl *impl = obj->monitor->impl;
	struct pw_properties *props;
	struct endpoint *endpoint;
	struct pw_proxy *proxy;
	const char *str, *media_class = NULL, *name = NULL;

	props = pw_properties_new(NULL, NULL);
	if (props == NULL)
		return NULL;

	if (obj->props) {
		if ((media_class = pw_properties_get(obj->props, PW_KEY_MEDIA_CLASS)) != NULL)
			pw_properties_set(props, PW_KEY_MEDIA_CLASS, media_class);
		if ((str = pw_properties_get(obj->props, PW_KEY_PRIORITY_SESSION)) != NULL)
			pw_properties_set(props, PW_KEY_PRIORITY_SESSION, str);
		if ((name = pw_properties_get(obj->props, PW_KEY_NODE_DESCRIPTION)) != NULL)
			pw_properties_set(props, PW_KEY_ENDPOINT_NAME, name);
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
	endpoint->props = props;
	endpoint->client_endpoint = (struct pw_client_endpoint_proxy *) proxy;
	endpoint->info.version = PW_VERSION_ENDPOINT_INFO;
	endpoint->info.name = (char*)pw_properties_get(endpoint->props, PW_KEY_ENDPOINT_NAME);
	endpoint->info.media_class = (char*)pw_properties_get(obj->props, PW_KEY_MEDIA_CLASS);
	endpoint->info.session_id = impl->session->info.id;
	endpoint->info.direction = obj->direction;
	endpoint->info.flags = 0;
	endpoint->info.change_mask =
		PW_ENDPOINT_CHANGE_MASK_STREAMS |
		PW_ENDPOINT_CHANGE_MASK_SESSION |
		PW_ENDPOINT_CHANGE_MASK_PROPS;
	endpoint->info.n_streams = 0;
	endpoint->info.props = &endpoint->props->dict;
	spa_list_init(&endpoint->stream_list);

	pw_client_endpoint_proxy_add_listener(endpoint->client_endpoint,
			&endpoint->client_endpoint_listener,
			&client_endpoint_events,
			endpoint);

	pw_proxy_add_object_listener(obj->proxy, &endpoint->listener, &endpoint_node_events, endpoint);

	pw_node_proxy_enum_params((struct pw_node_proxy*)obj->proxy,
				0, SPA_PARAM_EnumFormat,
				0, -1, NULL);

	return endpoint;
}

/** fallback, one stream for each node */
static int setup_alsa_fallback_endpoint(struct alsa_object *obj)
{
	struct alsa_node *n;
	const char *str;

	spa_list_for_each(n, &obj->node_list, link) {
		struct stream *s;
		struct endpoint *endpoint;

		endpoint = make_endpoint(n);
		if (endpoint == NULL)
			return -errno;

		s = calloc(1, sizeof(*s));
		if (s == NULL)
			return -errno;

		spa_list_append(&endpoint->stream_list, &s->link);
		endpoint->info.n_streams++;

		s->props = pw_properties_new(NULL, NULL);
		if ((str = pw_properties_get(n->props, PW_KEY_MEDIA_CLASS)) != NULL)
			pw_properties_set(s->props, PW_KEY_MEDIA_CLASS, str);
		if ((str = pw_properties_get(n->props, PW_KEY_PRIORITY_SESSION)) != NULL)
			pw_properties_set(s->props, PW_KEY_PRIORITY_SESSION, str);
		if (n->direction == PW_DIRECTION_OUTPUT)
			pw_properties_set(s->props, PW_KEY_ENDPOINT_STREAM_NAME, "Playback");
		else
			pw_properties_set(s->props, PW_KEY_ENDPOINT_STREAM_NAME, "Capture");

		s->info.version = PW_VERSION_ENDPOINT_STREAM_INFO;
		s->info.id = n->id;
		s->info.endpoint_id = endpoint->info.id;
		s->info.name = (char*)pw_properties_get(s->props, PW_KEY_ENDPOINT_STREAM_NAME);
		s->info.change_mask = PW_ENDPOINT_STREAM_CHANGE_MASK_PROPS;
		s->info.props = &s->props->dict;

		pw_log_debug("stream %d", n->id);
		pw_client_endpoint_proxy_stream_update(endpoint->client_endpoint,
				n->id,
				PW_CLIENT_ENDPOINT_STREAM_UPDATE_INFO,
				0, NULL,
				&s->info);
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
