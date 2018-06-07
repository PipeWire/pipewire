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
#include <pipewire/loop.h>

#include <pulse/mainloop.h>

#include "internal.h"

static void do_stop(void *data, uint64_t count)
{
        struct pa_mainloop *this = data;
        this->quit = true;
}

static enum spa_io map_flags_to_spa(pa_io_event_flags_t flags) {
	return (enum spa_io)
		((flags & PA_IO_EVENT_INPUT ? SPA_IO_IN : 0) |
		 (flags & PA_IO_EVENT_OUTPUT ? SPA_IO_OUT : 0) |
		 (flags & PA_IO_EVENT_ERROR ? SPA_IO_ERR : 0) |
		 (flags & PA_IO_EVENT_HANGUP ? SPA_IO_HUP : 0));
}

static pa_io_event_flags_t map_flags_from_spa(enum spa_io flags) {
	return (flags & SPA_IO_IN ? PA_IO_EVENT_INPUT : 0) |
		 (flags & SPA_IO_OUT ? PA_IO_EVENT_OUTPUT : 0) |
		 (flags & SPA_IO_ERR ? PA_IO_EVENT_ERROR : 0) |
		 (flags & SPA_IO_HUP ? PA_IO_EVENT_HANGUP : 0);
}

static void source_io_func(void *data, int fd, enum spa_io mask)
{
	pa_io_event *ev = data;
	if (ev->cb)
		ev->cb(&ev->mainloop->api, ev, ev->fd, map_flags_from_spa(mask), ev->userdata);
}

static pa_io_event* api_io_new(pa_mainloop_api*a, int fd, pa_io_event_flags_t events, pa_io_event_cb_t cb, void *userdata)
{
	pa_mainloop *mainloop = SPA_CONTAINER_OF(a, pa_mainloop, api);
	pa_io_event *ev;

	pa_assert(a);
	pa_assert(fd >= 0);
	pa_assert(cb);

	ev = calloc(1, sizeof(pa_io_event));
	ev->source = pw_loop_add_io(mainloop->loop, fd,
			map_flags_to_spa(events), false, source_io_func, ev);
	ev->fd = fd;
	ev->events = events;
	ev->mainloop = mainloop;
	ev->cb = cb;
	ev->userdata = userdata;

	return ev;
}

static void api_io_enable(pa_io_event* e, pa_io_event_flags_t events)
{
	pa_assert(e);

	if (e->events == events)
		return;

	e->events = events;
	pw_loop_update_io(e->mainloop->loop, e->source, map_flags_to_spa(events));
}

static void api_io_free(pa_io_event* e)
{
	pa_assert(e);
	pw_loop_destroy_source(e->mainloop->loop, e->source);
	if (e->destroy)
		e->destroy(&e->mainloop->api, e, e->userdata);
	free(e);
}

static void api_io_set_destroy(pa_io_event *e, pa_io_event_destroy_cb_t cb)
{
	pa_assert(e);
	e->destroy = cb;
}

static void source_timer_func(void *data, uint64_t expirations)
{
	pa_time_event *ev = data;
	struct timeval tv;
	if (ev->cb)
		ev->cb(&ev->mainloop->api, ev, &tv, ev->userdata);
}

static pa_time_event* api_time_new(pa_mainloop_api*a, const struct timeval *tv, pa_time_event_cb_t cb, void *userdata)
{
	pa_mainloop *mainloop = SPA_CONTAINER_OF(a, pa_mainloop, api);
	pa_time_event *ev;
	struct timespec ts;

	ev = calloc(1, sizeof(pa_time_event));
	ev->source = pw_loop_add_timer(mainloop->loop, source_timer_func, ev);
	ev->mainloop = mainloop;
	ev->cb = cb;
	ev->userdata = userdata;

	if (tv == NULL) {
		ts.tv_sec = 0;
		ts.tv_nsec = 1;
	}
	else {
		ts.tv_sec = tv->tv_sec;
		ts.tv_nsec = tv->tv_usec * 1000LL;
	}
	pw_loop_update_timer(mainloop->loop, ev->source, &ts, NULL, true);

	return ev;
}

static void api_time_restart(pa_time_event* e, const struct timeval *tv)
{
	struct timespec ts;

	pa_assert(e);

	if (tv == NULL) {
		ts.tv_sec = 0;
		ts.tv_nsec = 1;
	}
	else {
		ts.tv_sec = tv->tv_sec;
		ts.tv_nsec = tv->tv_usec * 1000LL;
	}
	pw_loop_update_timer(e->mainloop->loop, e->source, &ts, NULL, true);
}

static void api_time_free(pa_time_event* e)
{
	pa_assert(e);
	pw_loop_destroy_source(e->mainloop->loop, e->source);
	if (e->destroy)
		e->destroy(&e->mainloop->api, e, e->userdata);
	free(e);
}

static void api_time_set_destroy(pa_time_event *e, pa_time_event_destroy_cb_t cb)
{
	pa_assert(e);
	e->destroy = cb;
}

static void source_idle_func(void *data)
{
	pa_defer_event *ev = data;
	if (ev->cb)
		ev->cb(&ev->mainloop->api, ev, ev->userdata);
}

