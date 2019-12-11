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

#include <spa/utils/list.h>

#include <pipewire/log.h>

#include <pulse/operation.h>

#include "internal.h"

pa_operation *pa_operation_new(pa_context *c, pa_stream *s, pa_operation_cb_t cb, size_t userdata_size) {
	pa_operation *o;
	pa_assert(c);

	o = calloc(1, sizeof(pa_operation) + userdata_size);

	o->refcount = 1;
	o->context = c;
	o->stream = s ? pa_stream_ref(s) : NULL;
	o->seq = SPA_ID_INVALID;

	o->state = PA_OPERATION_RUNNING;
	o->callback = cb;
	o->userdata = SPA_MEMBER(o, sizeof(pa_operation), void);

	spa_list_append(&c->operations, &o->link);
	pa_operation_ref(o);
	pw_log_debug("new %p", o);

	return o;
}

int pa_operation_sync(pa_operation *o)
{
	pa_context *c = o->context;
	o->seq = pw_core_sync(c->core, 0, 0);
	pw_log_debug("operation %p: sync %d", o, o->seq);
	return 0;
}

SPA_EXPORT
pa_operation *pa_operation_ref(pa_operation *o)
{
	pa_assert(o);
	pa_assert(o->refcount >= 1);
	o->refcount++;
	return o;
}

static void operation_free(pa_operation *o)
{
        pa_assert(!o->context);
        pa_assert(!o->stream);
	pw_log_debug("%p %d", o, o->seq);
	free(o);
}

static void operation_unlink(pa_operation *o) {
	pa_assert(o);

	pw_log_debug("%p %d", o, o->seq);
	if (o->context) {
		pa_assert(o->refcount >= 2);

		spa_list_remove(&o->link);
		pa_operation_unref(o);

		o->context = NULL;
	}
	if (o->stream)
		pa_stream_unref(o->stream);
	o->stream = NULL;
	o->callback = NULL;
	o->userdata = NULL;
	o->state_callback = NULL;
	o->state_userdata = NULL;
}


SPA_EXPORT
void pa_operation_unref(pa_operation *o)
{
	pa_assert(o);
	pa_assert(o->refcount >= 1);
	if (--o->refcount == 0)
		operation_free(o);
}

static void operation_set_state(pa_operation *o, pa_operation_state_t st) {
	pa_assert(o);
	pa_assert(o->refcount >= 1);

	if (st == o->state)
		return;

	pa_operation_ref(o);

	pw_log_debug("new state %p %d %d", o, o->seq, st);
	o->state = st;

	if (o->state_callback)
		o->state_callback(o, o->state_userdata);

	if ((o->state == PA_OPERATION_DONE) || (o->state == PA_OPERATION_CANCELED))
		operation_unlink(o);

	pa_operation_unref(o);
}


SPA_EXPORT
void pa_operation_cancel(pa_operation *o)
{
	pa_assert(o);
	pa_assert(o->refcount >= 1);
	pw_log_debug("%p %d", o, o->seq);
	operation_set_state(o, PA_OPERATION_CANCELED);
}

void pa_operation_done(pa_operation *o) {
	pa_assert(o);
	pa_assert(o->refcount >= 1);
	operation_set_state(o, PA_OPERATION_DONE);
}


SPA_EXPORT
pa_operation_state_t pa_operation_get_state(PA_CONST pa_operation *o)
{
	pa_assert(o);
	pa_assert(o->refcount >= 1);
	return o->state;
}

SPA_EXPORT
void pa_operation_set_state_callback(pa_operation *o, pa_operation_notify_cb_t cb, void *userdata)
{
	pa_assert(o);
	pa_assert(o->refcount >= 1);

	if (o->state == PA_OPERATION_DONE || o->state == PA_OPERATION_CANCELED)
		return;

	o->state_callback = cb;
	o->state_userdata = userdata;
}
