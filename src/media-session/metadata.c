/* Metadata API
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

#include "pipewire/pipewire.h"
#include "pipewire/array.h"

#include <spa/utils/string.h>

#include <pipewire/extensions/metadata.h>

#include "media-session.h"

/** \page page_media_session_module_metadata Media Session Module: Metadata
 */

#define NAME "metadata"

struct metadata {
	struct pw_impl_metadata *impl;
	struct pw_metadata *metadata;

	struct sm_media_session *session;
	struct spa_hook session_listener;
	struct pw_proxy *proxy;
};

static void session_destroy(void *data)
{
	struct metadata *this = data;

	spa_hook_remove(&this->session_listener);
	pw_proxy_destroy(this->proxy);

	pw_impl_metadata_destroy(this->impl);
	free(this);
}

static const struct sm_media_session_events session_events = {
	SM_VERSION_MEDIA_SESSION_EVENTS,
	.destroy = session_destroy,
};

struct pw_metadata *sm_media_session_export_metadata(struct sm_media_session *sess,
		const char *name)
{
	struct metadata *this;
	int res;
	struct spa_dict_item items[1];

	this = calloc(1, sizeof(*this));
	if (this == NULL)
		goto error_errno;

	this->impl = pw_context_create_metadata(sess->context,
			name, NULL, 0);
	if (this->impl == NULL)
		goto error_errno;

	this->metadata = pw_impl_metadata_get_implementation(this->impl);

	items[0] = SPA_DICT_ITEM_INIT(PW_KEY_METADATA_NAME, name);

	this->session = sess;
	this->proxy = sm_media_session_export(sess,
			PW_TYPE_INTERFACE_Metadata,
			&SPA_DICT_INIT_ARRAY(items),
			this->metadata, 0);
	if (this->proxy == NULL)
		goto error_errno;

	sm_media_session_add_listener(sess, &this->session_listener,
			&session_events, this);

	return this->metadata;

error_errno:
	res = -errno;
	goto error_free;
error_free:
	free(this);
	errno = -res;
	return NULL;
}
