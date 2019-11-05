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

#include <alsa/use-case.h>
#include <alsa/asoundlib.h>

#include <spa/node/node.h>
#include <spa/utils/hook.h>
#include <spa/utils/names.h>
#include <spa/utils/keys.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>
#include <spa/debug/dict.h>

#include "pipewire/pipewire.h"
#include "pipewire/private.h"

static int client_endpoint_set_id(void *object, uint32_t id)
{
	struct alsa_object *obj = object;

	obj->client_endpoint_info.id = id;
	obj->client_endpoint_info.name = (char*)pw_properties_get(obj->props, PW_KEY_DEVICE_DESCRIPTION);
	obj->client_endpoint_info.media_class = (char*)pw_properties_get(obj->props, PW_KEY_MEDIA_CLASS);

	pw_client_endpoint_proxy_update(obj->client_endpoint,
			PW_CLIENT_ENDPOINT_UPDATE_INFO,
			0, NULL,
			&obj->client_endpoint_info);
	return 0;
}

static int client_endpoint_set_session_id(void *object, uint32_t id)
{
	struct alsa_object *obj = object;
	obj->client_endpoint_info.session_id = id;
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


static const struct pw_client_endpoint_proxy_events client_endpoint_events = {
	PW_VERSION_CLIENT_ENDPOINT_PROXY_EVENTS,
	.set_id = client_endpoint_set_id,
	.set_session_id = client_endpoint_set_session_id,
	.set_param = client_endpoint_set_param,
	.stream_set_param = client_endpoint_stream_set_param,
};

/** fallback, one stream for each node */
static int setup_alsa_fallback_endpoint(struct alsa_object *obj)
{
	struct alsa_node *n;

	spa_list_for_each(n, &obj->node_list, link) {
		n->info.version = PW_VERSION_ENDPOINT_STREAM_INFO;
		n->info.id = n->id;
		n->info.endpoint_id = obj->client_endpoint_info.id;
		n->info.name = (char*)pw_properties_get(n->props, PW_KEY_NODE_DESCRIPTION);
		n->info.change_mask = PW_ENDPOINT_STREAM_CHANGE_MASK_PROPS;
		n->info.props = &n->props->dict;

		pw_log_debug("stream %d", n->id);
		pw_client_endpoint_proxy_stream_update(obj->client_endpoint,
				n->id,
				PW_CLIENT_ENDPOINT_STREAM_UPDATE_INFO,
				0, NULL,
				&n->info);
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

	if ((res = snd_use_case_mgr_open(&obj->ucm, card_name)) < 0) {
		pw_log_error("can not open UCM for %s: %s", card_name, snd_strerror(res));
		goto exit;
	}

	num_verbs = snd_use_case_verb_list(obj->ucm, &verb_list);
	if (num_verbs < 0) {
		res = num_verbs;
		pw_log_error("UCM verb list not found for %s: %s", card_name, snd_strerror(num_verbs));
		goto close_exit;
	}

	for (i = 0; i < num_verbs; i++) {
		pw_log_debug("verb: %s", verb_list[i]);
	}

	obj->use_ucm = true;

	snd_use_case_free_list(verb_list, num_verbs);

	return 0;
close_exit:
	snd_use_case_mgr_close(obj->ucm);
exit:
	obj->ucm = NULL;
	free(name_free);
	return res;

}

static int setup_alsa_endpoint(struct alsa_object *obj)
{
	struct impl *impl = obj->monitor->impl;
	int res;

	obj->client_endpoint = pw_core_proxy_create_object(impl->core_proxy,
						"client-endpoint",
						PW_TYPE_INTERFACE_ClientEndpoint,
						PW_VERSION_CLIENT_ENDPOINT_PROXY,
						&obj->props->dict, 0);

	obj->client_endpoint_info.version = PW_VERSION_ENDPOINT_INFO;
	obj->client_endpoint_info.name = "name";
	obj->client_endpoint_info.media_class = "media-class";

	pw_client_endpoint_proxy_add_listener(obj->client_endpoint,
			&obj->client_endpoint_listener,
			&client_endpoint_events,
			obj);

	if ((res = setup_alsa_ucm_endpoint(obj)) < 0)
		res = setup_alsa_fallback_endpoint(obj);

	return res;
}
