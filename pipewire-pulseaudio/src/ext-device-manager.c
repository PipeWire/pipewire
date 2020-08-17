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

#include <pulse/ext-device-manager.h>

#include "internal.h"

struct ext_data {
	pa_context *context;
	pa_ext_device_manager_test_cb_t test_cb;
	pa_ext_device_manager_read_cb_t read_cb;
        pa_context_success_cb_t success_cb;
	int error;
	void *userdata;
};

static void device_test(pa_operation *o, void *userdata)
{
	struct ext_data *d = userdata;
	if (d->test_cb)
		d->test_cb(o->context, PA_INVALID_INDEX, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation *pa_ext_device_manager_test(
        pa_context *c,
        pa_ext_device_manager_test_cb_t cb,
        void *userdata)
{
	pa_operation *o;
	struct ext_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	o = pa_operation_new(c, NULL, device_test, sizeof(struct ext_data));
	d = o->userdata;
	d->context = c;
	d->test_cb = cb;
	d->userdata = userdata;
	d->error = PA_ERR_NOTIMPLEMENTED;
	pa_operation_sync(o);

	return o;
}

static void device_read(pa_operation *o, void *userdata)
{
	struct ext_data *d = userdata;
	if (d->read_cb)
		d->read_cb(o->context, NULL, 1, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation *pa_ext_device_manager_read(
        pa_context *c,
        pa_ext_device_manager_read_cb_t cb,
        void *userdata)
{
	pa_operation *o;
	struct ext_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	o = pa_operation_new(c, NULL, device_read, sizeof(struct ext_data));
	d = o->userdata;
	d->context = c;
	d->read_cb = cb;
	d->userdata = userdata;
	d->error = PA_ERR_NOTIMPLEMENTED;
	pa_operation_sync(o);

	return o;
}

static void on_success(pa_operation *o, void *userdata)
{
	struct ext_data *d = userdata;
	if (d->success_cb)
		d->success_cb(o->context, d->error, d->userdata);
	pa_operation_done(o);
}

SPA_EXPORT
pa_operation *pa_ext_device_manager_set_device_description(
        pa_context *c,
        const char* device,
        const char* description,
        pa_context_success_cb_t cb,
        void *userdata)
{
	pa_operation *o;
	struct ext_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	o = pa_operation_new(c, NULL, on_success, sizeof(struct ext_data));
	d = o->userdata;
	d->context = c;
	d->success_cb = cb;
	d->userdata = userdata;
	d->error = PA_ERR_NOTIMPLEMENTED;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation *pa_ext_device_manager_delete(
        pa_context *c,
        const char *const s[],
        pa_context_success_cb_t cb,
        void *userdata)
{
	pa_operation *o;
	struct ext_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	o = pa_operation_new(c, NULL, on_success, sizeof(struct ext_data));
	d = o->userdata;
	d->context = c;
	d->success_cb = cb;
	d->userdata = userdata;
	d->error = PA_ERR_NOTIMPLEMENTED;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation *pa_ext_device_manager_enable_role_device_priority_routing(
        pa_context *c,
        int enable,
        pa_context_success_cb_t cb,
        void *userdata)
{
	pa_operation *o;
	struct ext_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	o = pa_operation_new(c, NULL, on_success, sizeof(struct ext_data));
	d = o->userdata;
	d->context = c;
	d->success_cb = cb;
	d->userdata = userdata;
	d->error = PA_ERR_NOTIMPLEMENTED;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation *pa_ext_device_manager_reorder_devices_for_role(
        pa_context *c,
        const char* role,
        const char** devices,
        pa_context_success_cb_t cb,
        void *userdata)
{
	pa_operation *o;
	struct ext_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	o = pa_operation_new(c, NULL, on_success, sizeof(struct ext_data));
	d = o->userdata;
	d->context = c;
	d->success_cb = cb;
	d->userdata = userdata;
	d->error = PA_ERR_NOTIMPLEMENTED;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
pa_operation *pa_ext_device_manager_subscribe(
        pa_context *c,
        int enable,
        pa_context_success_cb_t cb,
        void *userdata)
{
	pa_operation *o;
	struct ext_data *d;

	pa_assert(c);
	pa_assert(c->refcount >= 1);

	PA_CHECK_VALIDITY_RETURN_NULL(c, c->state == PA_CONTEXT_READY, PA_ERR_BADSTATE);

	o = pa_operation_new(c, NULL, on_success, sizeof(struct ext_data));
	d = o->userdata;
	d->context = c;
	d->success_cb = cb;
	d->userdata = userdata;
	d->error = PA_ERR_NOTIMPLEMENTED;
	pa_operation_sync(o);

	return o;
}

SPA_EXPORT
void pa_ext_device_manager_set_subscribe_cb(
        pa_context *c,
        pa_ext_device_manager_subscribe_cb_t cb,
        void *userdata)
{
}
