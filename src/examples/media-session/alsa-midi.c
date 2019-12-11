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

	struct pw_properties *props;
	struct pw_proxy *proxy;
};

void * sm_alsa_midi_start(struct sm_media_session *session)
{
	struct impl *impl;

	impl = calloc(1, sizeof(struct impl));
	if (impl == NULL)
		return NULL;

	impl->session = session;
	impl->props = pw_properties_new(
			SPA_KEY_FACTORY_NAME, SPA_NAME_API_ALSA_SEQ_BRIDGE,
			SPA_KEY_NODE_NAME, "Midi-Bridge",
			NULL);
	if (impl->props == NULL)
		goto cleanup;

	impl->proxy = sm_media_session_create_object(session,
				"spa-node-factory",
				PW_TYPE_INTERFACE_Node,
				PW_VERSION_NODE,
				&impl->props->dict,
                                0);

	if (impl->proxy == NULL)
		goto cleanup_props;

	return impl;

cleanup_props:
	pw_properties_free(impl->props);
cleanup:
	free(impl);
	return NULL;
}

int sm_alsa_midi_stop(void *data)
{
	struct impl *impl = data;
	pw_proxy_destroy(impl->proxy);
	pw_properties_free(impl->props);
	free(impl);
	return 0;
}