static pa_defer_event* api_defer_new(pa_mainloop_api*a, pa_defer_event_cb_t cb, void *userdata)
{
	pa_mainloop *mainloop = SPA_CONTAINER_OF(a, pa_mainloop, api);
	pa_defer_event *ev;

	pa_assert(a);
	pa_assert(cb);

	ev = calloc(1, sizeof(pa_defer_event));
	ev->source = pw_loop_add_idle(mainloop->loop, true, source_idle_func, ev);
	ev->mainloop = mainloop;
	ev->cb = cb;
	ev->userdata = userdata;

	return ev;
}

static void api_defer_enable(pa_defer_event* e, int b)
{
	pa_assert(e);
	pw_loop_enable_idle(e->mainloop->loop, e->source, b ? true : false);
}

static void api_defer_free(pa_defer_event* e)
{
	pa_assert(e);
	pw_loop_destroy_source(e->mainloop->loop, e->source);
	if (e->destroy)
		e->destroy(&e->mainloop->api, e, e->userdata);
	free(e);
}

static void api_defer_set_destroy(pa_defer_event *e, pa_defer_event_destroy_cb_t cb)
{
	pa_assert(e);
	e->destroy = cb;
}

static void api_quit(pa_mainloop_api*a, int retval)
{
	pa_mainloop *m = SPA_CONTAINER_OF(a, pa_mainloop, api);
	m->quit = true;
	m->retval = retval;
	pa_mainloop_wakeup(m);
}

static const pa_mainloop_api api =
{
	.io_new = api_io_new,
	.io_enable = api_io_enable,
	.io_free = api_io_free,
	.io_set_destroy = api_io_set_destroy,

	.time_new = api_time_new,
	.time_restart = api_time_restart,
	.time_free = api_time_free,
	.time_set_destroy = api_time_set_destroy,

	.defer_new = api_defer_new,
	.defer_enable = api_defer_enable,
	.defer_free = api_defer_free,
	.defer_set_destroy = api_defer_set_destroy,

	.quit = api_quit,
};

pa_mainloop *pa_mainloop_new(void)
{
	pa_mainloop *loop;

	loop = calloc(1, sizeof(pa_mainloop));
	if (loop == NULL)
		return NULL;

	loop->loop = pw_loop_new(NULL);
	if (loop->loop == NULL)
		goto no_loop;

	loop->event = pw_loop_add_event(loop->loop, do_stop, loop);
	loop->api = api;
	loop->api.userdata = loop->loop;

	return loop;

      no_loop:
	free(loop);
	return NULL;
}

void pa_mainloop_free(pa_mainloop* m)
{
	pw_loop_destroy(m->loop);
	free(m);
}

int pa_mainloop_prepare(pa_mainloop *m, int timeout)
{
	if (m->quit)
		return -2;
	m->timeout = timeout;
	m->n_events = -EIO;
	return 0;
}

/** Execute the previously prepared poll. Returns a negative value on error.*/
int pa_mainloop_poll(pa_mainloop *m)
{
	if (m->quit)
		return -2;

	return m->n_events = pw_loop_iterate(m->loop, m->timeout);
}

int pa_mainloop_dispatch(pa_mainloop *m)
{
	if (m->quit)
		return -2;

	return m->n_events;
}

int pa_mainloop_get_retval(pa_mainloop *m)
{
	return m->retval;
}

/** Run a single iteration of the main loop. This is a convenience function
for pa_mainloop_prepare(), pa_mainloop_poll() and pa_mainloop_dispatch().
Returns a negative value on error or exit request. If block is nonzero,
block for events if none are queued. Optionally return the return value as
specified with the main loop's quit() routine in the integer variable retval points
to. On success returns the number of sources dispatched in this iteration. */
int pa_mainloop_iterate(pa_mainloop *m, int block, int *retval)
{
	int r;
	pa_assert(m);

	if ((r = pa_mainloop_prepare(m, block ? -1 : 0)) < 0)
		goto quit;

	if ((r = pa_mainloop_poll(m)) < 0)
		goto quit;

	if ((r = pa_mainloop_dispatch(m)) < 0)
		goto quit;

	return r;

      quit:
	if ((r == -2) && retval)
		*retval = pa_mainloop_get_retval(m);
	return r;
}

int pa_mainloop_run(pa_mainloop *m, int *retval)
{
	int r;

	while ((r = pa_mainloop_iterate(m, 1, retval)) >= 0)
		;

	if (r == -2)
		return 1;
	else
		return -1;
}


pa_mainloop_api* pa_mainloop_get_api(pa_mainloop *m)
{
	pa_assert(m);
	return &m->api;
}

void pa_mainloop_quit(pa_mainloop *m, int retval)
{
	pa_assert(m);
	m->api.quit(&m->api, retval);
}

void pa_mainloop_wakeup(pa_mainloop *m)
{
	pa_assert(m);
	pw_loop_signal_event(m->loop, m->event);
}

void pa_mainloop_set_poll_func(pa_mainloop *m, pa_poll_func poll_func, void *userdata)
{
	pw_log_warn("Not Implemented");
}
