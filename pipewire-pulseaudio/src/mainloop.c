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
#include <poll.h>

#include <pipewire/log.h>
#include <pipewire/loop.h>

#include <pulse/mainloop.h>
#include <pulse/xmalloc.h>
#include <pulse/timeval.h>

#include "internal.h"

static void do_stop(void *data, uint64_t count)
{
        struct pa_mainloop *this = data;
        this->quit = true;
}

static uint32_t map_flags_to_spa(pa_io_event_flags_t flags) {
	return (uint32_t)
		((flags & PA_IO_EVENT_INPUT ? SPA_IO_IN : 0) |
		 (flags & PA_IO_EVENT_OUTPUT ? SPA_IO_OUT : 0) |
		 (flags & PA_IO_EVENT_ERROR ? SPA_IO_ERR : 0) |
		 (flags & PA_IO_EVENT_HANGUP ? SPA_IO_HUP : 0));
}

static pa_io_event_flags_t map_flags_from_spa(uint32_t flags) {
	return (flags & SPA_IO_IN ? PA_IO_EVENT_INPUT : 0) |
		 (flags & SPA_IO_OUT ? PA_IO_EVENT_OUTPUT : 0) |
		 (flags & SPA_IO_ERR ? PA_IO_EVENT_ERROR : 0) |
		 (flags & SPA_IO_HUP ? PA_IO_EVENT_HANGUP : 0);
}

static void source_io_func(void *data, int fd, uint32_t mask)
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
	pw_log_debug("new io %p %p %08x", ev, ev->source, events);

	return ev;
}

static void api_io_enable(pa_io_event* e, pa_io_event_flags_t events)
{
	pa_assert(e);

	if (e->events == events || e->source == NULL)
		return;

	pw_log_debug("io %p", e);
	e->events = events;
	pw_loop_update_io(e->mainloop->loop, e->source, map_flags_to_spa(events));
}

