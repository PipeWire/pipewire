/* PipeWire
 *
 * Copyright © 2021 Wim Taymans
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

#include <pipewire/pipewire.h>

#include "avahi-poll.h"

struct impl {
	AvahiPoll api;
	struct pw_loop *loop;
};

struct AvahiWatch {
	struct impl *impl;
	struct spa_source *source;
	AvahiWatchEvent events;
	AvahiWatchCallback callback;
	void *userdata;
};

struct AvahiTimeout {
	struct impl *impl;
	struct spa_source *source;
	AvahiTimeoutCallback callback;
	void *userdata;
};

static AvahiWatchEvent from_pw_events(uint32_t mask)
{
	return (mask & SPA_IO_IN ? AVAHI_WATCH_IN : 0) |
		(mask & SPA_IO_OUT ? AVAHI_WATCH_OUT : 0) |
		(mask & SPA_IO_ERR ? AVAHI_WATCH_ERR : 0) |
		(mask & SPA_IO_HUP ? AVAHI_WATCH_HUP : 0);
}

static uint32_t to_pw_events(AvahiWatchEvent e) {
	return (e & AVAHI_WATCH_IN ? SPA_IO_IN : 0) |
		(e & AVAHI_WATCH_OUT ? SPA_IO_OUT : 0) |
		(e & AVAHI_WATCH_ERR ? SPA_IO_ERR : 0) |
		(e & AVAHI_WATCH_HUP ? SPA_IO_HUP : 0);
}

static void watch_callback(void *data, int fd, uint32_t mask)
{
	AvahiWatch *w = data;

	w->events = from_pw_events(mask);
	w->callback(w, fd, w->events, w->userdata);
	w->events = 0;
}

static AvahiWatch* watch_new(const AvahiPoll *api, int fd, AvahiWatchEvent event,
		AvahiWatchCallback callback, void *userdata)
{
	struct impl *impl = api->userdata;
	AvahiWatch *w;

	w = calloc(1, sizeof(*w));
	if (w == NULL)
		return NULL;

	w->impl = impl;
	w->events = 0;
	w->callback = callback;
	w->userdata = userdata;
	w->source = pw_loop_add_io(impl->loop, fd, to_pw_events(event),
			false, watch_callback, w);

	return w;
}

static void watch_update(AvahiWatch *w, AvahiWatchEvent event)
{
	struct impl *impl = w->impl;
	pw_loop_update_io(impl->loop, w->source, to_pw_events(event));
}

static AvahiWatchEvent watch_get_events(AvahiWatch *w)
{
	return w->events;
}

static void watch_free(AvahiWatch *w)
{
	struct impl *impl = w->impl;
	pw_loop_destroy_source(impl->loop, w->source);
	free(w);
}

static void timeout_callback(void *data, uint64_t expirations)
{
	AvahiTimeout *w = data;
	w->callback(w, w->userdata);
}

static AvahiTimeout* timeout_new(const AvahiPoll *api, const struct timeval *tv,
		AvahiTimeoutCallback callback, void *userdata)
{
	struct impl *impl = api->userdata;
	struct timespec value;
	AvahiTimeout *w;

	w = calloc(1, sizeof(*w));
	if (w == NULL)
		return NULL;

	w->impl = impl;
	w->callback = callback;
	w->userdata = userdata;
	w->source = pw_loop_add_timer(impl->loop, timeout_callback, w);

	if (tv != NULL) {
		value.tv_sec = tv->tv_sec;
		value.tv_nsec = tv->tv_usec * 1000UL;
		pw_loop_update_timer(impl->loop, w->source, &value, NULL, true);
	}
	return w;
}

static void timeout_update(AvahiTimeout *t, const struct timeval *tv)
{
	struct impl *impl = t->impl;
	struct timespec value, *v = NULL;

	if (tv != NULL) {
		value.tv_sec = tv->tv_sec;
		value.tv_nsec = tv->tv_usec * 1000UL;
		if (value.tv_sec == 0 && value.tv_nsec == 0)
			value.tv_nsec = 1;
		v = &value;
	}
	pw_loop_update_timer(impl->loop, t->source, v, NULL, true);
}

static void timeout_free(AvahiTimeout *t)
{
	struct impl *impl = t->impl;
	pw_loop_destroy_source(impl->loop, t->source);
	free(t);
}

static const AvahiPoll avahi_poll_api = {
	.watch_new = watch_new,
	.watch_update = watch_update,
	.watch_get_events = watch_get_events,
	.watch_free = watch_free,
	.timeout_new = timeout_new,
	.timeout_update = timeout_update,
	.timeout_free = timeout_free,
};

AvahiPoll* pw_avahi_poll_new(struct pw_loop *loop)
{
	struct impl *impl;

	impl = calloc(1, sizeof(*impl));
	if (impl == NULL)
		return NULL;

	impl->loop = loop;
	impl->api = avahi_poll_api;
	impl->api.userdata = impl;

	return &impl->api;
}

void pw_avahi_poll_free(AvahiPoll *p)
{
	struct impl *impl = SPA_CONTAINER_OF(p, struct impl, api);

	free(impl);
}
