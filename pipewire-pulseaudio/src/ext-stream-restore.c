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

#include <pipewire/pipewire.h>

#include <pulse/ext-stream-restore.h>

#include "internal.h"

#define EXT_VERSION	1

struct stream_data {
	pa_context *context;
	pa_ext_stream_restore_test_cb_t test_cb;
        pa_ext_stream_restore_read_cb_t read_cb;
        pa_context_success_cb_t success_cb;
	void *userdata;
};

static void restore_test(pa_operation *o, void *userdata)
{
	struct stream_data *d = userdata;

	if (d->test_cb)
		d->test_cb(o->context, EXT_VERSION, d->userdata);

	pa_operation_done(o);
}

SPA_EXPORT
pa_operation *pa_ext_stream_restore_test(
        pa_context *c,
        pa_ext_stream_restore_test_cb_t cb,
        void *userdata)
{
	pa_operation *o;
	struct stream_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	o = pa_operation_new(c, NULL, restore_test, sizeof(struct stream_data));
	d = o->userdata;
	d->context = c;
	d->test_cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

static void restore_read(pa_operation *o, void *userdata)
{
	struct stream_data *d = userdata;

	if (d->read_cb)
		d->read_cb(o->context, NULL, 1, d->userdata);

	pa_operation_done(o);
}

SPA_EXPORT
pa_operation *pa_ext_stream_restore_read(
        pa_context *c,
        pa_ext_stream_restore_read_cb_t cb,
        void *userdata)
{
	pa_operation *o;
	struct stream_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	o = pa_operation_new(c, NULL, restore_read, sizeof(struct stream_data));
	d = o->userdata;
	d->context = c;
	d->read_cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

static void on_success(pa_operation *o, void *userdata)
{
	struct stream_data *d = userdata;
	if (d->success_cb)
		d->success_cb(o->context, PA_OK, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation *pa_ext_stream_restore_write(
        pa_context *c,
        pa_update_mode_t mode,
        const pa_ext_stream_restore_info data[],
        unsigned n,
        int apply_immediately,
        pa_context_success_cb_t cb,
        void *userdata)
{
	pa_operation *o;
	struct stream_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	o = pa_operation_new(c, NULL, on_success, sizeof(struct stream_data));
	d = o->userdata;
	d->context = c;
	d->success_cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

/** Delete entries from the stream database. \since 0.9.12 */
SPA_EXPORT
pa_operation *pa_ext_stream_restore_delete(
        pa_context *c,
        const char *const s[],
        pa_context_success_cb_t cb,
        void *userdata)
{
	pa_operation *o;
	struct stream_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	o = pa_operation_new(c, NULL, on_success, sizeof(struct stream_data));
	d = o->userdata;
	d->context = c;
	d->success_cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

/** Subscribe to changes in the stream database. \since 0.9.12 */
SPA_EXPORT
pa_operation *pa_ext_stream_restore_subscribe(
        pa_context *c,
        int enable,
        pa_context_success_cb_t cb,
        void *userdata)
{
	pa_operation *o;
	struct stream_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	o = pa_operation_new(c, NULL, on_success, sizeof(struct stream_data));
	d = o->userdata;
	d->context = c;
	d->success_cb = cb;
	d->userdata = userdata;
	pa_operation_sync(o);

	return o;
}

/** Set the subscription callback that is called when
 * pa_ext_stream_restore_subscribe() was called. \since 0.9.12 */
SPA_EXPORT
void pa_ext_stream_restore_set_subscribe_cb(
        pa_context *c,
        pa_ext_stream_restore_subscribe_cb_t cb,
        void *userdata)
{
	pw_log_warn("Not Implemented");
}
