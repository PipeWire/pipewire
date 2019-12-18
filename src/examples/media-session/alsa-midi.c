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

#include <spa/utils/names.h>
#include <spa/node/keys.h>

#include "pipewire/pipewire.h"

#include "media-session.h"

struct impl {
	struct sm_media_session *session;
	struct spa_hook listener;

	struct pw_properties *props;
	struct pw_proxy *proxy;
};

static void session_destroy(void *data)
{
	struct impl *impl = data;
	spa_hook_remove(&impl->listener);
	pw_proxy_destroy(impl->proxy);
	pw_properties_free(impl->props);
	free(impl);
}

static const struct sm_media_session_events session_events = {
	SM_VERSION_MEDIA_SESSION_EVENTS,
	.destroy = session_destroy,
};

int sm_alsa_midi_start(struct sm_media_session *session)
{
	struct impl *impl;
	int res;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return -errno;

	impl->session = session;
	impl->props = pw_properties_new(
			SPA_KEY_FACTORY_NAME, SPA_NAME_API_ALSA_SEQ_BRIDGE,
			SPA_KEY_NODE_NAME, "Midi-Bridge",
			NULL);
	if (impl->props == NULL) {
		res = -errno;
		goto cleanup;
	}

	impl->proxy = sm_media_session_create_object(session,
				"spa-node-factory",
				PW_TYPE_INTERFACE_Node,
				PW_VERSION_NODE,
				&impl->props->dict,
                                0);

	if (impl->proxy == NULL) {
		res = -errno;
		goto cleanup_props;
	}
	sm_media_session_add_listener(session, &impl->listener, &session_events, impl);

	return 0;

cleanup_props:
	pw_properties_free(impl->props);
cleanup:
	free(impl);
	return res;
}