static void api_io_free(pa_io_event* e)
{
	pa_assert(e);
	pw_log_debug("io %p", e);
	if (e->source)
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

static void set_timer(pa_time_event *ev, const struct timeval *tv)
{
	pa_mainloop *mainloop = ev->mainloop;
	struct timespec ts;

	if (tv == NULL) {
		ts.tv_sec = 0;
		ts.tv_nsec = 1;
	} else {
		struct timeval ttv = *tv;

		if ((ttv.tv_usec & PA_TIMEVAL_RTCLOCK) != 0)
			ttv.tv_usec &= ~PA_TIMEVAL_RTCLOCK;
		else
			pa_rtclock_from_wallclock(&ttv);

		/* something strange, probably not wallclock time, try to
		 * use the timeval directly */
		if (ttv.tv_sec == 0 && ttv.tv_usec == 0)
			ttv = *tv;

		ts.tv_sec = ttv.tv_sec;
		ts.tv_nsec = ttv.tv_usec * SPA_NSEC_PER_USEC;

		/* make sure we never disable the timer */
		if (ts.tv_sec == 0 && ts.tv_nsec == 0)
			ts.tv_nsec++;
	}
	pw_log_debug("set timer %p %ld %ld", ev, ts.tv_sec, ts.tv_nsec);
	pw_loop_update_timer(mainloop->loop, ev->source, &ts, NULL, true);
}

static pa_time_event* api_time_new(pa_mainloop_api*a, const struct timeval *tv, pa_time_event_cb_t cb, void *userdata)
{
	pa_mainloop *mainloop = SPA_CONTAINER_OF(a, pa_mainloop, api);
	pa_time_event *ev;

	ev = calloc(1, sizeof(pa_time_event));
	ev->source = pw_loop_add_timer(mainloop->loop, source_timer_func, ev);
	ev->mainloop = mainloop;
	ev->cb = cb;
	ev->userdata = userdata;
	pw_log_debug("new timer %p", ev);

	set_timer(ev, tv);

	return ev;
}

static void api_time_restart(pa_time_event* e, const struct timeval *tv)
{
	pa_assert(e);
	set_timer(e, tv);
}

static void api_time_free(pa_time_event* e)
{
	pa_assert(e);
	pw_log_debug("io %p", e);
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
	pw_log_debug("new defer %p", ev);

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
	pw_log_debug("io %p", e);
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

SPA_EXPORT
pa_mainloop *pa_mainloop_new(void)
{
	pa_mainloop *loop;

	if (getenv("PULSE_INTERNAL"))
		return NULL;

	loop = calloc(1, sizeof(pa_mainloop));
	if (loop == NULL)
		return NULL;

	loop->loop = pw_loop_new(NULL);
	if (loop->loop == NULL)
		goto no_loop;

	loop->fd = pw_loop_get_fd(loop->loop);
	loop->event = pw_loop_add_event(loop->loop, do_stop, loop);
	loop->api = api;
	loop->api.userdata = loop->loop;

	pw_log_debug("%p: %p fd:%d", loop, loop->loop, loop->fd);

	return loop;

      no_loop:
	free(loop);
	return NULL;
}

bool pa_mainloop_api_is_our_api(pa_mainloop_api *api)
{
	pa_assert(api);
	return api->io_new == api_io_new;
}

SPA_EXPORT
void pa_mainloop_free(pa_mainloop* m)
{
	pw_log_debug("%p", m);
	pw_loop_destroy(m->loop);
	free(m);
}

SPA_EXPORT
int pa_mainloop_prepare(pa_mainloop *m, int timeout)
{
	if (m->quit)
		return -2;
	m->timeout = timeout;
	m->n_events = -EIO;
	return 0;
}

static int usec_to_timeout(pa_usec_t u)
{
	if (u == PA_USEC_INVALID)
		return -1;
	return (u + PA_USEC_PER_MSEC - 1) / PA_USEC_PER_MSEC;
}

/** Execute the previously prepared poll. Returns a negative value on error.*/
SPA_EXPORT
int pa_mainloop_poll(pa_mainloop *m)
{
	int res, timeout;
	bool do_iterate;

	if (m->quit)
		return -2;

	if (m->poll_func) {
		struct pollfd fds[1];

		fds[0].fd = m->fd;
		fds[0].events = POLLIN;
		fds[0].revents = 0;

		res = m->poll_func(fds, 1,
				usec_to_timeout(m->timeout),
				m->poll_func_userdata);
		do_iterate = res == 1 && SPA_FLAG_IS_SET(fds[0].revents, POLLIN);
		timeout = 0;
	} else {
		do_iterate = true;
		timeout = m->timeout;
	}

	if (do_iterate) {
		pw_loop_enter(m->loop);
		do {
			res = pw_loop_iterate(m->loop, timeout);
		} while (res == -EINTR);
		pw_loop_leave(m->loop);
	}

	if (res < 0) {
		if (res == -EINTR)
			res = 0;
	}
	m->n_events = res;
	return res;
}

SPA_EXPORT
int pa_mainloop_dispatch(pa_mainloop *m)
{
	if (m->quit)
		return -2;

	return m->n_events;
}

SPA_EXPORT
int pa_mainloop_get_retval(PA_CONST pa_mainloop *m)
{
	return m->retval;
}

/** Run a single iteration of the main loop. This is a convenience function
for pa_mainloop_prepare(), pa_mainloop_poll() and pa_mainloop_dispatch().
Returns a negative value on error or exit request. If block is nonzero,
block for events if none are queued. Optionally return the return value as
specified with the main loop's quit() routine in the integer variable retval points
to. On success returns the number of sources dispatched in this iteration. */
SPA_EXPORT
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

SPA_EXPORT
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


SPA_EXPORT
pa_mainloop_api* pa_mainloop_get_api(pa_mainloop *m)
{
	pa_assert(m);
	return &m->api;
}

SPA_EXPORT
void pa_mainloop_quit(pa_mainloop *m, int retval)
{
	pa_assert(m);
	m->api.quit(&m->api, retval);
}

SPA_EXPORT
void pa_mainloop_wakeup(pa_mainloop *m)
{
	pa_assert(m);
	pw_loop_signal_event(m->loop, m->event);
}

SPA_EXPORT
void pa_mainloop_set_poll_func(pa_mainloop *m, pa_poll_func poll_func, void *userdata)
{
	pa_assert(m);
	m->poll_func = poll_func;
	m->poll_func_userdata = userdata;
}

struct once_info {
	void (*callback)(pa_mainloop_api*m, void *userdata);
	void *userdata;
};

static void once_callback(pa_mainloop_api *m, pa_defer_event *e, void *userdata) {
	struct once_info *i = userdata;

	pa_assert(m);
	pa_assert(i);

	pa_assert(i->callback);
	i->callback(m, i->userdata);

	pa_assert(m->defer_free);
	m->defer_free(e);
}

static void free_callback(pa_mainloop_api *m, pa_defer_event *e, void *userdata) {
	struct once_info *i = userdata;

	pa_assert(m);
	pa_assert(i);
	pa_xfree(i);
}


SPA_EXPORT
void pa_mainloop_api_once(pa_mainloop_api* m, void (*callback)(pa_mainloop_api *m, void *userdata), void *userdata) {
	struct once_info *i;
	pa_defer_event *e;

	pa_assert(m);
	pa_assert(callback);

	pa_init_i18n();

	i = pa_xnew(struct once_info, 1);
	i->callback = callback;
	i->userdata = userdata;

	pa_assert(m->defer_new);
	pa_assert_se(e = m->defer_new(m, once_callback, i));
	m->defer_set_destroy(e, free_callback);
}

