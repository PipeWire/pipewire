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
#include <spa/support/loop.h>

#include <pipewire/log.h>
#include <pipewire/loop.h>

#include <pulse/mainloop-signal.h>

#include "internal.h"


static pa_mainloop_api *api = NULL;
static bool have_signals = false;
static struct spa_list signals;
static struct pw_loop *loop = NULL;

struct pa_signal_event {
	struct spa_list link;
	struct spa_source *source;
	pa_signal_cb_t callback;
	pa_signal_destroy_cb_t destroy;
	void *userdata;
};

SPA_EXPORT
int pa_signal_init(pa_mainloop_api *a)
{
	pa_assert(a);
	pa_assert(!api);

	api = a;
	spa_list_init(&signals);
	loop = a->userdata;

	return 0;
}

SPA_EXPORT
void pa_signal_done(void)
{
	pa_signal_event *ev;

	if (have_signals) {
		spa_list_consume(ev, &signals, link)
			pa_signal_free(ev);
	}
	api = NULL;
}

static void source_signal_func (void *data, int signal_number)
{
	pa_signal_event *ev = data;
	if (ev->callback)
		ev->callback(api, ev, signal_number, ev->userdata);
}

SPA_EXPORT
pa_signal_event* pa_signal_new(int sig, pa_signal_cb_t callback, void *userdata)
{
	pa_signal_event *ev;

	pa_assert(sig > 0);
	pa_assert(callback);

	ev = calloc(1, sizeof(pa_signal_event));
	ev->source = spa_loop_utils_add_signal(loop->utils, sig, source_signal_func, ev);
	ev->callback = callback;
	ev->userdata = userdata;

	if (!have_signals)
		spa_list_init(&signals);
	have_signals = true;
	spa_list_append(&signals, &ev->link);

	return ev;
}

SPA_EXPORT
void pa_signal_free(pa_signal_event *e)
{
	pa_assert(e);

	spa_list_remove(&e->link);
	spa_loop_utils_destroy_source(loop->utils, e->source);
	if (e->destroy)
		e->destroy(api, e, e->userdata);
	free(e);
}

SPA_EXPORT
void pa_signal_set_destroy(pa_signal_event *e, pa_signal_destroy_cb_t callback)
{
	pa_assert(e);
	e->destroy = callback;
}
